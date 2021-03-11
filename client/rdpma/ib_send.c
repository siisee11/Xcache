/*
 * Copyright (c) 2006, 2017 Oracle and/or its affiliates. All rights reserved.
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
 */
#include <linux/kernel.h>
#include <linux/in.h>
#include <linux/device.h>
#include <linux/dmapool.h>
#include <linux/ratelimit.h>

#include "rdpma.h"
#include "ib.h"

#if 0
/*
 * Convert IB-specific error message to rdpma error message and call core
 * completion handler.
 */
static void rdpma_ib_send_complete(struct rdpma_message *rm,
				 int wc_status,
				 void (*complete)(struct rdpma_message *rm, int status))
{
	int notify_status;

	switch (wc_status) {
	case IB_WC_WR_FLUSH_ERR:
		return;

	case IB_WC_SUCCESS:
		notify_status = RDPMA_RDMA_SUCCESS;
		break;

	case IB_WC_REM_ACCESS_ERR:
		notify_status = RDPMA_RDMA_REMOTE_ERROR;
		break;

	default:
		notify_status = RDPMA_RDMA_OTHER_ERROR;
		break;
	}
	complete(rm, notify_status);
}

static void rdpma_ib_send_unmap_data(struct rdpma_ib_connection *ic,
				   struct rm_data_op *op,
				   int wc_status)
{
	if (op->op_nents)
		ib_dma_unmap_sg(ic->i_cm_id->device,
				op->op_sg, op->op_nents,
				DMA_TO_DEVICE);
}

static void rdpma_ib_send_unmap_rdma(struct rdpma_ib_connection *ic,
				   struct rm_rdma_op *op,
				   int wc_status)
{
	if (op->op_mapped) {
		ib_dma_unmap_sg(ic->i_cm_id->device,
				op->op_sg, op->op_nents,
				op->op_write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
		op->op_mapped = 0;
	}

	/* If the user asked for a completion notification on this
	 * message, we can implement three different semantics:
	 *  1.	Notify when we received the ACK on the rdpma message
	 *	that was queued with the RDMA. This provides reliable
	 *	notification of RDMA status at the expense of a one-way
	 *	packet delay.
	 *  2.	Notify when the IB stack gives us the completion event for
	 *	the RDMA operation.
	 *  3.	Notify when the IB stack gives us the completion event for
	 *	the accompanying rdpma messages.
	 * Here, we implement approach #3. To implement approach #2,
	 * we would need to take an event for the rdma WR. To implement #1,
	 * don't call rdpma_rdma_send_complete at all, and fall back to the notify
	 * handling in the ACK processing code.
	 *
	 * Note: There's no need to explicitly sync any RDMA buffers using
	 * ib_dma_sync_sg_for_cpu - the completion for the RDMA
	 * operation itself unmapped the RDMA buffers, which takes care
	 * of synching.
	 */
	rdpma_ib_send_complete(container_of(op, struct rdpma_message, rdma),
			     wc_status, rdpma_rdma_send_complete);

	if (op->op_write)
		rdpma_stats_add(s_send_rdma_bytes, op->op_bytes);
	else
		rdpma_stats_add(s_recv_rdma_bytes, op->op_bytes);
}

/*
 * Unmap the resources associated with a struct send_work.
 *
 * Returns the rm for no good reason other than it is unobtainable
 * other than by switching on wr.opcode, currently, and the caller,
 * the event handler, needs it.
 */
static struct rdpma_message *rdpma_ib_send_unmap_op(struct rdpma_ib_connection *ic,
						struct rdpma_ib_send_work *send,
						int wc_status)
{
	struct rdpma_message *rm = NULL;

	/* In the error case, wc.opcode sometimes contains garbage */
	switch (send->s_wr.opcode) {
	case IB_WR_SEND:
		if (send->s_op) {
			rm = container_of(send->s_op, struct rdpma_message, data);
			rdpma_ib_send_unmap_data(ic, send->s_op, wc_status);
		}
		break;
	case IB_WR_RDMA_WRITE:
	case IB_WR_RDMA_READ:
		if (send->s_op) {
			rm = container_of(send->s_op, struct rdpma_message, rdma);
			rdpma_ib_send_unmap_rdma(ic, send->s_op, wc_status);
		}
		break;
	case IB_WR_ATOMIC_FETCH_AND_ADD:
	case IB_WR_ATOMIC_CMP_AND_SWP:
		if (send->s_op) {
			rm = container_of(send->s_op, struct rdpma_message, atomic);
			rdpma_ib_send_unmap_atomic(ic, send->s_op, wc_status);
		}
		break;
	default:
		printk_ratelimited(KERN_NOTICE
			       "rdpma/IB: %s: unexpected opcode 0x%x in WR!\n",
			       __func__, send->s_wr.opcode);
		break;
	}

	send->s_wr.opcode = 0xdead;

	return rm;
}
#endif

/*
 * The _oldest/_free ring operations here race cleanly with the alloc/unalloc
 * operations performed in the send path.  As the sender allocs and potentially
 * unallocs the next free entry in the ring it doesn't alter which is
 * the next to be freed, which is what this is concerned with.
 */
void rdpma_ib_send_cqe_handler(struct rdpma_ib_connection *ic, struct ib_wc *wc)
{
	struct rdpma_message *rm = NULL;
	struct rdpma_ib_send_work *send;
	u32 completed;
	u32 oldest;
	u32 i = 0;
	int nr_sig = 0;

	pr_info("[%s] wc wr_id 0x%llx status %u (%s) byte_len %u imm_data %u\n",
			__func__,
		 (unsigned long long)wc->wr_id, wc->status,
		 ib_wc_status_msg(wc->status), wc->byte_len,
		 be32_to_cpu(wc->ex.imm_data));
//	rdpma_ib_stats_inc(s_ib_tx_cq_event);

	/* We expect errors as the qp is drained during shutdown */
	if (wc->status != IB_WC_SUCCESS) {
		pr_err("send completion had status %u (%s), disconnecting and reconnecting\n",
				  wc->status, ib_wc_status_msg(wc->status));
	}
}
