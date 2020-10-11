/*
 * Copyright (c) 2006, 2018 Oracle and/or its affiliates. All rights reserved.
 */
#include <linux/kernel.h>
#include <linux/in.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/ratelimit.h>
#include <net/addrconf.h>

#include "ib.h"

static void rdpma_ib_cq_event_handler(struct ib_event *event, void *data)
{
	pr_info("event %u (%s) data %p\n",
		 event->event, ib_event_msg(event->event), data);
}

/* Plucking the oldest entry from the ring can be done concurrently with
 * the thread refilling the ring.  Each ring operation is protected by
 * spinlocks and the transient state of refilling doesn't change the
 * recording of which entry is oldest.
 *
 * This relies on IB only calling one cq comp_handler for each cq so that
 * there will only be one caller of rdpma_recv_incoming() per rdpma connection.
 */
static void rdpma_ib_cq_comp_handler_recv(struct ib_cq *cq, void *context)
{
	struct client_context *ctx = context;
	struct rdpma_ib_connection *ic = ctx->ic;

//	rdpmadebug("conn %p cq %p\n", conn, cq);

	tasklet_schedule(&ic->i_recv_tasklet);
}

static void poll_scq(struct rdpma_ib_connection *ic, struct ib_cq *cq,
		     struct ib_wc *wcs)
{
	int nr, i;
	struct ib_wc *wc;

	while ((nr = ib_poll_cq(cq, RDPMA_IB_WC_MAX, wcs)) > 0) {
		for (i = 0; i < nr; i++) {
			wc = wcs + i;
			rdpmadebug("wc wr_id 0x%llx status %u byte_len %u imm_data %u\n",
				 (unsigned long long)wc->wr_id, wc->status,
				 wc->byte_len, be32_to_cpu(wc->ex.imm_data));

//			if (wc->wr_id <= ic->i_send_ring.w_nr ||
//			    wc->wr_id == RDPMA_IB_ACK_WR_ID)
			rdpma_ib_send_cqe_handler(ic, wc);
//			else
//				rdpma_ib_mr_cqe_handler(ic, wc);

		}
	}
}

static void rdpma_ib_tasklet_fn_send(unsigned long data)
{
	struct rdpma_ib_connection *ic = (struct rdpma_ib_connection *)data;
//	struct rdpma_connection *conn = ic->conn;

//	rdpma_ib_stats_inc(s_ib_tasklet_call);

	/* if cq has been already reaped, ignore incoming cq event */
	if (atomic_read(&ic->i_cq_quiesce))
		return;

	poll_scq(ic, ic->i_send_cq, ic->i_send_wc);
	ib_req_notify_cq(ic->i_send_cq, IB_CQ_NEXT_COMP);
	poll_scq(ic, ic->i_send_cq, ic->i_send_wc);
}

static void poll_rcq(struct rdpma_ib_connection *ic, struct ib_cq *cq,
		     struct ib_wc *wcs,
		     struct rdpma_ib_ack_state *ack_state)
{
	int nr, i;
	struct ib_wc *wc;

	while ((nr = ib_poll_cq(cq, RDPMA_IB_WC_MAX, wcs)) > 0) {
		for (i = 0; i < nr; i++) {
			wc = wcs + i;
			rdpmadebug("wc wr_id 0x%llx status %u byte_len %u imm_data %u\n",
				 (unsigned long long)wc->wr_id, wc->status,
				 wc->byte_len, be32_to_cpu(wc->ex.imm_data));

			rdpma_ib_recv_cqe_handler(ic, wc, ack_state);
		}
	}
}

static void rdpma_ib_tasklet_fn_recv(unsigned long data)
{
	struct rdpma_ib_connection *ic = (struct rdpma_ib_connection *)data;
	struct rdpma_ib_device *rdpma_ibdev = ic->rdpma_ibdev;
	struct rdpma_ib_ack_state state;

#if 0
	if (!rdpma_ibdev)
		rdpma_conn_drop(conn);

//	rdpma_ib_stats_inc(s_ib_tasklet_call);

#endif
	/* if cq has been already reaped, ignore incoming cq event */
	if (atomic_read(&ic->i_cq_quiesce))
		return;

	memset(&state, 0, sizeof(state));
	poll_rcq(ic, ic->i_recv_cq, ic->i_recv_wc, &state);
//	ib_req_notify_cq(ic->i_recv_cq, IB_CQ_SOLICITED);
	ib_req_notify_cq(ic->i_recv_cq, IB_CQ_NEXT_COMP);
	poll_rcq(ic, ic->i_recv_cq, ic->i_recv_wc, &state);

#if 0
	if (state.ack_next_valid)
		rdpma_ib_set_ack(ic, state.ack_next, state.ack_required);
	if (state.ack_recv_valid && state.ack_recv > ic->i_ack_recv) {
		rdpma_send_drop_acked(conn, state.ack_recv, NULL);
		ic->i_ack_recv = state.ack_recv;
	}
#endif
}

static void rdpma_ib_qp_event_handler(struct ib_event *event, void *data)
{
	struct client_context *ctx = data;
	struct rdpma_ib_connection *ic = ctx->ic;

	rdpmadebug("conn %p ic %p event %u (%s)\n", ctx, ic, event->event,
		 ib_event_msg(event->event));

	switch (event->event) {
	case IB_EVENT_COMM_EST:
		rdma_notify(ic->i_cm_id, IB_EVENT_COMM_EST);
		break;
	default:
		rdpmadebug("Fatal QP Event %u (%s) reconnecting\n",
			 event->event, ib_event_msg(event->event));
		break;
	}
}

static void rdpma_ib_cq_comp_handler_send(struct ib_cq *cq, void *context)
{
	struct client_context *ctx = context;
	struct rdpma_ib_connection *ic = ctx->ic;

	tasklet_schedule(&ic->i_send_tasklet);
}


static inline int ibdev_get_unused_vector(struct rdpma_ib_device *rdpma_ibdev)
{
	int min = rdpma_ibdev->vector_load[rdpma_ibdev->dev->num_comp_vectors - 1];
	int index = rdpma_ibdev->dev->num_comp_vectors - 1;
	int i;

	for (i = rdpma_ibdev->dev->num_comp_vectors - 1; i >= 0; i--) {
		if (rdpma_ibdev->vector_load[i] < min) {
			index = i;
			min = rdpma_ibdev->vector_load[i];
		}
	}

	rdpma_ibdev->vector_load[index]++;
	return index;
}

static inline void ibdev_put_vector(struct rdpma_ib_device *rdpma_ibdev, int index)
{
	rdpma_ibdev->vector_load[index]--;
}

/*
 * This needs to be very careful to not leave IS_ERR pointers around for
 * cleanup to trip over.
 */
int rdpma_ib_setup_qp(struct client_context *ctx)
{
	struct rdpma_ib_connection *ic = ctx->ic;
//	struct ib_device *dev = ic->i_cm_id->device;
	struct ib_device *dev = ic->dev;
	struct ib_qp_init_attr attr;
	struct ib_cq_init_attr cq_attr = {};
	struct rdpma_ib_device *rdpma_ibdev;
	int ret = 0, fr_queue_space;

	/*
	 * It's normal to see a null device if an incoming connection races
	 * with device removal, so we don't print a warning.
	 */
	rdpma_ibdev = rdpma_ib_get_client_data(dev);
	if (!rdpma_ibdev)
		return -EOPNOTSUPP;

	/* The fr_queue_space is currently set to 512, to add extra space on
	 * completion queue and send queue. This extra space is used for FRMR
	 * registration and invalidation work requests
	 */
	fr_queue_space = (rdpma_ibdev->use_fastreg ? RDPMA_IB_DEFAULT_FR_WR : 0);

	/* add the conn now so that connection establishment has the dev */
//	rdpma_ib_add_conn(rdpma_ibdev, conn);
	ic->rdpma_ibdev = rdpma_ibdev;

#if 0
	if (rdpma_ibdev->max_wrs < ic->i_send_ring.w_nr + 1)
		rdpma_ib_ring_resize(&ic->i_send_ring, rdpma_ibdev->max_wrs - 1);
	if (rdpma_ibdev->max_wrs < ic->i_recv_ring.w_nr + 1)
		rdpma_ib_ring_resize(&ic->i_recv_ring, rdpma_ibdev->max_wrs - 1);
#endif

	/* Protection domain and memory range */
	ic->i_pd = rdpma_ibdev->pd;

	ic->i_scq_vector = ibdev_get_unused_vector(rdpma_ibdev);
//	cq_attr.cqe = ic->i_send_ring.w_nr + fr_queue_space + 1;
	cq_attr.cqe = fr_queue_space + 1;
	cq_attr.comp_vector = ic->i_scq_vector;
	ic->i_send_cq = ib_create_cq(dev, rdpma_ib_cq_comp_handler_send,
				     rdpma_ib_cq_event_handler, ctx,
				     &cq_attr);
	if (IS_ERR(ic->i_send_cq)) {
		ret = PTR_ERR(ic->i_send_cq);
		ic->i_send_cq = NULL;
		ibdev_put_vector(rdpma_ibdev, ic->i_scq_vector);
		rdpmadebug("ib_create_cq send failed: %d\n", ret);
		goto rdpma_ibdev_out;
	}

	ic->i_rcq_vector = ibdev_get_unused_vector(rdpma_ibdev);
//	cq_attr.cqe = ic->i_recv_ring.w_nr;
	cq_attr.cqe = 1 << 13;
	cq_attr.comp_vector = ic->i_rcq_vector;
	ic->i_recv_cq = ib_create_cq(dev, rdpma_ib_cq_comp_handler_recv,
				     rdpma_ib_cq_event_handler, ctx,
				     &cq_attr);
	if (IS_ERR(ic->i_recv_cq)) {
		ret = PTR_ERR(ic->i_recv_cq);
		ic->i_recv_cq = NULL;
		ibdev_put_vector(rdpma_ibdev, ic->i_rcq_vector);
		rdpmadebug("ib_create_cq recv failed: %d\n", ret);
		goto send_cq_out;
	}

	ret = ib_req_notify_cq(ic->i_send_cq, IB_CQ_NEXT_COMP);
	if (ret) {
		rdpmadebug("ib_req_notify_cq send failed: %d\n", ret);
		goto recv_cq_out;
	}

//	ret = ib_req_notify_cq(ic->i_recv_cq, IB_CQ_SOLICITED);
	ret = ib_req_notify_cq(ic->i_recv_cq, IB_CQ_NEXT_COMP);
	if (ret) {
		rdpmadebug("ib_req_notify_cq recv failed: %d\n", ret);
		goto recv_cq_out;
	}

	/* XXX negotiate max send/recv with remote? */
	memset(&attr, 0, sizeof(attr));
	ctx->depth = 64;
	attr.event_handler = rdpma_ib_qp_event_handler;
	attr.qp_context = ctx;
	/* + 1 to allow for the single ack message */
//	attr.cap.max_send_wr = ic->i_send_ring.w_nr + fr_queue_space + 1;
//	attr.cap.max_recv_wr = ic->i_recv_ring.w_nr + 1;
	attr.cap.max_send_wr = ctx->depth;
	attr.cap.max_recv_wr = ctx->depth;
	attr.cap.max_send_sge = rdpma_ibdev->max_sge;
	attr.cap.max_recv_sge = RDPMA_IB_RECV_SGE;
	attr.sq_sig_type = IB_SIGNAL_REQ_WR;
	attr.qp_type = IB_QPT_RC;
	attr.send_cq = ic->i_send_cq;
	attr.recv_cq = ic->i_recv_cq;

	/*
	 * XXX this can fail if max_*_wr is too large?  Are we supposed
	 * to back off until we get a value that the hardware can support?
	 */
//	ret = rdma_create_qp(ic->i_cm_id, ic->i_pd, &attr);
	ctx->qp = ib_create_qp(ic->i_pd, &attr);
	if (ret) {
		rdpmadebug("ib_create_qp failed: %d\n", ret);
		goto recv_cq_out;
	}

	/*
	ic->i_send_hdrs = ib_dma_alloc_coherent(dev,
					   ic->i_send_ring.w_nr *
						sizeof(struct rdpma_header),
					   &ic->i_send_hdrs_dma, GFP_KERNEL);
	if (!ic->i_send_hdrs) {
		ret = -ENOMEM;
		rdpmadebug("ib_dma_alloc_coherent send failed\n");
		goto qp_out;
	}

	ic->i_recv_hdrs = ib_dma_alloc_coherent(dev,
					   ic->i_recv_ring.w_nr *
						sizeof(struct rdpma_header),
					   &ic->i_recv_hdrs_dma, GFP_KERNEL);
	if (!ic->i_recv_hdrs) {
		ret = -ENOMEM;
		rdpmadebug("ib_dma_alloc_coherent recv failed\n");
		goto send_hdrs_dma_out;
	}

	ic->i_ack = ib_dma_alloc_coherent(dev, sizeof(struct rdpma_header),
				       &ic->i_ack_dma, GFP_KERNEL);
	if (!ic->i_ack) {
		ret = -ENOMEM;
		rdpmadebug("ib_dma_alloc_coherent ack failed\n");
		goto recv_hdrs_dma_out;
	}

	ic->i_sends = vzalloc_node(array_size(sizeof(struct rdpma_ib_send_work),
					      ic->i_send_ring.w_nr),
				   ibdev_to_node(dev));
	if (!ic->i_sends) {
		ret = -ENOMEM;
		rdpmadebug("send allocation failed\n");
		goto ack_dma_out;
	}

	ic->i_recvs = vzalloc_node(array_size(sizeof(struct rdpma_ib_recv_work),
					      ic->i_recv_ring.w_nr),
				   ibdev_to_node(dev));
	if (!ic->i_recvs) {
		ret = -ENOMEM;
		rdpmadebug("recv allocation failed\n");
		goto sends_out;
	}

	rdpma_ib_recv_init_ack(ic);
	*/

	rdpmadebug("ctx %p pd %p cq %p %p\n", ctx, ic->i_pd,
		 ic->i_send_cq, ic->i_recv_cq);

	goto out;

#if 0
sends_out:
	vfree(ic->i_sends);
ack_dma_out:
	ib_dma_free_coherent(dev, sizeof(struct rdpma_header),
			     ic->i_ack, ic->i_ack_dma);
recv_hdrs_dma_out:
	ib_dma_free_coherent(dev, ic->i_recv_ring.w_nr *
					sizeof(struct rdpma_header),
					ic->i_recv_hdrs, ic->i_recv_hdrs_dma);
send_hdrs_dma_out:
	ib_dma_free_coherent(dev, ic->i_send_ring.w_nr *
					sizeof(struct rdpma_header),
					ic->i_send_hdrs, ic->i_send_hdrs_dma);
qp_out:
	rdma_destroy_qp(ic->i_cm_id);
#endif
recv_cq_out:
	ib_destroy_cq(ic->i_recv_cq);
	ic->i_recv_cq = NULL;
send_cq_out:
	ib_destroy_cq(ic->i_send_cq);
	ic->i_send_cq = NULL;
rdpma_ibdev_out:
//	rdpma_ib_remove_conn(rdpma_ibdev, conn);
out:
	rdpma_ib_dev_put(rdpma_ibdev);

	return ret;
}


int rdpma_ib_conn_alloc(struct client_context *ctx, gfp_t gfp)
{
	struct rdpma_ib_connection *ic;
	unsigned long flags;
	int ret;

	/* XXX too lazy? */
	ic = kzalloc(sizeof(struct rdpma_ib_connection), gfp);
	if (!ic)
		return -ENOMEM;

	/*
	ret = rdpma_ib_recv_alloc_caches(ic, gfp);
	if (ret) {
		kfree(ic);
		return ret;
	}
	*/

	INIT_LIST_HEAD(&ic->ib_node);
	tasklet_init(&ic->i_send_tasklet, rdpma_ib_tasklet_fn_send,
		     (unsigned long)ic);
	tasklet_init(&ic->i_recv_tasklet, rdpma_ib_tasklet_fn_recv,
		     (unsigned long)ic);
	mutex_init(&ic->i_recv_mutex);
#if 0
#ifndef KERNEL_HAS_ATOMIC64
	spin_lock_init(&ic->i_ack_lock);
#endif
#endif
	atomic_set(&ic->i_signaled_sends, 0);
	atomic_set(&ic->i_fastreg_wrs, RDPMA_IB_DEFAULT_FR_WR);

#if 0
	spin_lock_irqsave(&ib_nodev_conns_lock, flags);
	list_add_tail(&ic->ib_node, &ib_nodev_conns);
	spin_unlock_irqrestore(&ib_nodev_conns_lock, flags);
#endif

	ic->dev = ibdev;

	ctx->ic = ic;

	return 0;
}

/*
 * Free a connection. Connection must be shut down and not set for reconnect.
 */
void rdpma_ib_conn_free(void *arg)
{
	struct rdpma_ib_connection *ic = arg;
	spinlock_t	*lock_ptr;

	rdpmadebug("ic %p\n", ic);

	/*
	 * Conn is either on a dev's list or on the nodev list.
	 * A race with shutdown() or connect() would cause problems
	 * (since rdpma_ibdev would change) but that should never happen.
	 */
//	lock_ptr = ic->rdpma_ibdev ? &ic->rdpma_ibdev->spinlock : &ib_nodev_conns_lock;
	lock_ptr = &ic->rdpma_ibdev->spinlock;

	spin_lock_irq(lock_ptr);
	list_del(&ic->ib_node);
	spin_unlock_irq(lock_ptr);

//	rdpma_ib_recv_free_caches(ic);

	kfree(ic);
}
