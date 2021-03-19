/**********************************************************************
 * Copyright (c) 2020
 *  Sang-Hoon Kim <sanghoonkim@ajou.ac.kr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTIABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 **********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <byteswap.h>

#include <sys/types.h>
#include <sys/mman.h>

#include <infiniband/verbs.h>

#include "types.h"
#include "config.h"

#define MAX_MR_SIZE	(1UL << 26)		/* Max 64 MB ( << 26 )*/

static struct ibv_context *__get_device_context(struct krono_t *kh)
{
	struct ibv_device **dev_list, *dev;
	int nr_devices;

	dev_list = ibv_get_device_list(&nr_devices);

	if (!dev_list) {
		perror("Failed to get IB device list");
		return NULL;
	}

	for (int i = 0; i < nr_devices; i++) {
		uint64_t guid;

		dev = dev_list[i];
		guid = bswap_64(ibv_get_device_guid(dev));
		for (int j = 0; j < kh->nr_nodes; j++) {
			if (guid == kh->nodes[j]) {
				struct ibv_context *ctx = ibv_open_device(dev);

				if (!ctx) {
					perror("Cannot allocate context from device");
					goto out;
				}

				printf("my nid: %d, guid: 0x%016lx)\n", j, kh->nodes[j]);
				mynid = kh->nid = j;

				ibv_free_device_list(dev_list);
				return ctx;
			}
		}
	}

out:
	ibv_free_device_list(dev_list);
	return NULL;
}

static int __count = 0;

#define RECV_BUFFER_MAX (1U << 20)

static bool __post_recv_wr(struct krono_t *kh)
{
	for (int i = 0; i < MAX_MR_SIZE / RECV_BUFFER_MAX; i++) {
		void *buffer = kh->buffer + i * RECV_BUFFER_MAX;
		struct ibv_sge sg_list = {
			.addr = (uintptr_t)buffer,
			.length = RECV_BUFFER_MAX,
			.lkey = kh->mr->lkey,
		};
		struct ibv_recv_wr wr = {
			.next = NULL,
			.wr_id = 0,
			.sg_list = &sg_list,
			.num_sge = 1,
		};
		struct ibv_recv_wr *bad_wr;

		memset(buffer, 0xcd, 4096);
		wr.wr_id += __count++;

		if (ibv_post_recv(kh->qp, &wr, &bad_wr)) {
			perror("Unable to post recv wr");
			return false;
		}
		printf("Posted recv wr for %p\n", buffer);
	}
	return true;
}

static unsigned long __elapsed_time(struct timespec * const start, struct timespec * const end)
{
	return (end->tv_sec * 1e9 + end->tv_nsec) - (start->tv_sec * 1e9 + start->tv_nsec);
}

static bool __post_send_wr(struct krono_t *kh)
{
	struct timespec t0, t1, t2;
	void *buffer = kh->buffer;
	struct ibv_sge sg_list = {
		.addr = (uintptr_t)buffer,
		.length = 4096,
		.lkey = kh->mr->lkey,
	};
	struct ibv_send_wr wr = {
		.wr_id = 0xf00dbeef,
		.next = NULL,
		.sg_list = &sg_list,
		.num_sge = 1,
		.opcode = IBV_WR_SEND,
		.send_flags = IBV_SEND_SIGNALED,
	};
	struct ibv_send_wr *bad_wr;

	memset(buffer, 0x43 + __count++, 32);
	wr.wr_id += __count;
	{
		struct ibv_qp_attr attr;
		struct ibv_qp_init_attr init_attr;
		bzero(&attr, sizeof(attr));
		ibv_query_qp(kh->qp, &attr, IBV_QP_RQ_PSN | IBV_QP_SQ_PSN, &init_attr);
		printf("Posting send wr for %p %d %d\n", buffer, attr.rq_psn, attr.sq_psn);
	}
	clock_gettime(CLOCK_REALTIME, &t0);
	if (ibv_post_send(kh->qp, &wr, &bad_wr)) {
		perror("Unable to post send wr");
		return false;
	}
	clock_gettime(CLOCK_REALTIME, &t1);
	{
		struct ibv_wc wc;
		int num_comp;

		do {
			num_comp = ibv_poll_cq(kh->cq, 1, &wc);
		} while (num_comp == 0);
		clock_gettime(CLOCK_REALTIME, &t2);

		printf("%lu ns %lu ns\n",
				__elapsed_time(&t0, &t1), 
				__elapsed_time(&t1, &t2));

		if (num_comp < 0) {
			printf("Unable to poll cq\n");
			return false;
		}

		if (wc.status == !IBV_WC_SUCCESS) {
			printf("wc failed %d\n", wc.status);
			return false;
		}

		printf("Sent 0x%lx\n", wc.wr_id);

	}
	return true;
}

static bool __init_queues(struct krono_t *kh)
{
	struct ibv_context *ctx = kh->ctx;
	struct ibv_cq *cq = NULL;
	struct ibv_qp *qp;
	int flags;
	int psn = lrand48() & 0xffffff;
	{
		cq = ibv_create_cq(ctx, 64, kh, NULL, 0);
		if (!cq) {
			perror("Cannot create cq");
			return false;
		}
		kh->cq = cq;
	}
	{
		struct ibv_qp_init_attr qattr = {
			.qp_type = IBV_QPT_RC,
			.send_cq = cq,
			.recv_cq = cq,
			.cap = {
				.max_send_wr = 64,
				.max_recv_wr = 64, /* rx_depth */
				.max_send_sge = 1,
				.max_recv_sge = 1,
			},
		};

		qp = ibv_create_qp(kh->pd, &qattr);
		if (!qp) {
			perror("Unable to create qp");
			return false;
		}
		kh->qp = qp;

		printf("qpn: %d, psn: %d\n", qp->qp_num, psn);
	}
	{
		/* Retrieve the IDs */
		struct ibv_port_attr pattr;
		int ret;
		bzero(&pattr, sizeof(pattr));
		ret = ibv_query_port(
				ctx, 1 /* 1-base index for port number */, &pattr);
		if (ret) {
			perror("Cannot query port attributes");
			ibv_close_device(ctx);
			return false;
		}
		printf("sm_lid: %d, lid: %d\n", pattr.sm_lid, pattr.lid);

		set_node_info(kh, mynid, "sm_lid", pattr.sm_lid);
		set_node_info(kh, mynid, "lid", pattr.lid);
		set_node_info(kh, mynid, "qpn", qp->qp_num);
		set_node_info(kh, mynid, "psn", psn);
		set_node_info(kh, mynid, "rkey", kh->mr->rkey);
	}
	{
		struct ibv_qp_attr attr = {
			.qp_state = IBV_QPS_INIT,
			.pkey_index = 0,
			.port_num = 1,
			.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE,
		};
		flags = IBV_QP_STATE;
		flags |= IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

		if (ibv_modify_qp(qp, &attr, flags)) {
			perror("Unable to set INIT");
		}
	}
	{
		struct ibv_qp_attr attr = {
			.qp_state = IBV_QPS_RTR,
			.path_mtu = IBV_MTU_4096,
			.ah_attr = {
				.src_path_bits = 0,
				.port_num = 1,
				.dlid = 0,		/* destination lid */
				.is_global = 0,
			},
			.dest_qp_num = 0,	/* dest_qpn */
			.rq_psn = 0,		/*dest->psn */
			.max_dest_rd_atomic = 1,
			.min_rnr_timer = 12,
		};
		unsigned int value;
		int peer = (mynid == 0) ? 1 : 0;

		while(!get_node_info(kh, peer, "lid", &value)) {
			printf("Wait for peer %d\n", peer);
			sleep(1);
		}
		attr.ah_attr.dlid = value;

		get_node_info(kh, peer, "qpn", &attr.dest_qp_num);
		get_node_info(kh, peer, "psn", &attr.rq_psn);

		flags = IBV_QP_STATE;
		flags |= IBV_QP_PATH_MTU;
		flags |= IBV_QP_AV | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN;
		flags |= IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

		if (ibv_modify_qp(qp, &attr, flags )) {
			perror("Unable to set RTR");
		}
	}
	{
		struct ibv_qp_attr attr = {
			.qp_state = IBV_QPS_RTS,
			.sq_psn = psn, /* my->psn */
			.timeout = 14,
			.retry_cnt = 7,
			.rnr_retry = 7,
			.max_rd_atomic = 1,

		};
		flags = IBV_QP_STATE;
		flags |= IBV_QP_SQ_PSN;
		flags |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC;

		if (ibv_modify_qp(qp, &attr, flags)) {
			perror("Unable to set RTS");
		}
	}

	return true;
}

static bool __prep_pd_mr(struct krono_t *kh)
{
	/* Allocate a protection domain */
	struct ibv_context *ctx = kh->ctx;
	struct ibv_pd *pd = ibv_alloc_pd(ctx);
	const size_t size = MAX_MR_SIZE;

	void *buffer = NULL;
	struct ibv_mr *mr = NULL;

	if (!pd) {
		perror("Unable to allocate pd");
		return false;
	}

	buffer = mmap(NULL, size , PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS,
			0, 0);
	if (!buffer) {
		perror("Cannot allocate buffer");
		goto out;
	}
	mr = ibv_reg_mr(pd, buffer, size,
			IBV_ACCESS_LOCAL_WRITE |
			IBV_ACCESS_REMOTE_WRITE |
			IBV_ACCESS_REMOTE_READ |
			IBV_ACCESS_REMOTE_ATOMIC);

	if (!mr) {
		perror("Cannot alloate mr");
		goto out_munmap;
	}

	kh->pd = pd;
	kh->mr = mr;
	kh->buffer = buffer;

	printf("buffer:%p, lkey: %d, rkey: %d\n",
			buffer, mr->lkey, mr->rkey);

	return true;

out_munmap:
	munmap(buffer, size);
out:
	ibv_dealloc_pd(pd);
	return false;
}


bool init_ib(struct krono_t *kh)
{
	struct ibv_context *ctx;

	srand48(getpid() * time(NULL));

	ctx = __get_device_context(kh);
	if (!ctx) return false;

	kh->ctx = ctx;

	if (!__prep_pd_mr(kh)) {
		return false;
	}
	if (!__init_queues(kh)) {
		return false;
	}

	if (mynid == 0) {
		for (int i = 0; i < 32; i++) {
			__post_send_wr(kh);
		}
	} else {
		struct ibv_wc wc;
		int num_comp;
		sleep(1);
		__post_recv_wr(kh);


		do {
			num_comp = ibv_poll_cq(kh->cq, 1, &wc);
		} while (num_comp == 0);

		if (num_comp < 0) {
			printf("Shit failed\n");
			return false;
		}

		if (wc.status == !IBV_WC_SUCCESS) {
			printf("wc failed shit %d\n", wc.status);
			return false;
		}

		//printf("%lx: %x\n", wc.wr_id, *(int *)buffer);
	}
	return true;
}

void fini_ib(struct krono_t *kh)
{
	if (kh->qp) {
		ibv_destroy_qp(kh->qp);
	}
	if (kh->cq) {
		ibv_destroy_cq(kh->cq);
	}
	if (kh->buffer) {
		munmap(kh->buffer, MAX_MR_SIZE);
	}

	if (kh->mr) {
		ibv_dereg_mr(kh->mr);
	}

	if (kh->pd) {
		ibv_dealloc_pd(kh->pd);
	}

	if (kh->ctx) {
		ibv_close_device(kh->ctx);
	}
	return;
}

