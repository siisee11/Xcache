#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/slab.h>
#include <linux/cpumask.h>
#include <linux/jiffies.h>

#include "pmdfc.h"
#include "rdma_conn.h"
#include "rdma_op.h"
#include "timeperf.h"

#define KTIME_CHECK 1

extern struct pmdfc_rdma_ctrl *gctrl;
extern int numcpus;
extern int numqueues;
extern struct kmem_cache *req_cache;
/* we mainly do send wrs */
extern int QP_MAX_SEND_WR;

uint32_t bit_mask(int node_id, int msg_num, int type, int state, uint32_t num){
	uint32_t target = (((uint32_t)node_id << 28) | ((uint32_t)msg_num << 16) | ((uint32_t)type << 12) | ((uint32_t)state << 8) | ((uint32_t)num & 0x000000ff));
	return target;
}

void bit_unmask(uint32_t target, int* node_id, int* msg_num, int* type, int* state, uint32_t* num){
	*num = (uint32_t)(target & 0x000000ff);
	*state = (int)((target >> 8) & 0x0000000f);
	*type = (int)((target >> 12) & 0x0000000f);
	*msg_num = (int)((target >> 16) & 0x00000fff);
	*node_id = (int)((target >> 28) & 0x0000000f);
}

#ifdef KTIME_CHECK
void pmdfc_rdma_print_stat() {
	fperf_print("begin_recv");
	fperf_print("post_send");
	fperf_print("poll_rr");
}
EXPORT_SYMBOL_GPL(pmdfc_rdma_print_stat);
#else
void pmdfc_rdma_print_stat() {
	return;
}
EXPORT_SYMBOL_GPL(pmdfc_rdma_print_stat);
#endif

static void pmdfc_rdma_send_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct rdma_req *req =
		container_of(wc->wr_cqe, struct rdma_req, cqe);
	struct rdma_queue *q = cq->cq_context;
	struct ib_device *ibdev = q->ctrl->rdev->dev;

//	pr_info("[ PASS ] wc.wr_id(%llx) send_done\n", wc->wr_id);
	if (unlikely(wc->status != IB_WC_SUCCESS)) {
		pr_err("[ FAIL ] pmdfc_rdma_send_done status is not success, it is=%d\n", wc->status);
		//q->write_error = wc->status;
	}
	ib_dma_unmap_page(ibdev, req->dma, PAGE_SIZE, DMA_TO_DEVICE); /* XXX for test */

	atomic_dec(&q->pending);
	kmem_cache_free(req_cache, req);
}

/* allocates a pmdfc rdma request, creates a dma mapping for it in
 * req->dma, and synchronizes the dma mapping in the direction of
 * the dma map.
 * Don't touch the page with cpu after creating the request for it!
 * Deallocates the request if there was an error */
static inline int get_req_for_page(struct rdma_req **req, struct ib_device *dev,
		struct page *page, enum dma_data_direction dir)
{
	int ret;

	ret = 0;
	*req = kmem_cache_alloc(req_cache, GFP_ATOMIC);
	if (unlikely(!req)) {
		pr_err("[ FAIL ] no memory for req\n");
		ret = -ENOMEM;
		goto out;
	}

	(*req)->page = page;
	init_completion(&(*req)->done);

	(*req)->dma = ib_dma_map_page(dev, page, 0, PAGE_SIZE, dir);
	if (unlikely(ib_dma_mapping_error(dev, (*req)->dma))) {
		pr_err("[ FAIL ] ib_dma_mapping_error\n");
		ret = -ENOMEM;
		kmem_cache_free(req_cache, req);
		goto out;
	}

	ib_dma_sync_single_for_device(dev, (*req)->dma, PAGE_SIZE, dir);
out:
	return ret;
}

/* XXX: same with rdma_conn.c */
/* the buffer needs to come from kernel (not high memory) */
inline static int get_req_for_buf(struct rdma_req **req, struct ib_device *dev,
		void *buf, size_t size,
		enum dma_data_direction dir)
{
	int ret;

	ret = 0;
	*req = kmem_cache_alloc(req_cache, GFP_ATOMIC);
	if (unlikely(!req)) {
		pr_err("[ FAIL ] no memory for req\n");
		ret = -ENOMEM;
		goto out;
	}

	init_completion(&(*req)->done);

	(*req)->dma = ib_dma_map_single(dev, buf, size, dir);
	if (unlikely(ib_dma_mapping_error(dev, (*req)->dma))) {
		pr_err("[ FAIL ] %s: ib_dma_mapping_error\n", __func__ );
		ret = -ENOMEM;
		kmem_cache_free(req_cache, req);
		goto out;
	}

	ib_dma_sync_single_for_device(dev, (*req)->dma, size, dir);
out:
	return ret;
}


/* polls queue until we reach target completed wrs or qp is empty */
static inline int poll_target(struct rdma_queue *q, int target)
{
	unsigned long flags;
	int completed = 0;

	while (completed < target && atomic_read(&q->pending) > 0) {
		spin_lock_irqsave(&q->cq_lock, flags);
		completed += ib_process_cq_direct(q->recv_cq, target - completed);
		spin_unlock_irqrestore(&q->cq_lock, flags);
		cpu_relax();
	}

	return completed;
}

/* post simple RR */
int post_recv(struct rdma_queue *q){
	struct ib_recv_wr wr = {};
	const struct ib_recv_wr* bad_wr;
	struct ib_sge sge = {};
	int ret;

	sge.addr = 0;
	sge.length = 0;
	sge.lkey = q->ctrl->rdev->pd->local_dma_lkey;

	wr.wr_id = 0;
	wr.sg_list = &sge;
	wr.num_sge = 0;
	wr.next = NULL;

	ret = ib_post_recv(q->qp, &wr, &bad_wr);
	if(ret){
		printk(KERN_ALERT "[%s] ib_post_recv failed\n", __func__);
		return 1;
	}
	return 0;
}

/** rdpma_put - put page into server
 *
 */
int rdpma_put(struct page *page, uint64_t key, uint32_t imm)
{
	struct rdma_queue *q;
	struct rdma_req *req[2];
	struct ib_device *dev;
	struct ib_sge sge[2];
	int ret, inflight;
	uint64_t *key_ptr;
	const struct ib_send_wr *bad_wr;
	struct ib_rdma_wr rdma_wr = {};
	int queue_id, msg_id;
	int nid, mid, type, tx_state;
	uint32_t num;
	struct ib_wc wc;
	int ne = 0;
	int cpuid = smp_processor_id();

	/* get q and its infomation */
	q = pmdfc_rdma_get_queue(cpuid, QP_WRITE_SYNC);
	queue_id = pmdfc_rdma_get_queue_id(cpuid, QP_WRITE_SYNC);
	dev = q->ctrl->rdev->dev;

	/* Protect from overrun */
	while ((inflight = atomic_read(&q->pending)) >= QP_MAX_SEND_WR - 8) {
		BUG_ON(inflight > QP_MAX_SEND_WR);
		poll_target(q, 2048);
		pr_info_ratelimited("[ WARN ] back pressure writes");
	}

	/* thi msg_id is unique in this queue */
	spin_lock(&q->queue_lock);
	msg_id = idr_alloc(&q->queue_status_idr, q, 0, 0, GFP_ATOMIC);
	spin_unlock(&q->queue_lock);

	BUG_ON(msg_id >= 64);

	/* 1. post recv */
	ret = post_recv(q);
	BUG_ON(ret);

//	pr_info("[ INFO ] %s:: pid= %d\n", __func__, smp_processor_id());

	/* 2. post send */

	/* setup imm data */
	imm = htonl(bit_mask(queue_id, msg_id, MSG_WRITE, TX_WRITE_BEGIN, 0));

	memset(sge, 0, sizeof(struct ib_sge) * 2);

	key_ptr = kzalloc(sizeof(u64), GFP_ATOMIC);
	*key_ptr = key;

	/* DMA PAGE */
	ret = get_req_for_page(&req[0], dev, page, DMA_TO_DEVICE);
	if (unlikely(ret))
		return ret;

	BUG_ON(req[0]->dma == 0);

	sge[0].addr = req[0]->dma;
	sge[0].length = PAGE_SIZE;
	sge[0].lkey = q->ctrl->rdev->pd->local_dma_lkey;

	/* DMA KEY */
	ret = get_req_for_buf(&req[1], dev, key_ptr, sizeof(uint64_t), DMA_TO_DEVICE);
	if (unlikely(ret))
		return ret;

	BUG_ON(req[1]->dma == 0);

	sge[1].addr = req[1]->dma;
	sge[1].length = sizeof(uint64_t);
	sge[1].lkey = q->ctrl->rdev->pd->local_dma_lkey;

	req[0]->cqe.done = pmdfc_rdma_send_done;

	/* TODO: add a chain of WR, we already have a list so should be easy
	 * to just post requests in batches */
	rdma_wr.wr.next    = NULL;
	rdma_wr.wr.wr_cqe  = &req[0]->cqe;
	rdma_wr.wr.sg_list = &sge[0];
	rdma_wr.wr.num_sge = 2;
	rdma_wr.wr.opcode  = IB_WR_SEND_WITH_IMM;
	rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
	rdma_wr.wr.ex.imm_data = imm;

//	atomic_inc(&q->pending);
	ret = ib_post_send(q->qp, &rdma_wr.wr, &bad_wr);
	if (unlikely(ret)) {
		pr_err("[ FAIL ] ib_post_send failed: %d\n", ret);
	}

	/* send queue polling */
	do{
		ne = ib_poll_cq(q->qp->send_cq, 1, &wc);
		if(ne < 0){
			printk(KERN_ALERT "[%s]: ib_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);

	if(wc.status != IB_WC_SUCCESS){
		printk(KERN_ALERT "[%s]: sending request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);
		return 1;
	}

	ib_dma_unmap_page(dev, req[0]->dma, PAGE_SIZE, DMA_TO_DEVICE); /* XXX for test */
	ib_dma_unmap_page(dev, req[1]->dma, sizeof(uint64_t), DMA_TO_DEVICE); /* XXX for test */
//	atomic_dec(&q->pending);
	kmem_cache_free(req_cache, req[0]);
	kmem_cache_free(req_cache, req[1]);


	/* Polling recv cq here */
	do{
		ne = ib_poll_cq(q->qp->recv_cq, 1, &wc);
		if(ne < 0){
			printk(KERN_ALERT "[%s]: ib_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);

	if(unlikely(wc.status != IB_WC_SUCCESS)){
		printk(KERN_ALERT "[%s]: recv request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);
		return 1;
	}

	bit_unmask(ntohl(wc.ex.imm_data), &nid, &mid, &type, &tx_state, &num);
	pr_info("[%s]: nid(%d), mid(%d), type(%d), tx_state(%d), num(%d)\n", __func__, nid, mid, type, tx_state, num);

	return ret;
}
EXPORT_SYMBOL_GPL(rdpma_put);

/** rdpma_get - get page from server 
 *
 * return -1 if failed
 */
int rdpma_get(struct page *page, uint64_t key)
{
	struct rdma_queue *q;
	struct rdma_req *req;
	struct ib_device *dev;
	struct ib_sge sge = { };
	struct ib_rdma_wr rdma_wr = {};
	const struct ib_send_wr *bad_wr;
	int inflight;
	u64 *key_ptr;
	int msg_id;
	uint64_t imm;
	int cpuid = smp_processor_id();
	int queue_id;
	uint64_t *addr, *raddr;
	void* dma_addr;
	uint64_t page_dma;
	struct ib_wc wc;
	int ret, ne = 0;
	int qid, mid, type, tx_state;
	uint32_t num;

	//VM_BUG_ON_PAGE(!PageSwapCache(page), page);

	/* get q and its infomation */
	q = pmdfc_rdma_get_queue(cpuid, QP_READ_SYNC);
	queue_id = pmdfc_rdma_get_queue_id(cpuid, QP_READ_SYNC);
	dev = q->ctrl->rdev->dev;

	/* thi msg_id is unique in this queue */
	spin_lock(&q->queue_lock);
	msg_id = idr_alloc(&q->queue_status_idr, q, 0, 0, GFP_ATOMIC);
	spin_unlock(&q->queue_lock);

	BUG_ON(msg_id >= 64);

#ifdef KTIME_CHECK
	fperf_start("begin_recv");
#endif
	/* 1. post recv page first to reduce RNR */
//	ret = begin_recv(q, page);
	ret = post_recv(q);
	BUG_ON(ret);

#ifdef KTIME_CHECK
	fperf_end("begin_recv");
#endif

	/* 2. post send key */
#ifdef KTIME_CHECK
	fperf_start("post_send");
#endif

	/* setup imm data */
	imm = htonl(bit_mask(queue_id, msg_id, MSG_READ, TX_WRITE_BEGIN, 0));

	/* get dma address by queue_id and msg_id */
	dma_addr = (uintptr_t)GET_LOCAL_META_REGION(gctrl->rdev->local_dma_addr, queue_id, msg_id);
	addr = (uint64_t*)GET_LOCAL_META_REGION(gctrl->rdev->local_mm, queue_id, msg_id);
	raddr = (uint64_t*)GET_REMOTE_ADDRESS_BASE(gctrl->rdev->local_mm, queue_id, msg_id);

	pr_info("[ INFO ] dma_addr=%lx, addr= %lx\n", dma_addr, (uint64_t)addr);

	/* First 8byte for key */
	*addr = key;

	/* DMA Page and write dma address to server */
	page_dma = ib_dma_map_page(dev, page, 0, PAGE_SIZE, DMA_BIDIRECTIONAL);
	if (unlikely(ib_dma_mapping_error(dev, page_dma))) {
		pr_err("[ FAIL ] ib_dma_mapping_error\n");
		ret = -ENOMEM;
		kmem_cache_free(req_cache, req);
		return -1;
	}
	ib_dma_sync_single_for_device(dev, page_dma, PAGE_SIZE, DMA_BIDIRECTIONAL);
	pr_info("[ INFO ] ib_dma_map_page { page_dma=%lx }\n", page_dma);
	BUG_ON(page_dma == 0);

	/* Next 8byte for page_dma address */
	*raddr = page_dma;
	pr_info("[ INFO ] WRITE { key=%llx, page_dma=%llx }\n", *addr, *raddr);
	pr_info("[ INFO ] Write to remote_addr= %llx\n", q->ctrl->servermr.baseaddr + GET_OFFSET_FROM_BASE(queue_id, msg_id));

	sge.addr = dma_addr;
	sge.length = METADATA_SIZE;
	sge.lkey = q->ctrl->rdev->pd->local_dma_lkey;

	/* TODO: add a chain of WR, we already have a list so should be easy
	 * to just post requests in batches */
	rdma_wr.wr.next    = NULL;
	rdma_wr.wr.sg_list = &sge;
	rdma_wr.wr.num_sge = 1;
	rdma_wr.wr.opcode  = IB_WR_RDMA_WRITE_WITH_IMM;
	rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
	rdma_wr.wr.ex.imm_data = imm;
	rdma_wr.remote_addr = q->ctrl->servermr.baseaddr + GET_OFFSET_FROM_BASE(queue_id, msg_id);
	rdma_wr.rkey = q->ctrl->servermr.key;

//	atomic_inc(&q->pending);
	ret = ib_post_send(q->qp, &rdma_wr.wr, &bad_wr);
	if (unlikely(ret)) {
		pr_err("[ FAIL ] ib_post_send failed: %d\n", ret);
	}

	/* Poll send completion queue first */
	do{
		ne = ib_poll_cq(q->qp->send_cq, 1, &wc);
		if(ne < 0){
			printk(KERN_ALERT "[%s]: ib_poll_cq failed\n", __func__);
			ret = -1;
			goto out;
		}
	}while(ne < 1);

	if(wc.status != IB_WC_SUCCESS){
		printk(KERN_ALERT "[%s]: sending request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);
		ret = -1;
		goto out;
	}

#ifdef KTIME_CHECK
	fperf_end("post_send");
#endif

#ifdef KTIME_CHECK
	fperf_start("poll_rr");
#endif
	/* Polling recv cq here */
	do{
		ne = ib_poll_cq(q->qp->recv_cq, 1, &wc);
		if(ne < 0){
			printk(KERN_ALERT "[%s]: ib_poll_cq failed\n", __func__);
			ret = -1;
			goto out;
		}
	}while(ne < 1);

	if(unlikely(wc.status != IB_WC_SUCCESS)){
		printk(KERN_ALERT "[%s]: recv request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);

		ret = -1;
		goto out;
	}

	bit_unmask(ntohl(wc.ex.imm_data), &qid, &mid, &type, &tx_state, &num);
	pr_info("[%s]: qid(%d), mid(%d), type(%d), tx_state(%d), num(%d)\n", __func__, qid, mid, type, tx_state, num);

	ib_dma_unmap_page(dev, page_dma, PAGE_SIZE, DMA_BIDIRECTIONAL);

	if ( tx_state == TX_READ_ABORTED ) {
		pr_info("TX_READ_ABORTED\n");
		ret = -1;
	} else {
		ret = 0;
	}
	
//	atomic_dec(&q->pending);

#ifdef KTIME_CHECK
	fperf_end("poll_rr");
#endif

out:
	idr_remove(&q->queue_status_idr, msg_id);

	return ret;
}
EXPORT_SYMBOL_GPL(rdpma_get);


/* XXX */
inline struct rdma_queue *pmdfc_rdma_get_queue(unsigned int cpuid,
		enum qp_type type)
{
	BUG_ON(gctrl == NULL);

	cpuid = cpuid % numqueues;
	switch (type) {
		case QP_READ_SYNC:
			if (cpuid >= numqueues / 2)
				cpuid = cpuid - (numqueues/ 2);
			return &gctrl->queues[cpuid];
		case QP_WRITE_SYNC:
			if (cpuid < numqueues / 2)
				cpuid = cpuid + (numqueues/ 2);
			return &gctrl->queues[cpuid];
		default:
			BUG();
	};
}

/* XXX */
inline int pmdfc_rdma_get_queue_id(unsigned int cpuid,
		enum qp_type type)
{
	cpuid = cpuid % numqueues;
	switch (type) {
		case QP_READ_SYNC:
			if (cpuid >= numqueues / 2)
				cpuid = cpuid - (numqueues/ 2);
			return cpuid;
		case QP_WRITE_SYNC:
			if (cpuid < numqueues / 2)
				cpuid = cpuid + (numqueues/ 2);
			return cpuid;
		default:
			BUG();
			return cpuid;
	};
}
