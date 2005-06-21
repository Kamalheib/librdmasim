/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies Ltd.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * $Id$
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdlib.h>
#include <netinet/in.h>
#include <pthread.h>

#include "mthca.h"
#include "doorbell.h"

enum {
	MTHCA_SEND_DOORBELL	= 0x10,
        MTHCA_RECV_DOORBELL	= 0x18
};

enum {
	MTHCA_NEXT_DBD       = 1 << 7,
	MTHCA_NEXT_FENCE     = 1 << 6,
	MTHCA_NEXT_CQ_UPDATE = 1 << 3,
	MTHCA_NEXT_EVENT_GEN = 1 << 2,
	MTHCA_NEXT_SOLICIT   = 1 << 1,
};

enum {
	MTHCA_INVAL_LKEY = 0x100
};

enum {
	MTHCA_INLINE_SEG = 1 << 31
};

struct mthca_next_seg {
	uint32_t	nda_op;	/* [31:6] next WQE [4:0] next opcode */
	uint32_t	ee_nds;	/* [31:8] next EE  [7] DBD [6] F [5:0] next WQE size */
	uint32_t	flags;	/* [3] CQ [2] Event [1] Solicit */
	uint32_t	imm;	/* immediate data */
};

struct mthca_tavor_ud_seg {
	uint32_t	reserved1;
	uint32_t	lkey;
	uint64_t	av_addr;
	uint32_t	reserved2[4];
	uint32_t	dqpn;
	uint32_t	qkey;
	uint32_t	reserved3[2];
};

struct mthca_arbel_ud_seg {
	uint32_t	av[8];
	uint32_t	dqpn;
	uint32_t	qkey;
	uint32_t	reserved[2];
};

struct mthca_bind_seg {
	uint32_t	flags;	/* [31] Atomic [30] rem write [29] rem read */
	uint32_t	reserved;
	uint32_t	new_rkey;
	uint32_t	lkey;
	uint64_t	addr;
	uint64_t	length;
};

struct mthca_raddr_seg {
	uint64_t	raddr;
	uint32_t	rkey;
	uint32_t	reserved;
};

struct mthca_atomic_seg {
	uint64_t	swap_add;
	uint64_t	compare;
};

struct mthca_data_seg {
	uint32_t	byte_count;
	uint32_t	lkey;
	uint64_t	addr;
};

struct mthca_inline_seg {
	uint32_t	byte_count;
};

static const uint8_t mthca_opcode[] = {
	[IBV_WR_SEND]                 = MTHCA_OPCODE_SEND,
	[IBV_WR_SEND_WITH_IMM]        = MTHCA_OPCODE_SEND_IMM,
	[IBV_WR_RDMA_WRITE]           = MTHCA_OPCODE_RDMA_WRITE,
	[IBV_WR_RDMA_WRITE_WITH_IMM]  = MTHCA_OPCODE_RDMA_WRITE_IMM,
	[IBV_WR_RDMA_READ]            = MTHCA_OPCODE_RDMA_READ,
	[IBV_WR_ATOMIC_CMP_AND_SWP]   = MTHCA_OPCODE_ATOMIC_CS,
	[IBV_WR_ATOMIC_FETCH_AND_ADD] = MTHCA_OPCODE_ATOMIC_FA,
};

static void *get_recv_wqe(struct mthca_qp *qp, int n)
{
	return qp->buf + (n << qp->rq.wqe_shift);
}

static void *get_send_wqe(struct mthca_qp *qp, int n)
{
	return qp->buf + qp->send_wqe_offset + (n << qp->sq.wqe_shift);
}

static inline int wq_overflow(struct mthca_wq *wq, int nreq, struct mthca_cq *cq)
{
	unsigned cur;

	cur = wq->head - wq->tail;
	if (cur + nreq < wq->max)
		return 0;

	pthread_spin_lock(&cq->lock);
	cur = wq->head - wq->tail;
	pthread_spin_lock(&cq->lock);

	return cur + nreq >= wq->max;
}

int mthca_tavor_post_send(struct ibv_qp *ibqp, struct ibv_send_wr *wr,
			  struct ibv_send_wr **bad_wr)
{
	struct mthca_qp *qp = to_mqp(ibqp);
	void *wqe, *prev_wqe;
	int ind;
	int nreq;
	int ret = 0;
	int size, size0 = 0;
	int i;
	uint32_t f0 = 0, op0 = 0;

	pthread_spin_lock(&qp->sq.lock);

	ind = qp->sq.next_ind;

	for (nreq = 0; wr; ++nreq, wr = wr->next) {
		if (wq_overflow(&qp->sq, nreq, to_mcq(qp->ibv_qp.send_cq))) {
			ret = -1;
			*bad_wr = wr;
			goto out;
		}

		wqe = get_send_wqe(qp, ind);
		prev_wqe = qp->sq.last;
		qp->sq.last = wqe;

		((struct mthca_next_seg *) wqe)->nda_op = 0;
		((struct mthca_next_seg *) wqe)->ee_nds = 0;
		((struct mthca_next_seg *) wqe)->flags =
			((wr->send_flags & IBV_SEND_SIGNALED) ?
			 htonl(MTHCA_NEXT_CQ_UPDATE) : 0) |
			((wr->send_flags & IBV_SEND_SOLICITED) ?
			 htonl(MTHCA_NEXT_SOLICIT) : 0)   |
			htonl(1);
		if (wr->opcode == IBV_WR_SEND_WITH_IMM ||
		    wr->opcode == IBV_WR_RDMA_WRITE_WITH_IMM)
			((struct mthca_next_seg *) wqe)->imm = wr->imm_data;

		wqe += sizeof (struct mthca_next_seg);
		size = sizeof (struct mthca_next_seg) / 16;

		switch (qp->qpt) {
		case IBV_QPT_RC:
			switch (wr->opcode) {
			case IBV_WR_ATOMIC_CMP_AND_SWP:
			case IBV_WR_ATOMIC_FETCH_AND_ADD:
				((struct mthca_raddr_seg *) wqe)->raddr =
					htonll(wr->wr.atomic.remote_addr);
				((struct mthca_raddr_seg *) wqe)->rkey =
					htonl(wr->wr.atomic.rkey);
				((struct mthca_raddr_seg *) wqe)->reserved = 0;

				wqe += sizeof (struct mthca_raddr_seg);

				if (wr->opcode == IBV_WR_ATOMIC_CMP_AND_SWP) {
					((struct mthca_atomic_seg *) wqe)->swap_add =
						htonll(wr->wr.atomic.swap);
					((struct mthca_atomic_seg *) wqe)->compare =
						htonll(wr->wr.atomic.compare_add);
				} else {
					((struct mthca_atomic_seg *) wqe)->swap_add =
						htonll(wr->wr.atomic.compare_add);
					((struct mthca_atomic_seg *) wqe)->compare = 0;
				}

				wqe += sizeof (struct mthca_atomic_seg);
				size += sizeof (struct mthca_raddr_seg) / 16 +
					sizeof (struct mthca_atomic_seg);
				break;

			case IBV_WR_RDMA_WRITE:
			case IBV_WR_RDMA_WRITE_WITH_IMM:
			case IBV_WR_RDMA_READ:
				((struct mthca_raddr_seg *) wqe)->raddr =
					htonll(wr->wr.rdma.remote_addr);
				((struct mthca_raddr_seg *) wqe)->rkey =
					htonl(wr->wr.rdma.rkey);
				((struct mthca_raddr_seg *) wqe)->reserved = 0;
				wqe += sizeof (struct mthca_raddr_seg);
				size += sizeof (struct mthca_raddr_seg) / 16;
				break;

			default:
				/* No extra segments required for sends */
				break;
			}

			break;

		case IBV_QPT_UC:
			switch (wr->opcode) {
			case IBV_WR_RDMA_WRITE:
			case IBV_WR_RDMA_WRITE_WITH_IMM:
				((struct mthca_raddr_seg *) wqe)->raddr =
					htonll(wr->wr.rdma.remote_addr);
				((struct mthca_raddr_seg *) wqe)->rkey =
					htonl(wr->wr.rdma.rkey);
				((struct mthca_raddr_seg *) wqe)->reserved = 0;
				wqe += sizeof (struct mthca_raddr_seg);
				size += sizeof (struct mthca_raddr_seg) / 16;
				break;

			default:
				/* No extra segments required for sends */
				break;
			}

			break;

		case IBV_QPT_UD:
			((struct mthca_tavor_ud_seg *) wqe)->lkey =
				htonl(to_mah(wr->wr.ud.ah)->key);
			((struct mthca_tavor_ud_seg *) wqe)->av_addr =
				htonll((uintptr_t) to_mah(wr->wr.ud.ah)->av);
			((struct mthca_tavor_ud_seg *) wqe)->dqpn =
				htonl(wr->wr.ud.remote_qpn);
			((struct mthca_tavor_ud_seg *) wqe)->qkey =
				htonl(wr->wr.ud.remote_qkey);

			wqe += sizeof (struct mthca_tavor_ud_seg);
			size += sizeof (struct mthca_tavor_ud_seg) / 16;
			break;

		default:
			break;
		}

		if (wr->num_sge > qp->sq.max_gs) {
			ret = -1;
			*bad_wr = wr;
			goto out;
		}

		if (wr->send_flags & IBV_SEND_INLINE) {
			struct mthca_inline_seg *seg = wqe;
			int max_size = (1 << qp->sq.wqe_shift) - sizeof *seg - size * 16;
			int s = 0;

			wqe += sizeof *seg;
			for (i = 0; i < wr->num_sge; ++i) {
				struct ibv_sge *sge = &wr->sg_list[i];

				s += sge->length;

				if (s > max_size) {
					ret = -1;
					*bad_wr = wr;
					goto out;
				}

				memcpy(wqe, (void*) (intptr_t) sge->addr, sge->length);
				wqe += sge->length;
			}

			seg->byte_count = htonl(MTHCA_INLINE_SEG | s);
			size += align(s + sizeof *seg, 16) / 16;
		} else {
			struct mthca_data_seg *seg;

			for (i = 0; i < wr->num_sge; ++i) {
				seg = wqe;
				seg->byte_count = htonl(wr->sg_list[i].length);
				seg->lkey = htonl(wr->sg_list[i].lkey);
				seg->addr = htonll(wr->sg_list[i].addr);
				wqe += sizeof *seg;
			}

			size += wr->num_sge * (sizeof *seg / 16);
		}

		qp->wrid[ind + qp->rq.max] = wr->wr_id;

		if (wr->opcode >= sizeof mthca_opcode / sizeof mthca_opcode[0]) {
			ret = -1;
			*bad_wr = wr;
			goto out;
		}

		if (prev_wqe) {
			((struct mthca_next_seg *) prev_wqe)->nda_op =
				htonl(((ind << qp->sq.wqe_shift) +
				       qp->send_wqe_offset) |
				      mthca_opcode[wr->opcode]);

			((struct mthca_next_seg *) prev_wqe)->ee_nds =
				htonl((size0 ? 0 : MTHCA_NEXT_DBD) | size);
		}

		if (!size0) {
			size0 = size;
			op0   = mthca_opcode[wr->opcode];
		}

		++ind;
		if (ind >= qp->sq.max)
			ind -= qp->sq.max;
	}

out:
	if (nreq) {
		uint32_t doorbell[2];

		doorbell[0] = htonl(((qp->sq.next_ind << qp->sq.wqe_shift) +
				     qp->send_wqe_offset) | f0 | op0);
		doorbell[1] = htonl((ibqp->qp_num << 8) | size0);

		mthca_write64(doorbell, to_mctx(ibqp->context), MTHCA_SEND_DOORBELL);
	}

	qp->sq.next_ind = ind;
	qp->sq.head    += nreq;

	pthread_spin_unlock(&qp->sq.lock);
	return ret;
}

int mthca_tavor_post_recv(struct ibv_qp *ibqp, struct ibv_recv_wr *wr,
			  struct ibv_recv_wr **bad_wr)
{
	struct mthca_qp *qp = to_mqp(ibqp);
	int ret = 0;
	int nreq;
	int i;
	int size;
	int size0 = 0;
	int ind;
	void *wqe;
	void *prev_wqe;

	pthread_spin_lock(&qp->rq.lock);

	ind = qp->rq.next_ind;

	for (nreq = 0; wr; ++nreq, wr = wr->next) {
		if (wq_overflow(&qp->rq, nreq, to_mcq(qp->ibv_qp.send_cq))) {
			ret = -1;
			*bad_wr = wr;
			goto out;
		}

		wqe = get_recv_wqe(qp, ind);
		prev_wqe = qp->rq.last;
		qp->rq.last = wqe;

		((struct mthca_next_seg *) wqe)->nda_op = 0;
		((struct mthca_next_seg *) wqe)->ee_nds =
			htonl(MTHCA_NEXT_DBD);
		((struct mthca_next_seg *) wqe)->flags =
			htonl(MTHCA_NEXT_CQ_UPDATE);

		wqe += sizeof (struct mthca_next_seg);
		size = sizeof (struct mthca_next_seg) / 16;

		if (wr->num_sge > qp->rq.max_gs) {
			ret = -1;
			*bad_wr = wr;
			goto out;
		}

		for (i = 0; i < wr->num_sge; ++i) {
			((struct mthca_data_seg *) wqe)->byte_count =
				htonl(wr->sg_list[i].length);
			((struct mthca_data_seg *) wqe)->lkey =
				htonl(wr->sg_list[i].lkey);
			((struct mthca_data_seg *) wqe)->addr =
				htonll(wr->sg_list[i].addr);
			wqe += sizeof (struct mthca_data_seg);
			size += sizeof (struct mthca_data_seg) / 16;
		}

		qp->wrid[ind] = wr->wr_id;

		if (prev_wqe) {
			((struct mthca_next_seg *) prev_wqe)->nda_op =
				htonl((ind << qp->rq.wqe_shift) | 1);
			((struct mthca_next_seg *) prev_wqe)->ee_nds =
				htonl(MTHCA_NEXT_DBD | size);
		}

		if (!size0)
			size0 = size;

		++ind;
		if (ind >= qp->rq.max)
			ind -= qp->rq.max;
	}

out:
	if (nreq) {
		uint32_t doorbell[2];

		doorbell[0] = htonl((qp->rq.next_ind << qp->rq.wqe_shift) | size0);
		doorbell[1] = htonl((ibqp->qp_num << 8) | nreq);

		mthca_write64(doorbell, to_mctx(ibqp->context), MTHCA_RECV_DOORBELL);
	}

	qp->rq.next_ind = ind;
	qp->rq.head    += nreq;

	pthread_spin_unlock(&qp->rq.lock);
	return ret;
}

int mthca_arbel_post_send(struct ibv_qp *ibqp, struct ibv_send_wr *wr,
			  struct ibv_send_wr **bad_wr)
{
	struct mthca_qp *qp = to_mqp(ibqp);
	void *wqe, *prev_wqe;
	int ind;
	int nreq;
	int ret = 0;
	int size, size0 = 0;
	int i;
	uint32_t f0 = 0, op0 = 0;

	pthread_spin_lock(&qp->sq.lock);

	/* XXX check that state is OK to post send */

	ind = qp->sq.head & (qp->sq.max - 1);

	for (nreq = 0; wr; ++nreq, wr = wr->next) {
		if (wq_overflow(&qp->sq, nreq, to_mcq(qp->ibv_qp.send_cq))) {
			ret = -1;
			*bad_wr = wr;
			goto out;
		}

		wqe = get_send_wqe(qp, ind);
		prev_wqe = qp->sq.last;
		qp->sq.last = wqe;

		((struct mthca_next_seg *) wqe)->flags =
			((wr->send_flags & IBV_SEND_SIGNALED) ?
			 htonl(MTHCA_NEXT_CQ_UPDATE) : 0) |
			((wr->send_flags & IBV_SEND_SOLICITED) ?
			 htonl(MTHCA_NEXT_SOLICIT) : 0)   |
			htonl(1);
		if (wr->opcode == IBV_WR_SEND_WITH_IMM ||
		    wr->opcode == IBV_WR_RDMA_WRITE_WITH_IMM)
			((struct mthca_next_seg *) wqe)->imm = wr->imm_data;

		wqe += sizeof (struct mthca_next_seg);
		size = sizeof (struct mthca_next_seg) / 16;

		switch (qp->qpt) {
		case IBV_QPT_RC:
			switch (wr->opcode) {
			case IBV_WR_ATOMIC_CMP_AND_SWP:
			case IBV_WR_ATOMIC_FETCH_AND_ADD:
				((struct mthca_raddr_seg *) wqe)->raddr =
					htonll(wr->wr.atomic.remote_addr);
				((struct mthca_raddr_seg *) wqe)->rkey =
					htonl(wr->wr.atomic.rkey);
				((struct mthca_raddr_seg *) wqe)->reserved = 0;

				wqe += sizeof (struct mthca_raddr_seg);

				if (wr->opcode == IBV_WR_ATOMIC_CMP_AND_SWP) {
					((struct mthca_atomic_seg *) wqe)->swap_add =
						htonll(wr->wr.atomic.swap);
					((struct mthca_atomic_seg *) wqe)->compare =
						htonll(wr->wr.atomic.compare_add);
				} else {
					((struct mthca_atomic_seg *) wqe)->swap_add =
						htonll(wr->wr.atomic.compare_add);
					((struct mthca_atomic_seg *) wqe)->compare = 0;
				}

				wqe += sizeof (struct mthca_atomic_seg);
				size += sizeof (struct mthca_raddr_seg) / 16 +
					sizeof (struct mthca_atomic_seg);
				break;

			case IBV_WR_RDMA_WRITE:
			case IBV_WR_RDMA_WRITE_WITH_IMM:
			case IBV_WR_RDMA_READ:
				((struct mthca_raddr_seg *) wqe)->raddr =
					htonll(wr->wr.rdma.remote_addr);
				((struct mthca_raddr_seg *) wqe)->rkey =
					htonl(wr->wr.rdma.rkey);
				((struct mthca_raddr_seg *) wqe)->reserved = 0;
				wqe += sizeof (struct mthca_raddr_seg);
				size += sizeof (struct mthca_raddr_seg) / 16;
				break;

			default:
				/* No extra segments required for sends */
				break;
			}

			break;

		case IBV_QPT_UC:
			switch (wr->opcode) {
			case IBV_WR_RDMA_WRITE:
			case IBV_WR_RDMA_WRITE_WITH_IMM:
			case IBV_WR_RDMA_READ:
				((struct mthca_raddr_seg *) wqe)->raddr =
					htonll(wr->wr.rdma.remote_addr);
				((struct mthca_raddr_seg *) wqe)->rkey =
					htonl(wr->wr.rdma.rkey);
				((struct mthca_raddr_seg *) wqe)->reserved = 0;
				wqe += sizeof (struct mthca_raddr_seg);
				size += sizeof (struct mthca_raddr_seg) / 16;
				break;

			default:
				/* No extra segments required for sends */
				break;
			}

			break;

		case IBV_QPT_UD:
			memcpy(((struct mthca_arbel_ud_seg *) wqe)->av,
			       to_mah(wr->wr.ud.ah)->av, sizeof (struct mthca_av));
			((struct mthca_arbel_ud_seg *) wqe)->dqpn =
				htonl(wr->wr.ud.remote_qpn);
			((struct mthca_arbel_ud_seg *) wqe)->qkey =
				htonl(wr->wr.ud.remote_qkey);

			wqe += sizeof (struct mthca_arbel_ud_seg);
			size += sizeof (struct mthca_arbel_ud_seg) / 16;
			break;

		default:
			break;
		}

		if (wr->num_sge > qp->sq.max_gs) {
			ret = -1;
			*bad_wr = wr;
			goto out;
		}

		if (wr->send_flags & IBV_SEND_INLINE) {
			struct mthca_inline_seg *seg = wqe;
			int max_size = (1 << qp->sq.wqe_shift) - sizeof *seg - size * 16;
			int s = 0;

			wqe += sizeof *seg;
			for (i = 0; i < wr->num_sge; ++i) {
				struct ibv_sge *sge = &wr->sg_list[i];

				s += sge->length;

				if (s > max_size) {
					ret = -1;
					*bad_wr = wr;
					goto out;
				}

				memcpy(wqe, (void*) (uintptr_t) sge->addr, sge->length);
				wqe += sge->length;
			}

			seg->byte_count = htonl(MTHCA_INLINE_SEG | s);
			size += align(s + sizeof *seg, 16) / 16;
		} else {
			struct mthca_data_seg *seg;

			for (i = 0; i < wr->num_sge; ++i) {
				seg = wqe;
				seg->byte_count = htonl(wr->sg_list[i].length);
				seg->lkey = htonl(wr->sg_list[i].lkey);
				seg->addr = htonll(wr->sg_list[i].addr);
				wqe += sizeof *seg;
			}

			size += wr->num_sge * (sizeof *seg / 16);
		}

		qp->wrid[ind + qp->rq.max] = wr->wr_id;

		if (wr->opcode >= sizeof mthca_opcode / sizeof mthca_opcode[0]) {
			ret = -1;
			*bad_wr = wr;
			goto out;
		}

		if (prev_wqe) {
			((struct mthca_next_seg *) prev_wqe)->nda_op =
				htonl(((ind << qp->sq.wqe_shift) +
				       qp->send_wqe_offset) |
				      mthca_opcode[wr->opcode]);
			mb();
			((struct mthca_next_seg *) prev_wqe)->ee_nds =
				htonl(MTHCA_NEXT_DBD | size);
		}

		if (!size0) {
			size0 = size;
			op0   = mthca_opcode[wr->opcode];
		}

		++ind;
		if (ind >= qp->sq.max)
			ind -= qp->sq.max;
	}

out:
	if (nreq) {
		uint32_t doorbell[2];

		doorbell[0] = htonl((nreq << 24)                  |
				    ((qp->sq.head & 0xffff) << 8) |
				    f0 | op0);
		doorbell[1] = htonl((ibqp->qp_num << 8) | size0);

		qp->sq.head += nreq;

		/*
		 * Make sure that descriptors are written before
		 * doorbell record.
		 */
		mb();
		*qp->sq.db = htonl(qp->sq.head & 0xffff);

		/*
		 * Make sure doorbell record is written before we
		 * write MMIO send doorbell.
		 */
		mb();
		mthca_write64(doorbell, to_mctx(ibqp->context), MTHCA_SEND_DOORBELL);
	}

	pthread_spin_unlock(&qp->sq.lock);
	return ret;
}

int mthca_arbel_post_recv(struct ibv_qp *ibqp, struct ibv_recv_wr *wr,
			  struct ibv_recv_wr **bad_wr)
{
	struct mthca_qp *qp = to_mqp(ibqp);
	int ret = 0;
	int nreq;
	int ind;
	int i;
	void *wqe;

 	pthread_spin_lock(&qp->rq.lock);

	/* XXX check that state is OK to post receive */

	ind = qp->rq.head & (qp->rq.max - 1);

	for (nreq = 0; wr; ++nreq, wr = wr->next) {
		if (wq_overflow(&qp->rq, nreq, to_mcq(qp->ibv_qp.send_cq))) {
			ret = -1;
			*bad_wr = wr;
			goto out;
		}

		wqe = get_recv_wqe(qp, ind);

		((struct mthca_next_seg *) wqe)->flags = 0;

		wqe += sizeof (struct mthca_next_seg);

		if (wr->num_sge > qp->rq.max_gs) {
			ret = -1;
			*bad_wr = wr;
			goto out;
		}

		for (i = 0; i < wr->num_sge; ++i) {
			((struct mthca_data_seg *) wqe)->byte_count =
				htonl(wr->sg_list[i].length);
			((struct mthca_data_seg *) wqe)->lkey =
				htonl(wr->sg_list[i].lkey);
			((struct mthca_data_seg *) wqe)->addr =
				htonll(wr->sg_list[i].addr);
			wqe += sizeof (struct mthca_data_seg);
		}

		if (i < qp->rq.max_gs) {
			((struct mthca_data_seg *) wqe)->byte_count = 0;
			((struct mthca_data_seg *) wqe)->lkey = htonl(MTHCA_INVAL_LKEY);
			((struct mthca_data_seg *) wqe)->addr = 0;
		}

		qp->wrid[ind] = wr->wr_id;

		++ind;
		if (ind >= qp->rq.max)
			ind -= qp->rq.max;
	}
out:
	if (nreq) {
		qp->rq.head += nreq;

		/*
		 * Make sure that descriptors are written before
		 * doorbell record.
		 */
		mb();
		*qp->rq.db = htonl(qp->rq.head & 0xffff);
	}

	pthread_spin_unlock(&qp->rq.lock);
	return ret;
}

int mthca_alloc_qp_buf(struct ibv_pd *pd, struct ibv_qp_cap *cap,
		       struct mthca_qp *qp)
{
	int size;

	qp->rq.max_gs 	 = cap->max_recv_sge;
	qp->sq.max_gs 	 = align(cap->max_inline_data + sizeof (struct mthca_inline_seg),
				 sizeof (struct mthca_data_seg)) / sizeof (struct mthca_data_seg);
	if (qp->sq.max_gs < cap->max_send_sge)
		qp->sq.max_gs = cap->max_send_sge;

	qp->wrid = malloc((qp->rq.max + qp->sq.max) * sizeof (uint64_t));
	if (!qp->wrid)
		return -1;

	size = sizeof (struct mthca_next_seg) +
		qp->rq.max_gs * sizeof (struct mthca_data_seg);

	for (qp->rq.wqe_shift = 6; 1 << qp->rq.wqe_shift < size;
	     qp->rq.wqe_shift++)
		; /* nothing */

	size = sizeof (struct mthca_next_seg) +
		qp->sq.max_gs * sizeof (struct mthca_data_seg);
	switch (qp->qpt) {
	case IBV_QPT_UD:
		if (mthca_is_memfree(pd->context))
			size += sizeof (struct mthca_arbel_ud_seg);
		else
			size += sizeof (struct mthca_tavor_ud_seg);
		break;
	default:
		/* bind seg is as big as atomic + raddr segs */
		size += sizeof (struct mthca_bind_seg);
	}

	for (qp->sq.wqe_shift = 6; 1 << qp->sq.wqe_shift < size;
	     qp->sq.wqe_shift++)
		; /* nothing */

	qp->send_wqe_offset = align(qp->rq.max << qp->rq.wqe_shift,
				    1 << qp->sq.wqe_shift);

	qp->buf_size = qp->send_wqe_offset + (qp->sq.max << qp->sq.wqe_shift);

	if (posix_memalign(&qp->buf, to_mdev(pd->context->device)->page_size,
			   align(qp->buf_size, to_mdev(pd->context->device)->page_size))) {
		free(qp->wrid);
		return -1;
	}

	memset(qp->buf, 0, qp->buf_size);

	if (mthca_is_memfree(pd->context)) {
		struct mthca_next_seg *next;
		struct mthca_data_seg *scatter;
		int i;
		uint32_t sz;

		sz = htonl((sizeof (struct mthca_next_seg) +
			    qp->rq.max_gs * sizeof (struct mthca_data_seg)) / 16);

		for (i = 0; i < qp->rq.max; ++i) {
			next = get_recv_wqe(qp, i);
			next->nda_op = htonl(((i + 1) & (qp->rq.max - 1)) <<
					     qp->rq.wqe_shift);
			next->ee_nds = sz;

			for (scatter = (void *) (next + 1);
			     (void *) scatter < (void *) next + (1 << qp->rq.wqe_shift);
			     ++scatter)
				scatter->lkey = htonl(MTHCA_INVAL_LKEY);
		}

		for (i = 0; i < qp->sq.max; ++i) {
			next = get_send_wqe(qp, i);
			next->nda_op = htonl((((i + 1) & (qp->sq.max - 1)) <<
					      qp->sq.wqe_shift) +
					     qp->send_wqe_offset);
		}
	}

	return 0;
}

void mthca_return_cap(struct ibv_pd *pd, struct mthca_qp *qp, struct ibv_qp_cap *cap)
{
	/*
	 * Maximum inline data size is the full WQE size less the size
	 * of the next segment, inline segment and other non-data segments.
	 */
	cap->max_inline_data = (1 << qp->sq.wqe_shift) -
		sizeof (struct mthca_next_seg) -
		sizeof (struct mthca_inline_seg);

	switch (qp->qpt) {
	case IBV_QPT_UD:
		if (mthca_is_memfree(pd->context))
			cap->max_inline_data -= sizeof (struct mthca_arbel_ud_seg);
		else
			cap->max_inline_data -= sizeof (struct mthca_tavor_ud_seg);
		break;

	default:
		/*
		 * inline data won't be used in the same WQE as an
		 * atomic or bind segment, so we don't have to
		 * subtract anything off here.
		 */
		break;
	}

	cap->max_send_wr     = qp->sq.max;
	cap->max_recv_wr     = qp->rq.max;
	cap->max_send_sge    = qp->sq.max_gs;
	cap->max_recv_sge    = qp->rq.max_gs;
}

struct mthca_qp *mthca_find_qp(struct mthca_context *ctx, uint32_t qpn)
{
	int tind = (qpn & (ctx->num_qps - 1)) >> ctx->qp_table_shift;

	if (ctx->qp_table[tind].refcnt)
		return ctx->qp_table[tind].table[qpn & ctx->qp_table_mask];
	else
		return NULL;
}

int mthca_store_qp(struct mthca_context *ctx, uint32_t qpn, struct mthca_qp *qp)
{
	int tind = (qpn & (ctx->num_qps - 1)) >> ctx->qp_table_shift;
	int ret = 0;

	pthread_mutex_lock(&ctx->qp_table_mutex);

	if (!ctx->qp_table[tind].refcnt++) {
		ctx->qp_table[tind].table = calloc(ctx->qp_table_mask + 1,
						   sizeof (struct mthca_qp *));
		if (!ctx->qp_table[tind].table) {
			ret = -1;
			goto out;
		}
	}

	ctx->qp_table[tind].table[qpn & ctx->qp_table_mask] = qp;

out:
	pthread_mutex_unlock(&ctx->qp_table_mutex);
	return ret;
}

void mthca_clear_qp(struct mthca_context *ctx, uint32_t qpn)
{
	int tind = (qpn & (ctx->num_qps - 1)) >> ctx->qp_table_shift;

	pthread_mutex_lock(&ctx->qp_table_mutex);

	if (!--ctx->qp_table[tind].refcnt)
		free(ctx->qp_table[tind].table);
	else
		ctx->qp_table[tind].table[qpn & ctx->qp_table_mask] = NULL;
	
	pthread_mutex_unlock(&ctx->qp_table_mutex);
}

int mthca_free_err_wqe(struct mthca_qp *qp, int is_send,
		       int index, int *dbd, uint32_t *new_wqe)
{
	struct mthca_next_seg *next;

	if (is_send)
		next = get_send_wqe(qp, index);
	else
		next = get_recv_wqe(qp, index);

	if (mthca_is_memfree(qp->ibv_qp.context))
		*dbd = 1;
	else
		*dbd = !!(next->ee_nds & htonl(MTHCA_NEXT_DBD));
	if (next->ee_nds & htonl(0x3f))
		*new_wqe = (next->nda_op & htonl(~0x3f)) |
			(next->ee_nds & htonl(0x3f));
	else
		*new_wqe = 0;

	return 0;
}

