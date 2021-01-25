#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/slab.h>
#include <linux/cpumask.h>
#include <linux/jiffies.h>

#include "pmdfc.h"
#include "rdma_conn.h"
#include "rdma_op.h"
#include "timeperf.h"

//#define SINGLE_TEST 1
//#define KTIME_CHECK 1

extern struct pmdfc_rdma_ctrl *gctrl;
extern int numcpus;
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
	fperf_print("drain_queue");
	fperf_print("pmdfc_rdma_read_done"); 
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

static void pmdfc_rdma_read_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct rdma_req *req =
		container_of(wc->wr_cqe, struct rdma_req, cqe);
	struct rdma_queue *q = cq->cq_context;
	struct ib_device *ibdev = q->ctrl->rdev->dev;
	int node_id, msg_num, type, tx_state;
	uint32_t num;

#ifdef KTIME_CHECK
	fperf_start(__func__);
#endif

	if (unlikely(wc->status != IB_WC_SUCCESS))
		pr_err("[ FAIL ] pmdfc_rdma_read_done status is not success, it is=%d\n", wc->status);

	bit_unmask(ntohl(wc->ex.imm_data), &node_id, &msg_num, &type, &tx_state, &num);
//	pr_info("[%s]: node_id(%d), msg_num(%d), type(%d), tx_state(%d), num(%d)\n", __func__, node_id, msg_num, type, tx_state, num);

	ib_dma_unmap_page(ibdev, req->dma, PAGE_SIZE, DMA_FROM_DEVICE);

	if ( tx_state == TX_READ_ABORTED ) {
		q->success = -1;
	} 
	q->success = 1;
//	else {
//		SetPageUptodate(req->page);
//		unlock_page(req->page);
//	}

	complete(&req->done);
	atomic_dec(&q->pending);
	kmem_cache_free(req_cache, req);
#ifdef KTIME_CHECK
	fperf_end(__func__);
#endif
}

static void pmdfc_rdma_recv_empty_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct rdma_req *req =
		container_of(wc->wr_cqe, struct rdma_req, cqe);
	struct rdma_queue *q = cq->cq_context;
	struct ib_device *ibdev = q->ctrl->rdev->dev;
	int node_id, msg_num, type, tx_state;
	uint32_t num;

	if (unlikely(wc->status != IB_WC_SUCCESS))
		pr_err("[ FAIL ] pmdfc_rdma_recv_empty_done status is not success, it is=%d\n", wc->status);
	
	bit_unmask(ntohl(wc->ex.imm_data), &node_id, &msg_num, &type, &tx_state, &num);
//	pr_info("[%s]: node_id(%d), msg_num(%d), type(%d), tx_state(%d), num(%d)\n", __func__, node_id, msg_num, type, tx_state, num);

	complete(&req->done);
	atomic_dec(&q->pending);
	kmem_cache_free(req_cache, req);
}

/**
 * pmdfc_rdma_post_rdma - post request with rdma verb
 * @q: target rdma_queue.
 * @qe: request object.
 * @sge: 
 * @roffset: remote address offset from baseaddr.
 * @op: RDMA operation code.
 * @imm_data: immediate data. 
 *
 * This function post send in batch manner.
 * Note that only last work request to be signaled.
 *
 * If generate_single_write_request succeeds, then return 0
 * if not return negative value.
 */
static inline int pmdfc_rdma_post_rdma(struct rdma_queue *q, struct rdma_req *qe,
		struct ib_sge *sge, u64 roffset, enum ib_wr_opcode op)
{
	const struct ib_send_wr *bad_wr;
	struct ib_rdma_wr rdma_wr = {};
	int ret;

	BUG_ON(qe->dma == 0);

	sge->addr = qe->dma;
	sge->length = PAGE_SIZE;
	sge->lkey = q->ctrl->rdev->pd->local_dma_lkey;

	/* TODO: add a chain of WR, we already have a list so should be easy
	 * to just post requests in batches */
	rdma_wr.wr.next    = NULL;
	rdma_wr.wr.wr_cqe  = &qe->cqe;
	rdma_wr.wr.sg_list = sge;
	rdma_wr.wr.num_sge = 1;
	rdma_wr.wr.opcode  = op;
	rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
	rdma_wr.remote_addr = q->ctrl->servermr.baseaddr + roffset;
	rdma_wr.rkey = q->ctrl->servermr.key;

	atomic_inc(&q->pending);
	ret = ib_post_send(q->qp, &rdma_wr.wr, &bad_wr);
	if (unlikely(ret)) {
		pr_err("[ FAIL ] ib_post_send failed: %d\n", ret);
	}

	/* XXX : Polling here need or need not? */

	return ret;
}

/**
 * pmdfc_rdma_post_send - post request with send
 * @q: target rdma_queue.
 * @qe: request object.
 * @sge: 
 * @roffset: remote address offset from baseaddr.
 * @op: RDMA operation code.
 * @imm_data: immediate data. 
 *
 * If generate_single_write_request succeeds, then return 0
 * if not return negative value.
 */
static inline int pmdfc_rdma_post_send(struct rdma_queue *q, struct rdma_req *qe,
		struct ib_sge *sge, enum ib_wr_opcode op, uint32_t imm_data)
{
	const struct ib_send_wr *bad_wr;
	struct ib_rdma_wr rdma_wr = {};
	int ret;

	BUG_ON(qe->dma == 0);

	/* TODO: add a chain of WR, we already have a list so should be easy
	 * to just post requests in batches */
	rdma_wr.wr.next    = NULL;
	rdma_wr.wr.wr_cqe  = &qe->cqe;
	rdma_wr.wr.sg_list = sge;
	rdma_wr.wr.num_sge = 2;
	rdma_wr.wr.opcode  = op;
	rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
	rdma_wr.wr.ex.imm_data = imm_data;

	atomic_inc(&q->pending);
	ret = ib_post_send(q->qp, &rdma_wr.wr, &bad_wr);
	if (unlikely(ret)) {
		pr_err("[ FAIL ] ib_post_send failed: %d\n", ret);
	}

	/* XXX : Polling here need or need not? */

	return ret;
}

/**
 * pmdfc_rdma_post_recv - post recv request 
 */
static inline int pmdfc_rdma_post_recv(struct rdma_queue *q, struct rdma_req *qe,
		struct ib_sge *sge)
{
	const struct ib_recv_wr *bad_wr;
	struct ib_recv_wr wr = {};
	int ret;

	sge->addr = qe->dma;
	sge->length = PAGE_SIZE;
	sge->lkey = q->ctrl->rdev->pd->local_dma_lkey;

	/* TODO: add a chain of WR, we already have a list so should be easy
	 * to just post requests in batches */
	wr.next    = NULL;
	wr.wr_cqe  = &qe->cqe;
	wr.sg_list = sge;
	wr.num_sge = 1;

	atomic_inc(&q->pending);
	ret = ib_post_recv(q->qp, &wr, &bad_wr);
	if (unlikely(ret)) {
		pr_err("[ FAIL ] ib_post_send failed: %d\n", ret);
	}

	return ret;
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

	/**
	 * ib_dma_map_page - Map a physical page to DMA address
	 * @dev: The device for which the dma_addr is to be created
	 * @page: The page to be mapped
	 * @offset: The offset within the page
	 * @size: The size of the region in bytes
	 * @direction: The direction of the DMA
	 */
	(*req)->dma = ib_dma_map_page(dev, page, 0, PAGE_SIZE, dir);
	if (unlikely(ib_dma_mapping_error(dev, (*req)->dma))) {
		pr_err("[ FAIL ] ib_dma_mapping_error\n");
		ret = -ENOMEM;
		kmem_cache_free(req_cache, req);
		goto out;
	}


	/**
	 * ib_dma_sync_single_for_device - Prepare DMA region to be accessed by device
	 * @dev: The device for which the DMA address was created
	 * @addr: The DMA address
	 * @size: The size of the region in bytes
	 * @dir: The direction of the DMA
	 */
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
		completed += ib_process_cq_direct(q->cq, target - completed);
		spin_unlock_irqrestore(&q->cq_lock, flags);
		cpu_relax();
	}

	return completed;
}

static inline int drain_queue(struct rdma_queue *q)
{
	unsigned long flags;

	while (atomic_read(&q->pending) > 0) {
		spin_lock_irqsave(&q->cq_lock, flags);
		ib_process_cq_direct(q->cq, 16);
		spin_unlock_irqrestore(&q->cq_lock, flags);
		cpu_relax();
	}

	return 1;
}

static inline int begin_read(struct rdma_queue *q, struct page *page,
		u64 roffset)
{
	struct rdma_req *req;
	struct ib_device *dev = q->ctrl->rdev->dev;
	struct ib_sge sge = {};
	int ret, inflight;

	/* back pressure in-flight reads, can't send more than
	 * QP_MAX_SEND_WR at a time */
	while ((inflight = atomic_read(&q->pending)) >= QP_MAX_SEND_WR) {
		BUG_ON(inflight > QP_MAX_SEND_WR); /* only valid case is == */
		//poll_target(q, 8);
		poll_target(q, 2048);
		pr_info_ratelimited("[ warn ] back pressure happened on reads");
	}

	ret = get_req_for_page(&req, dev, page, DMA_TO_DEVICE);
	if (unlikely(ret))
		return ret;

	req->cqe.done = pmdfc_rdma_read_done;
	ret = pmdfc_rdma_post_rdma(q, req, &sge, roffset, IB_WR_RDMA_READ);
	return ret;
}

/* XXX */
static inline int begin_recv(struct rdma_queue *q, struct page *page)
{
	struct rdma_req *req;
	struct ib_device *dev = q->ctrl->rdev->dev;
	struct ib_sge sge = {};
	int ret, inflight;
	const struct ib_recv_wr *bad_wr;
	struct ib_recv_wr wr = {};

	/* back pressure in-flight reads, can't send more than
	 * QP_MAX_SEND_WR at a time */
	while ((inflight = atomic_read(&q->pending)) >= QP_MAX_SEND_WR) {
		BUG_ON(inflight > QP_MAX_SEND_WR); /* only valid case is == */
		//poll_target(q, 8);
		poll_target(q, 2048);
		pr_info_ratelimited("[ WARN ] back pressure happened on reads");
	}

	ret = get_req_for_page(&req, dev, page, DMA_FROM_DEVICE); /* XXX DMA_FROM_DEVICE or DMA_TO_DEVICE no effect*/
	if (unlikely(ret))
		return ret;

	req->cqe.done = pmdfc_rdma_read_done;
//	ret = pmdfc_rdma_post_recv(q, req, &sge);

	sge.addr = req->dma;
	sge.length = PAGE_SIZE;
	sge.lkey = q->ctrl->rdev->pd->local_dma_lkey;

	/* TODO: add a chain of WR, we already have a list so should be easy
	 * to just post requests in batches */
	wr.next    = NULL;
	wr.wr_cqe  = &req->cqe;
	wr.sg_list = &sge;
	wr.num_sge = 1;

	atomic_inc(&q->pending);
	ret = ib_post_recv(q->qp, &wr, &bad_wr);
	if (unlikely(ret)) {
		pr_err("[ FAIL ] ib_post_send failed: %d\n", ret);
	}

	return ret;
}

/* XXX */
static inline int begin_recv_empty(struct rdma_queue *q)
{
	struct rdma_req *req;
	struct ib_device *dev = q->ctrl->rdev->dev;
	struct ib_sge sge = {};
	int ret, inflight;
	const struct ib_recv_wr *bad_wr;
	struct ib_recv_wr wr = {};

	/* back pressure in-flight reads, can't send more than
	 * QP_MAX_SEND_WR at a time */
	while ((inflight = atomic_read(&q->pending)) >= QP_MAX_SEND_WR) {
		BUG_ON(inflight > QP_MAX_SEND_WR); /* only valid case is == */
		//poll_target(q, 8);
		poll_target(q, 2048);
		pr_info_ratelimited("[ WARN ] back pressure happened on reads");
	}

	req = kmem_cache_alloc(req_cache, GFP_ATOMIC);
	if (unlikely(!req)) {
		pr_err("[ FAIL ] no memory for req\n");
		ret = -ENOMEM;
		return ret;
	}

	init_completion(&(req)->done);
	req->cqe.done = pmdfc_rdma_recv_empty_done;

	sge.addr = 0;
	sge.length = 0;
	sge.lkey = q->ctrl->rdev->pd->local_dma_lkey;

	/* TODO: add a chain of WR, we already have a list so should be easy
	 * to just post requests in batches */
	wr.next    = NULL;
	wr.wr_cqe  = &req->cqe;
	wr.sg_list = &sge;
	wr.num_sge = 0;

	atomic_inc(&q->pending);
	ret = ib_post_recv(q->qp, &wr, &bad_wr);
	if (unlikely(ret)) {
		pr_err("[ FAIL ] ib_post_send failed: %d\n", ret);
	}

	return ret;
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

	//VM_BUG_ON_PAGE(!PageSwapCache(page), page);

	q = pmdfc_rdma_get_queue(smp_processor_id(), QP_WRITE_SYNC);
	dev = q->ctrl->rdev->dev; 

	/* 2. post recv */
	ret = begin_recv_empty(q);
	BUG_ON(ret);

//	pr_info("[ INFO ] %s:: pid= %d\n", __func__, smp_processor_id());

	while ((inflight = atomic_read(&q->pending)) >= QP_MAX_SEND_WR - 8) {
		BUG_ON(inflight > QP_MAX_SEND_WR);
		poll_target(q, 2048);
		pr_info_ratelimited("[ WARN ] back pressure writes");
	}

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

	atomic_inc(&q->pending);
	ret = ib_post_send(q->qp, &rdma_wr.wr, &bad_wr);
	if (unlikely(ret)) {
		pr_err("[ FAIL ] ib_post_send failed: %d\n", ret);
	}

	BUG_ON(ret);


	drain_queue(q);

	return ret;
}
EXPORT_SYMBOL_GPL(rdpma_put);

/* XXX */
int rdpma_get(struct page *page, uint64_t key, uint32_t imm)
{
	struct rdma_queue *q;
	struct rdma_req *req;
	struct ib_device *dev;
	struct ib_sge sge = { };
	struct ib_rdma_wr rdma_wr = {};
	const struct ib_send_wr *bad_wr;
	int ret, inflight;
	u64 *key_ptr;

	//VM_BUG_ON_PAGE(!PageSwapCache(page), page);

//	pr_info("[ INFO ] %s:: pid= %d\n", __func__, smp_processor_id());
	q = pmdfc_rdma_get_queue(smp_processor_id(), QP_READ_SYNC);
	dev = q->ctrl->rdev->dev;

#ifdef KTIME_CHECK
	fperf_start("begin_recv");
#endif
	/* 1. post recv page first to reduce RNR */
//	ret = begin_recv(q, page);
	ret = begin_recv_empty(q);
	BUG_ON(ret);

#ifdef KTIME_CHECK
	fperf_end("begin_recv");
#endif

	/* 2. post send key */

	while ((inflight = atomic_read(&q->pending)) >= QP_MAX_SEND_WR - 8) {
		BUG_ON(inflight > QP_MAX_SEND_WR);
		poll_target(q, 2048);
		pr_info_ratelimited("[ WARN ] back pressure writes");
	}

#ifdef KTIME_CHECK
	fperf_start("post_send");
#endif
	key_ptr = kzalloc(sizeof(u64), GFP_ATOMIC);
	*key_ptr = key;

	/* DMA KEY */
	ret = get_req_for_buf(&req, dev, key_ptr, sizeof(uint64_t), DMA_TO_DEVICE);
	if (unlikely(ret))
		return ret;

	req->cqe.done = pmdfc_rdma_send_done;

	BUG_ON(req->dma == 0);

	sge.addr = req->dma;
	sge.length = sizeof(uint64_t);
	sge.lkey = q->ctrl->rdev->pd->local_dma_lkey;

	/* TODO: add a chain of WR, we already have a list so should be easy
	 * to just post requests in batches */
	rdma_wr.wr.next    = NULL;
	rdma_wr.wr.wr_cqe  = &req->cqe;
	rdma_wr.wr.sg_list = &sge;
	rdma_wr.wr.num_sge = 1;
	rdma_wr.wr.opcode  = IB_WR_SEND_WITH_IMM;
	rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
	rdma_wr.wr.ex.imm_data = imm;

	atomic_inc(&q->pending);
	ret = ib_post_send(q->qp, &rdma_wr.wr, &bad_wr);
	if (unlikely(ret)) {
		pr_err("[ FAIL ] ib_post_send failed: %d\n", ret);
	}
#ifdef KTIME_CHECK
	fperf_end("post_send");
#endif
	BUG_ON(ret);

#ifdef KTIME_CHECK
	fperf_start("drain_queue");
#endif
	drain_queue(q);
#ifdef KTIME_CHECK
	fperf_end("drain_queue");
#endif
	/* page not found */
	if (q->success == -1) {
		q->success = 1;
		return -1;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(rdpma_get);


/* page is unlocked when the wr is done.
 * posts an RDMA read on this cpu's qp */
int pmdfc_rdma_read_async(struct page *page, u64 roffset)
{
	struct rdma_queue *q;
	int ret;

	//VM_BUG_ON_PAGE(!PageSwapCache(page), page);
	//VM_BUG_ON_PAGE(!PageLocked(page), page);
	//VM_BUG_ON_PAGE(PageUptodate(page), page);

	q = pmdfc_rdma_get_queue(smp_processor_id(), QP_READ_ASYNC);

	ret = begin_read(q, page, roffset);
	return ret;
}
EXPORT_SYMBOL_GPL(pmdfc_rdma_read_async);

int pmdfc_rdma_read_sync(struct page *page, u64 roffset)
{
	struct rdma_queue *q;
	int ret;

	//VM_BUG_ON_PAGE(!PageSwapCache(page), page);
	//VM_BUG_ON_PAGE(!PageLocked(page), page);
	//VM_BUG_ON_PAGE(PageUptodate(page), page);

	q = pmdfc_rdma_get_queue(smp_processor_id(), QP_READ_SYNC);
	ret = begin_read(q, page, roffset);
	return ret;
}
EXPORT_SYMBOL_GPL(pmdfc_rdma_read_sync);


inline struct rdma_queue *pmdfc_rdma_get_queue(unsigned int cpuid,
		enum qp_type type)
{
	BUG_ON(gctrl == NULL);

#ifdef SINGLE_TEST
	switch (type) {
		case QP_READ_SYNC:
			return &gctrl->queues[0];
		case QP_WRITE_SYNC:
			return &gctrl->queues[1];
		default:
			BUG();
	};
#else
	switch (type) {
		case QP_READ_SYNC:
			if (cpuid >= numcpus / 2)
				cpuid = cpuid - (numcpus / 2);
			return &gctrl->queues[cpuid];
		case QP_WRITE_SYNC:
			if (cpuid < numcpus / 2)
				cpuid = cpuid + (numcpus / 2);
			return &gctrl->queues[cpuid];
		default:
			BUG();
	};
#endif
}
