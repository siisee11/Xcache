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
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <rdma/rdma_cm.h>

#include "rdpma.h"
#include "ib.h"

void rdpma_ib_recv_cqe_handler(struct rdpma_ib_connection *ic,
			     struct ib_wc *wc,
			     struct rdpma_ib_ack_state *state)
{
	struct rdpma_ib_recv_work *recv;

	pr_info("[%s] wc wr_id 0x%llx status %u (%s) byte_len %u imm_data %u\n",
		 __func__, 
		 (unsigned long long)wc->wr_id, wc->status,
		 ib_wc_status_msg(wc->status), wc->byte_len,
		 be32_to_cpu(wc->ex.imm_data));

//	rdpma_ib_stats_inc(s_ib_rx_cq_event);
//	recv = &ic->i_recvs[rdpma_ib_ring_oldest(&ic->i_recv_ring)];
//	ib_dma_unmap_sg(ic->i_cm_id->device, &recv->r_frag->f_sg, 1,
//			DMA_FROM_DEVICE);

	/* Also process recvs in connecting state because it is possible
	 * to get a recv completion _before_ the rdmacm ESTABLISHED
	 * event is processed.
	 */
	if (wc->status == IB_WC_SUCCESS) {
		pr_info("[%s]: IB_WC_SUCCESS!!\n", __func__);
	} else {
		/* We expect errors as the qp is drained during shutdown */
		pr_err("recv completion had status %u (%s), disconnecting and reconnecting\n",
				  wc->status, ib_wc_status_msg(wc->status));
	}

	rdpma_post_recv();

	if((int)wc->opcode == IB_WC_RECV_RDMA_WITH_IMM){
		int node_id, pid, type, tx_state;
		uint32_t num;
		bit_unmask(ntohl(wc->ex.imm_data), &node_id, &pid, &type, &tx_state, &num);
		pr_info("[%s]: node_id(%d), pid(%d), type(%d), tx_state(%d), num(%d)\n", __func__, node_id, pid, type, tx_state, num);
		if(type == MSG_WRITE_REQUEST_REPLY){
			struct request_struct* new_request = kmem_cache_alloc(request_cache, GFP_KERNEL);
			dprintk("[%s]: received MSG_WRITE_REQUEST_REPLY\n", __func__);
			new_request->type = MSG_WRITE;
			new_request->pid = pid;
			new_request->num = num;

			spin_lock(&ctx->lock);
			list_add_tail(&new_request->entry, &ctx->req_list);
			wake_up(&ctx->req_wq);
			spin_unlock(&ctx->lock);
		}
		else if(type == MSG_WRITE_REPLY){
			dprintk("[%s]: received MSG_WRITE_REPLY\n", __func__);
			unset_bit(pid);
			/* TODO: need to distinguish committed or aborted? */
		}
		else if(type == MSG_READ_REQUEST_REPLY){
			dprintk("[%s]: received MSG_READ_REQUEST_REPLY\n", __func__);
			if(tx_state == TX_READ_READY){
				struct request_struct* new_request = kmem_cache_alloc(request_cache, GFP_KERNEL);
				new_request->type = MSG_READ;
				new_request->pid = pid;
				new_request->num = num;

				spin_lock(&ctx->lock);
				list_add_tail(&new_request->entry, &ctx->req_list);
				wake_up(&ctx->req_wq);
				spin_unlock(&ctx->lock);
			}
			else{
				dprintk("[%s]: remote server aborted read request\n", __func__);
				ctx->process_state[pid] = PROCESS_STATE_ABORT;
				/*
				   unset_bit(pid);*/
			}
		}
		else{
			printk(KERN_ALERT "[%s]: received weired type msg from remote server (%d)\n", __func__, type);
		}
	}
	else{
		printk(KERN_ALERT "[%s]: received weired opcode from remote server (%d)\n", __func__, (int)wc->opcode);
	}
}
