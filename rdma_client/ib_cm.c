/*
 * Copyright (c) 2006, 2018 Oracle and/or its affiliates. All rights reserved.
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
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/ratelimit.h>
#include <net/addrconf.h>

static void pmdfc_ib_cq_event_handler(struct ib_event *event, void *data)
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
 * there will only be one caller of pmdfc_recv_incoming() per pmdfc connection.
 */
static void pmdfc_ib_cq_comp_handler_recv(struct ib_cq *cq, void *context)
{
	struct pmdfc_connection *conn = context;
	struct pmdfc_ib_connection *ic = conn->c_transport_data;

	pmdfcdebug("conn %p cq %p\n", conn, cq);

	pmdfc_ib_stats_inc(s_ib_evt_handler_call);

	tasklet_schedule(&ic->i_recv_tasklet);
}

static void poll_scq(struct pmdfc_ib_connection *ic, struct ib_cq *cq,
		     struct ib_wc *wcs)
{
	int nr, i;
	struct ib_wc *wc;

	while ((nr = ib_poll_cq(cq, pmdfc_IB_WC_MAX, wcs)) > 0) {
		for (i = 0; i < nr; i++) {
			wc = wcs + i;
			pmdfcdebug("wc wr_id 0x%llx status %u byte_len %u imm_data %u\n",
				 (unsigned long long)wc->wr_id, wc->status,
				 wc->byte_len, be32_to_cpu(wc->ex.imm_data));

			if (wc->wr_id <= ic->i_send_ring.w_nr ||
			    wc->wr_id == pmdfc_IB_ACK_WR_ID)
				pmdfc_ib_send_cqe_handler(ic, wc);
			else
				pmdfc_ib_mr_cqe_handler(ic, wc);

		}
	}
}

static void pmdfc_ib_tasklet_fn_send(unsigned long data)
{
	struct pmdfc_ib_connection *ic = (struct pmdfc_ib_connection *)data;
	struct pmdfc_connection *conn = ic->conn;

	pmdfc_ib_stats_inc(s_ib_tasklet_call);

	/* if cq has been already reaped, ignore incoming cq event */
	if (atomic_read(&ic->i_cq_quiesce))
		return;

	poll_scq(ic, ic->i_send_cq, ic->i_send_wc);
	ib_req_notify_cq(ic->i_send_cq, IB_CQ_NEXT_COMP);
	poll_scq(ic, ic->i_send_cq, ic->i_send_wc);

	if (pmdfc_conn_up(conn) &&
	    (!test_bit(pmdfc_LL_SEND_FULL, &conn->c_flags) ||
	    test_bit(0, &conn->c_map_queued)))
		pmdfc_send_xmit(&ic->conn->c_path[0]);
}

static void poll_rcq(struct pmdfc_ib_connection *ic, struct ib_cq *cq,
		     struct ib_wc *wcs,
		     struct pmdfc_ib_ack_state *ack_state)
{
	int nr, i;
	struct ib_wc *wc;

	while ((nr = ib_poll_cq(cq, pmdfc_IB_WC_MAX, wcs)) > 0) {
		for (i = 0; i < nr; i++) {
			wc = wcs + i;
			pmdfcdebug("wc wr_id 0x%llx status %u byte_len %u imm_data %u\n",
				 (unsigned long long)wc->wr_id, wc->status,
				 wc->byte_len, be32_to_cpu(wc->ex.imm_data));

			pmdfc_ib_recv_cqe_handler(ic, wc, ack_state);
		}
	}
}

static void pmdfc_ib_tasklet_fn_recv(unsigned long data)
{
	struct pmdfc_ib_connection *ic = (struct pmdfc_ib_connection *)data;
	struct pmdfc_connection *conn = ic->conn;
	struct pmdfc_ib_device *pmdfc_ibdev = ic->pmdfc_ibdev;
	struct pmdfc_ib_ack_state state;

	if (!pmdfc_ibdev)
		pmdfc_conn_drop(conn);

	pmdfc_ib_stats_inc(s_ib_tasklet_call);

	/* if cq has been already reaped, ignore incoming cq event */
	if (atomic_read(&ic->i_cq_quiesce))
		return;

	memset(&state, 0, sizeof(state));
	poll_rcq(ic, ic->i_recv_cq, ic->i_recv_wc, &state);
	ib_req_notify_cq(ic->i_recv_cq, IB_CQ_SOLICITED);
	poll_rcq(ic, ic->i_recv_cq, ic->i_recv_wc, &state);

	if (state.ack_next_valid)
		pmdfc_ib_set_ack(ic, state.ack_next, state.ack_required);
	if (state.ack_recv_valid && state.ack_recv > ic->i_ack_recv) {
		pmdfc_send_drop_acked(conn, state.ack_recv, NULL);
		ic->i_ack_recv = state.ack_recv;
	}

	if (pmdfc_conn_up(conn))
		pmdfc_ib_attempt_ack(ic);
}

static void pmdfc_ib_qp_event_handler(struct ib_event *event, void *data)
{
	struct pmdfc_connection *conn = data;
	struct pmdfc_ib_connection *ic = conn->c_transport_data;

	pmdfcdebug("conn %p ic %p event %u (%s)\n", conn, ic, event->event,
		 ib_event_msg(event->event));

	switch (event->event) {
	case IB_EVENT_COMM_EST:
		rdma_notify(ic->i_cm_id, IB_EVENT_COMM_EST);
		break;
	default:
		pmdfcdebug("Fatal QP Event %u (%s) - connection %pI6c->%pI6c, reconnecting\n",
			 event->event, ib_event_msg(event->event),
			 &conn->c_laddr, &conn->c_faddr);
		pmdfc_conn_drop(conn);
		break;
	}
}

static void pmdfc_ib_cq_comp_handler_send(struct ib_cq *cq, void *context)
{
	struct pmdfc_connection *conn = context;
	struct pmdfc_ib_connection *ic = conn->c_transport_data;

	pmdfcdebug("conn %p cq %p\n", conn, cq);

	pmdfc_ib_stats_inc(s_ib_evt_handler_call);

	tasklet_schedule(&ic->i_send_tasklet);
}

/*
 * This needs to be very careful to not leave IS_ERR pointers around for
 * cleanup to trip over.
 */
static int pmdfc_ib_setup_qp(struct pmdfc_connection *conn)
{
	struct pmdfc_ib_connection *ic = conn->c_transport_data;
	struct ib_device *dev = ic->i_cm_id->device;
	struct ib_qp_init_attr attr;
	struct ib_cq_init_attr cq_attr = {};
	struct pmdfc_ib_device *pmdfc_ibdev;
	int ret, fr_queue_space;

	/*
	 * It's normal to see a null device if an incoming connection races
	 * with device removal, so we don't print a warning.
	 */
	pmdfc_ibdev = pmdfc_ib_get_client_data(dev);
	if (!pmdfc_ibdev)
		return -EOPNOTSUPP;

	/* The fr_queue_space is currently set to 512, to add extra space on
	 * completion queue and send queue. This extra space is used for FRMR
	 * registration and invalidation work requests
	 */
	fr_queue_space = (pmdfc_ibdev->use_fastreg ? pmdfc_IB_DEFAULT_FR_WR : 0);

	/* add the conn now so that connection establishment has the dev */
	pmdfc_ib_add_conn(pmdfc_ibdev, conn);

	if (pmdfc_ibdev->max_wrs < ic->i_send_ring.w_nr + 1)
		pmdfc_ib_ring_resize(&ic->i_send_ring, pmdfc_ibdev->max_wrs - 1);
	if (pmdfc_ibdev->max_wrs < ic->i_recv_ring.w_nr + 1)
		pmdfc_ib_ring_resize(&ic->i_recv_ring, pmdfc_ibdev->max_wrs - 1);

	/* Protection domain and memory range */
	ic->i_pd = pmdfc_ibdev->pd;

	ic->i_scq_vector = ibdev_get_unused_vector(pmdfc_ibdev);
	cq_attr.cqe = ic->i_send_ring.w_nr + fr_queue_space + 1;
	cq_attr.comp_vector = ic->i_scq_vector;
	ic->i_send_cq = ib_create_cq(dev, pmdfc_ib_cq_comp_handler_send,
				     pmdfc_ib_cq_event_handler, conn,
				     &cq_attr);
	if (IS_ERR(ic->i_send_cq)) {
		ret = PTR_ERR(ic->i_send_cq);
		ic->i_send_cq = NULL;
		ibdev_put_vector(pmdfc_ibdev, ic->i_scq_vector);
		pmdfcdebug("ib_create_cq send failed: %d\n", ret);
		goto pmdfc_ibdev_out;
	}

	ic->i_rcq_vector = ibdev_get_unused_vector(pmdfc_ibdev);
	cq_attr.cqe = ic->i_recv_ring.w_nr;
	cq_attr.comp_vector = ic->i_rcq_vector;
	ic->i_recv_cq = ib_create_cq(dev, pmdfc_ib_cq_comp_handler_recv,
				     pmdfc_ib_cq_event_handler, conn,
				     &cq_attr);
	if (IS_ERR(ic->i_recv_cq)) {
		ret = PTR_ERR(ic->i_recv_cq);
		ic->i_recv_cq = NULL;
		ibdev_put_vector(pmdfc_ibdev, ic->i_rcq_vector);
		pmdfcdebug("ib_create_cq recv failed: %d\n", ret);
		goto send_cq_out;
	}

	ret = ib_req_notify_cq(ic->i_send_cq, IB_CQ_NEXT_COMP);
	if (ret) {
		pmdfcdebug("ib_req_notify_cq send failed: %d\n", ret);
		goto recv_cq_out;
	}

	ret = ib_req_notify_cq(ic->i_recv_cq, IB_CQ_SOLICITED);
	if (ret) {
		pmdfcdebug("ib_req_notify_cq recv failed: %d\n", ret);
		goto recv_cq_out;
	}

	/* XXX negotiate max send/recv with remote? */
	memset(&attr, 0, sizeof(attr));
	attr.event_handler = pmdfc_ib_qp_event_handler;
	attr.qp_context = conn;
	/* + 1 to allow for the single ack message */
	attr.cap.max_send_wr = ic->i_send_ring.w_nr + fr_queue_space + 1;
	attr.cap.max_recv_wr = ic->i_recv_ring.w_nr + 1;
	attr.cap.max_send_sge = pmdfc_ibdev->max_sge;
	attr.cap.max_recv_sge = pmdfc_IB_RECV_SGE;
	attr.sq_sig_type = IB_SIGNAL_REQ_WR;
	attr.qp_type = IB_QPT_RC;
	attr.send_cq = ic->i_send_cq;
	attr.recv_cq = ic->i_recv_cq;

	/*
	 * XXX this can fail if max_*_wr is too large?  Are we supposed
	 * to back off until we get a value that the hardware can support?
	 */
	ret = rdma_create_qp(ic->i_cm_id, ic->i_pd, &attr);
	if (ret) {
		pmdfcdebug("rdma_create_qp failed: %d\n", ret);
		goto recv_cq_out;
	}

	ic->i_send_hdrs = ib_dma_alloc_coherent(dev,
					   ic->i_send_ring.w_nr *
						sizeof(struct pmdfc_header),
					   &ic->i_send_hdrs_dma, GFP_KERNEL);
	if (!ic->i_send_hdrs) {
		ret = -ENOMEM;
		pmdfcdebug("ib_dma_alloc_coherent send failed\n");
		goto qp_out;
	}

	ic->i_recv_hdrs = ib_dma_alloc_coherent(dev,
					   ic->i_recv_ring.w_nr *
						sizeof(struct pmdfc_header),
					   &ic->i_recv_hdrs_dma, GFP_KERNEL);
	if (!ic->i_recv_hdrs) {
		ret = -ENOMEM;
		pmdfcdebug("ib_dma_alloc_coherent recv failed\n");
		goto send_hdrs_dma_out;
	}

	ic->i_ack = ib_dma_alloc_coherent(dev, sizeof(struct pmdfc_header),
				       &ic->i_ack_dma, GFP_KERNEL);
	if (!ic->i_ack) {
		ret = -ENOMEM;
		pmdfcdebug("ib_dma_alloc_coherent ack failed\n");
		goto recv_hdrs_dma_out;
	}

	ic->i_sends = vzalloc_node(array_size(sizeof(struct pmdfc_ib_send_work),
					      ic->i_send_ring.w_nr),
				   ibdev_to_node(dev));
	if (!ic->i_sends) {
		ret = -ENOMEM;
		pmdfcdebug("send allocation failed\n");
		goto ack_dma_out;
	}

	ic->i_recvs = vzalloc_node(array_size(sizeof(struct pmdfc_ib_recv_work),
					      ic->i_recv_ring.w_nr),
				   ibdev_to_node(dev));
	if (!ic->i_recvs) {
		ret = -ENOMEM;
		pmdfcdebug("recv allocation failed\n");
		goto sends_out;
	}

	pmdfc_ib_recv_init_ack(ic);

	pmdfcdebug("conn %p pd %p cq %p %p\n", conn, ic->i_pd,
		 ic->i_send_cq, ic->i_recv_cq);

	goto out;

sends_out:
	vfree(ic->i_sends);
ack_dma_out:
	ib_dma_free_coherent(dev, sizeof(struct pmdfc_header),
			     ic->i_ack, ic->i_ack_dma);
recv_hdrs_dma_out:
	ib_dma_free_coherent(dev, ic->i_recv_ring.w_nr *
					sizeof(struct pmdfc_header),
					ic->i_recv_hdrs, ic->i_recv_hdrs_dma);
send_hdrs_dma_out:
	ib_dma_free_coherent(dev, ic->i_send_ring.w_nr *
					sizeof(struct pmdfc_header),
					ic->i_send_hdrs, ic->i_send_hdrs_dma);
qp_out:
	rdma_destroy_qp(ic->i_cm_id);
recv_cq_out:
	ib_destroy_cq(ic->i_recv_cq);
	ic->i_recv_cq = NULL;
send_cq_out:
	ib_destroy_cq(ic->i_send_cq);
	ic->i_send_cq = NULL;
pmdfc_ibdev_out:
	pmdfc_ib_remove_conn(pmdfc_ibdev, conn);
out:
	pmdfc_ib_dev_put(pmdfc_ibdev);

	return ret;
}

static u32 pmdfc_ib_protocol_compatible(struct rdma_cm_event *event, bool isv6)
{
	const union pmdfc_ib_conn_priv *dp = event->param.conn.private_data;
	u8 data_len, major, minor;
	u32 version = 0;
	__be16 mask;
	u16 common;

	/*
	 * rdma_cm private data is odd - when there is any private data in the
	 * request, we will be given a pretty large buffer without telling us the
	 * original size. The only way to tell the difference is by looking at
	 * the contents, which are initialized to zero.
	 * If the protocol version fields aren't set, this is a connection attempt
	 * from an older version. This could could be 3.0 or 2.0 - we can't tell.
	 * We really should have changed this for OFED 1.3 :-(
	 */

	/* Be paranoid. pmdfc always has privdata */
	if (!event->param.conn.private_data_len) {
		printk(KERN_NOTICE "pmdfc incoming connection has no private data, "
			"rejecting\n");
		return 0;
	}

	if (isv6) {
		data_len = sizeof(struct pmdfc6_ib_connect_private);
		major = dp->ricp_v6.dp_protocol_major;
		minor = dp->ricp_v6.dp_protocol_minor;
		mask = dp->ricp_v6.dp_protocol_minor_mask;
	} else {
		data_len = sizeof(struct pmdfc_ib_connect_private);
		major = dp->ricp_v4.dp_protocol_major;
		minor = dp->ricp_v4.dp_protocol_minor;
		mask = dp->ricp_v4.dp_protocol_minor_mask;
	}

	/* Even if len is crap *now* I still want to check it. -ASG */
	if (event->param.conn.private_data_len < data_len || major == 0)
		return pmdfc_PROTOCOL_4_0;

	common = be16_to_cpu(mask) & pmdfc_IB_SUPPORTED_PROTOCOLS;
	if (major == 4 && common) {
		version = pmdfc_PROTOCOL_4_0;
		while ((common >>= 1) != 0)
			version++;
	} else if (pmdfc_PROTOCOL_COMPAT_VERSION ==
		   pmdfc_PROTOCOL(major, minor)) {
		version = pmdfc_PROTOCOL_COMPAT_VERSION;
	} else {
		if (isv6)
			printk_ratelimited(KERN_NOTICE "pmdfc: Connection from %pI6c using incompatible protocol version %u.%u\n",
					   &dp->ricp_v6.dp_saddr, major, minor);
		else
			printk_ratelimited(KERN_NOTICE "pmdfc: Connection from %pI4 using incompatible protocol version %u.%u\n",
					   &dp->ricp_v4.dp_saddr, major, minor);
	}
	return version;
}

#if IS_ENABLED(CONFIG_IPV6)
/* Given an IPv6 address, find the net_device which hosts that address and
 * return its index.  This is used by the pmdfc_ib_cm_handle_connect() code to
 * find the interface index of where an incoming request comes from when
 * the request is using a link local address.
 *
 * Note one problem in this search.  It is possible that two interfaces have
 * the same link local address.  Unfortunately, this cannot be solved unless
 * the underlying layer gives us the interface which an incoming RDMA connect
 * request comes from.
 */
static u32 __pmdfc_find_ifindex(struct net *net, const struct in6_addr *addr)
{
	struct net_device *dev;
	int idx = 0;

	rcu_read_lock();
	for_each_netdev_rcu(net, dev) {
		if (ipv6_chk_addr(net, addr, dev, 1)) {
			idx = dev->ifindex;
			break;
		}
	}
	rcu_read_unlock();

	return idx;
}
#endif

int pmdfc_ib_cm_handle_connect(struct rdma_cm_id *cm_id,
			     struct rdma_cm_event *event, bool isv6)
{
	__be64 lguid = cm_id->route.path_rec->sgid.global.interface_id;
	__be64 fguid = cm_id->route.path_rec->dgid.global.interface_id;
	const struct pmdfc_ib_conn_priv_cmn *dp_cmn;
	struct pmdfc_connection *conn = NULL;
	struct pmdfc_ib_connection *ic = NULL;
	struct rdma_conn_param conn_param;
	const union pmdfc_ib_conn_priv *dp;
	union pmdfc_ib_conn_priv dp_rep;
	struct in6_addr s_mapped_addr;
	struct in6_addr d_mapped_addr;
	const struct in6_addr *saddr6;
	const struct in6_addr *daddr6;
	int destroy = 1;
	u32 ifindex = 0;
	u32 version;
	int err = 1;

	/* Check whether the remote protocol version matches ours. */
	version = pmdfc_ib_protocol_compatible(event, isv6);
	if (!version) {
		err = pmdfc_RDMA_REJ_INCOMPAT;
		goto out;
	}

	dp = event->param.conn.private_data;
	if (isv6) {
#if IS_ENABLED(CONFIG_IPV6)
		dp_cmn = &dp->ricp_v6.dp_cmn;
		saddr6 = &dp->ricp_v6.dp_saddr;
		daddr6 = &dp->ricp_v6.dp_daddr;
		/* If either address is link local, need to find the
		 * interface index in order to create a proper pmdfc
		 * connection.
		 */
		if (ipv6_addr_type(daddr6) & IPV6_ADDR_LINKLOCAL) {
			/* Using init_net for now ..  */
			ifindex = __pmdfc_find_ifindex(&init_net, daddr6);
			/* No index found...  Need to bail out. */
			if (ifindex == 0) {
				err = -EOPNOTSUPP;
				goto out;
			}
		} else if (ipv6_addr_type(saddr6) & IPV6_ADDR_LINKLOCAL) {
			/* Use our address to find the correct index. */
			ifindex = __pmdfc_find_ifindex(&init_net, daddr6);
			/* No index found...  Need to bail out. */
			if (ifindex == 0) {
				err = -EOPNOTSUPP;
				goto out;
			}
		}
#else
		err = -EOPNOTSUPP;
		goto out;
#endif
	} else {
		dp_cmn = &dp->ricp_v4.dp_cmn;
		ipv6_addr_set_v4mapped(dp->ricp_v4.dp_saddr, &s_mapped_addr);
		ipv6_addr_set_v4mapped(dp->ricp_v4.dp_daddr, &d_mapped_addr);
		saddr6 = &s_mapped_addr;
		daddr6 = &d_mapped_addr;
	}

	pmdfcdebug("saddr %pI6c daddr %pI6c pmdfcv%u.%u lguid 0x%llx fguid 0x%llx, tos:%d\n",
		 saddr6, daddr6, pmdfc_PROTOCOL_MAJOR(version),
		 pmdfc_PROTOCOL_MINOR(version),
		 (unsigned long long)be64_to_cpu(lguid),
		 (unsigned long long)be64_to_cpu(fguid), dp_cmn->ricpc_dp_toss);

	/* pmdfc/IB is not currently netns aware, thus init_net */
	conn = pmdfc_conn_create(&init_net, daddr6, saddr6,
			       &pmdfc_ib_transport, dp_cmn->ricpc_dp_toss,
			       GFP_KERNEL, ifindex);
	if (IS_ERR(conn)) {
		pmdfcdebug("pmdfc_conn_create failed (%ld)\n", PTR_ERR(conn));
		conn = NULL;
		goto out;
	}

	/*
	 * The connection request may occur while the
	 * previous connection exist, e.g. in case of failover.
	 * But as connections may be initiated simultaneously
	 * by both hosts, we have a random backoff mechanism -
	 * see the comment above pmdfc_queue_reconnect()
	 */
	mutex_lock(&conn->c_cm_lock);
	if (!pmdfc_conn_transition(conn, pmdfc_CONN_DOWN, pmdfc_CONN_CONNECTING)) {
		if (pmdfc_conn_state(conn) == pmdfc_CONN_UP) {
			pmdfcdebug("incoming connect while connecting\n");
			pmdfc_conn_drop(conn);
			pmdfc_ib_stats_inc(s_ib_listen_closed_stale);
		} else
		if (pmdfc_conn_state(conn) == pmdfc_CONN_CONNECTING) {
			/* Wait and see - our connect may still be succeeding */
			pmdfc_ib_stats_inc(s_ib_connect_raced);
		}
		goto out;
	}

	ic = conn->c_transport_data;

	pmdfc_ib_set_protocol(conn, version);
	pmdfc_ib_set_flow_control(conn, be32_to_cpu(dp_cmn->ricpc_credit));

	/* If the peer gave us the last packet it saw, process this as if
	 * we had received a regular ACK. */
	if (dp_cmn->ricpc_ack_seq)
		pmdfc_send_drop_acked(conn, be64_to_cpu(dp_cmn->ricpc_ack_seq),
				    NULL);

	BUG_ON(cm_id->context);
	BUG_ON(ic->i_cm_id);

	ic->i_cm_id = cm_id;
	cm_id->context = conn;

	/* We got halfway through setting up the ib_connection, if we
	 * fail now, we have to take the long route out of this mess. */
	destroy = 0;

	err = pmdfc_ib_setup_qp(conn);
	if (err) {
		pmdfc_ib_conn_error(conn, "pmdfc_ib_setup_qp failed (%d)\n", err);
		goto out;
	}

	pmdfc_ib_cm_fill_conn_param(conn, &conn_param, &dp_rep, version,
				  event->param.conn.responder_resources,
				  event->param.conn.initiator_depth, isv6);

	/* rdma_accept() calls rdma_reject() internally if it fails */
	if (rdma_accept(cm_id, &conn_param))
		pmdfc_ib_conn_error(conn, "rdma_accept failed\n");

out:
	if (conn)
		mutex_unlock(&conn->c_cm_lock);
	if (err)
		rdma_reject(cm_id, &err, sizeof(int));
	return destroy;
}


int pmdfc_ib_cm_initiate_connect(struct rdma_cm_id *cm_id, bool isv6)
{
	struct pmdfc_connection *conn = cm_id->context;
	struct pmdfc_ib_connection *ic = conn->c_transport_data;
	struct rdma_conn_param conn_param;
	union pmdfc_ib_conn_priv dp;
	int ret;

	/* If the peer doesn't do protocol negotiation, we must
	 * default to pmdfcv3.0 */
	pmdfc_ib_set_protocol(conn, pmdfc_PROTOCOL_4_1);
	ic->i_flowctl = pmdfc_ib_sysctl_flow_control;	/* advertise flow control */

	ret = pmdfc_ib_setup_qp(conn);
	if (ret) {
		pmdfc_ib_conn_error(conn, "pmdfc_ib_setup_qp failed (%d)\n", ret);
		goto out;
	}

	pmdfc_ib_cm_fill_conn_param(conn, &conn_param, &dp,
				  conn->c_proposed_version,
				  UINT_MAX, UINT_MAX, isv6);
	ret = rdma_connect(cm_id, &conn_param);
	if (ret)
		pmdfc_ib_conn_error(conn, "rdma_connect failed (%d)\n", ret);

out:
	/* Beware - returning non-zero tells the rdma_cm to destroy
	 * the cm_id. We should certainly not do it as long as we still
	 * "own" the cm_id. */
	if (ret) {
		if (ic->i_cm_id == cm_id)
			ret = 0;
	}
	ic->i_active_side = true;
	return ret;
}

int pmdfc_ib_conn_path_connect(struct pmdfc_conn_path *cp)
{
	struct pmdfc_connection *conn = cp->cp_conn;
	struct sockaddr_storage src, dest;
	rdma_cm_event_handler handler;
	struct pmdfc_ib_connection *ic;
	int ret;

	ic = conn->c_transport_data;

	/* XXX I wonder what affect the port space has */
	/* delegate cm event handler to rdma_transport */
#if IS_ENABLED(CONFIG_IPV6)
	if (conn->c_isv6)
		handler = pmdfc6_rdma_cm_event_handler;
	else
#endif
		handler = pmdfc_rdma_cm_event_handler;
	ic->i_cm_id = rdma_create_id(&init_net, handler, conn,
				     RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(ic->i_cm_id)) {
		ret = PTR_ERR(ic->i_cm_id);
		ic->i_cm_id = NULL;
		pmdfcdebug("rdma_create_id() failed: %d\n", ret);
		goto out;
	}

	pmdfcdebug("created cm id %p for conn %p\n", ic->i_cm_id, conn);

	if (ipv6_addr_v4mapped(&conn->c_faddr)) {
		struct sockaddr_in *sin;

		sin = (struct sockaddr_in *)&src;
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = conn->c_laddr.s6_addr32[3];
		sin->sin_port = 0;

		sin = (struct sockaddr_in *)&dest;
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = conn->c_faddr.s6_addr32[3];
		sin->sin_port = htons(pmdfc_PORT);
	} else {
		struct sockaddr_in6 *sin6;

		sin6 = (struct sockaddr_in6 *)&src;
		sin6->sin6_family = AF_INET6;
		sin6->sin6_addr = conn->c_laddr;
		sin6->sin6_port = 0;
		sin6->sin6_scope_id = conn->c_dev_if;

		sin6 = (struct sockaddr_in6 *)&dest;
		sin6->sin6_family = AF_INET6;
		sin6->sin6_addr = conn->c_faddr;
		sin6->sin6_port = htons(pmdfc_CM_PORT);
		sin6->sin6_scope_id = conn->c_dev_if;
	}

	ret = rdma_resolve_addr(ic->i_cm_id, (struct sockaddr *)&src,
				(struct sockaddr *)&dest,
				pmdfc_RDMA_RESOLVE_TIMEOUT_MS);
	if (ret) {
		pmdfcdebug("addr resolve failed for cm id %p: %d\n", ic->i_cm_id,
			 ret);
		rdma_destroy_id(ic->i_cm_id);
		ic->i_cm_id = NULL;
	}

out:
	return ret;
}

/*
 * This is so careful about only cleaning up resources that were built up
 * so that it can be called at any point during startup.  In fact it
 * can be called multiple times for a given connection.
 */
void pmdfc_ib_conn_path_shutdown(struct pmdfc_conn_path *cp)
{
	struct pmdfc_connection *conn = cp->cp_conn;
	struct pmdfc_ib_connection *ic = conn->c_transport_data;
	int err = 0;

	pmdfcdebug("cm %p pd %p cq %p %p qp %p\n", ic->i_cm_id,
		 ic->i_pd, ic->i_send_cq, ic->i_recv_cq,
		 ic->i_cm_id ? ic->i_cm_id->qp : NULL);

	if (ic->i_cm_id) {
		struct ib_device *dev = ic->i_cm_id->device;

		pmdfcdebug("disconnecting cm %p\n", ic->i_cm_id);
		err = rdma_disconnect(ic->i_cm_id);
		if (err) {
			/* Actually this may happen quite frequently, when
			 * an outgoing connect raced with an incoming connect.
			 */
			pmdfcdebug("failed to disconnect, cm: %p err %d\n",
				ic->i_cm_id, err);
		}

		/* kick off "flush_worker" for all pools in order to reap
		 * all FRMR registrations that are still marked "FRMR_IS_INUSE"
		 */
		pmdfc_ib_flush_mrs();

		/*
		 * We want to wait for tx and rx completion to finish
		 * before we tear down the connection, but we have to be
		 * careful not to get stuck waiting on a send ring that
		 * only has unsignaled sends in it.  We've shutdown new
		 * sends before getting here so by waiting for signaled
		 * sends to complete we're ensured that there will be no
		 * more tx processing.
		 */
		wait_event(pmdfc_ib_ring_empty_wait,
			   pmdfc_ib_ring_empty(&ic->i_recv_ring) &&
			   (atomic_read(&ic->i_signaled_sends) == 0) &&
			   (atomic_read(&ic->i_fastreg_inuse_count) == 0) &&
			   (atomic_read(&ic->i_fastreg_wrs) == pmdfc_IB_DEFAULT_FR_WR));
		tasklet_kill(&ic->i_send_tasklet);
		tasklet_kill(&ic->i_recv_tasklet);

		atomic_set(&ic->i_cq_quiesce, 1);

		/* first destroy the ib state that generates callbacks */
		if (ic->i_cm_id->qp)
			rdma_destroy_qp(ic->i_cm_id);
		if (ic->i_send_cq) {
			if (ic->pmdfc_ibdev)
				ibdev_put_vector(ic->pmdfc_ibdev, ic->i_scq_vector);
			ib_destroy_cq(ic->i_send_cq);
		}

		if (ic->i_recv_cq) {
			if (ic->pmdfc_ibdev)
				ibdev_put_vector(ic->pmdfc_ibdev, ic->i_rcq_vector);
			ib_destroy_cq(ic->i_recv_cq);
		}

		/* then free the resources that ib callbacks use */
		if (ic->i_send_hdrs)
			ib_dma_free_coherent(dev,
					   ic->i_send_ring.w_nr *
						sizeof(struct pmdfc_header),
					   ic->i_send_hdrs,
					   ic->i_send_hdrs_dma);

		if (ic->i_recv_hdrs)
			ib_dma_free_coherent(dev,
					   ic->i_recv_ring.w_nr *
						sizeof(struct pmdfc_header),
					   ic->i_recv_hdrs,
					   ic->i_recv_hdrs_dma);

		if (ic->i_ack)
			ib_dma_free_coherent(dev, sizeof(struct pmdfc_header),
					     ic->i_ack, ic->i_ack_dma);

		if (ic->i_sends)
			pmdfc_ib_send_clear_ring(ic);
		if (ic->i_recvs)
			pmdfc_ib_recv_clear_ring(ic);

		rdma_destroy_id(ic->i_cm_id);

		/*
		 * Move connection back to the nodev list.
		 */
		if (ic->pmdfc_ibdev)
			pmdfc_ib_remove_conn(ic->pmdfc_ibdev, conn);

		ic->i_cm_id = NULL;
		ic->i_pd = NULL;
		ic->i_send_cq = NULL;
		ic->i_recv_cq = NULL;
		ic->i_send_hdrs = NULL;
		ic->i_recv_hdrs = NULL;
		ic->i_ack = NULL;
	}
	BUG_ON(ic->pmdfc_ibdev);

	/* Clear pending transmit */
	if (ic->i_data_op) {
		struct pmdfc_message *rm;

		rm = container_of(ic->i_data_op, struct pmdfc_message, data);
		pmdfc_message_put(rm);
		ic->i_data_op = NULL;
	}

	/* Clear the ACK state */
	clear_bit(IB_ACK_IN_FLIGHT, &ic->i_ack_flags);
#ifdef KERNEL_HAS_ATOMIC64
	atomic64_set(&ic->i_ack_next, 0);
#else
	ic->i_ack_next = 0;
#endif
	ic->i_ack_recv = 0;

	/* Clear flow control state */
	ic->i_flowctl = 0;
	atomic_set(&ic->i_credits, 0);

	pmdfc_ib_ring_init(&ic->i_send_ring, pmdfc_ib_sysctl_max_send_wr);
	pmdfc_ib_ring_init(&ic->i_recv_ring, pmdfc_ib_sysctl_max_recv_wr);

	if (ic->i_ibinc) {
		pmdfc_inc_put(&ic->i_ibinc->ii_inc);
		ic->i_ibinc = NULL;
	}

	vfree(ic->i_sends);
	ic->i_sends = NULL;
	vfree(ic->i_recvs);
	ic->i_recvs = NULL;
	ic->i_active_side = false;
}

int pmdfc_ib_conn_alloc(struct pmdfc_connection *conn, gfp_t gfp)
{
	struct pmdfc_ib_connection *ic;
	unsigned long flags;
	int ret;

	/* XXX too lazy? */
	ic = kzalloc(sizeof(struct pmdfc_ib_connection), gfp);
	if (!ic)
		return -ENOMEM;

	ret = pmdfc_ib_recv_alloc_caches(ic, gfp);
	if (ret) {
		kfree(ic);
		return ret;
	}

	INIT_LIST_HEAD(&ic->ib_node);
	tasklet_init(&ic->i_send_tasklet, pmdfc_ib_tasklet_fn_send,
		     (unsigned long)ic);
	tasklet_init(&ic->i_recv_tasklet, pmdfc_ib_tasklet_fn_recv,
		     (unsigned long)ic);
	mutex_init(&ic->i_recv_mutex);
#ifndef KERNEL_HAS_ATOMIC64
	spin_lock_init(&ic->i_ack_lock);
#endif
	atomic_set(&ic->i_signaled_sends, 0);
	atomic_set(&ic->i_fastreg_wrs, pmdfc_IB_DEFAULT_FR_WR);

	/*
	 * pmdfc_ib_conn_shutdown() waits for these to be emptied so they
	 * must be initialized before it can be called.
	 */
	pmdfc_ib_ring_init(&ic->i_send_ring, pmdfc_ib_sysctl_max_send_wr);
	pmdfc_ib_ring_init(&ic->i_recv_ring, pmdfc_ib_sysctl_max_recv_wr);

	ic->conn = conn;
	conn->c_transport_data = ic;

	spin_lock_irqsave(&ib_nodev_conns_lock, flags);
	list_add_tail(&ic->ib_node, &ib_nodev_conns);
	spin_unlock_irqrestore(&ib_nodev_conns_lock, flags);


	pmdfcdebug("conn %p conn ic %p\n", conn, conn->c_transport_data);
	return 0;
}

/*
 * Free a connection. Connection must be shut down and not set for reconnect.
 */
void pmdfc_ib_conn_free(void *arg)
{
	struct pmdfc_ib_connection *ic = arg;
	spinlock_t	*lock_ptr;

	pmdfcdebug("ic %p\n", ic);

	/*
	 * Conn is either on a dev's list or on the nodev list.
	 * A race with shutdown() or connect() would cause problems
	 * (since pmdfc_ibdev would change) but that should never happen.
	 */
	lock_ptr = ic->pmdfc_ibdev ? &ic->pmdfc_ibdev->spinlock : &ib_nodev_conns_lock;

	spin_lock_irq(lock_ptr);
	list_del(&ic->ib_node);
	spin_unlock_irq(lock_ptr);

	pmdfc_ib_recv_free_caches(ic);

	kfree(ic);
}


/*
 * An error occurred on the connection
 */
void
__pmdfc_ib_conn_error(struct pmdfc_connection *conn, const char *fmt, ...)
{
	va_list ap;

	pmdfc_conn_drop(conn);

	va_start(ap, fmt);
	vprintk(fmt, ap);
	va_end(ap);
}
