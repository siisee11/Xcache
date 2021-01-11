#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/slab.h>
#include <linux/cpumask.h>

#include "rdma_conn.h"
#include "rdma_op.h"

//#define SINGLE_TEST 1

extern struct pmdfc_rdma_ctrl *gctrl;
extern int numcpus;
extern struct kmem_cache *req_cache;
/* we mainly do send wrs */
extern int QP_MAX_SEND_WR;

static void pmdfc_rdma_write_done(struct ib_cq *cq, struct ib_wc *wc)
{
  struct rdma_req *req =
    container_of(wc->wr_cqe, struct rdma_req, cqe);
  struct rdma_queue *q = cq->cq_context;
  struct ib_device *ibdev = q->ctrl->rdev->dev;

  if (unlikely(wc->status != IB_WC_SUCCESS)) {
    pr_err("[ FAIL ] pmdfc_rdma_write_done status is not success, it is=%d\n", wc->status);
    //q->write_error = wc->status;
  }
  ib_dma_unmap_page(ibdev, req->dma, PAGE_SIZE, DMA_TO_DEVICE);

  atomic_dec(&q->pending);
  kmem_cache_free(req_cache, req);
}

static void pmdfc_rdma_read_done(struct ib_cq *cq, struct ib_wc *wc)
{
  struct rdma_req *req =
    container_of(wc->wr_cqe, struct rdma_req, cqe);
  struct rdma_queue *q = cq->cq_context;
  struct ib_device *ibdev = q->ctrl->rdev->dev;

  if (unlikely(wc->status != IB_WC_SUCCESS))
    pr_err("[ FAIL ] pmdfc_rdma_read_done status is not success, it is=%d\n", wc->status);

  ib_dma_unmap_page(ibdev, req->dma, PAGE_SIZE, DMA_FROM_DEVICE);

  SetPageUptodate(req->page);
  unlock_page(req->page);
  complete(&req->done);
  atomic_dec(&q->pending);
  kmem_cache_free(req_cache, req);
}

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
  //ret = ib_post_send(q->qp, &rdma_wr.wr, NULL);
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

static inline int write_queue_add(struct rdma_queue *q, struct page *page,
				  u64 roffset)
{
  struct rdma_req *req;
  struct ib_device *dev = q->ctrl->rdev->dev;
  struct ib_sge sge = {};
  int ret, inflight;

  while ((inflight = atomic_read(&q->pending)) >= QP_MAX_SEND_WR - 8) {
    BUG_ON(inflight > QP_MAX_SEND_WR);
    poll_target(q, 2048);
    pr_info_ratelimited("[ WARN ] back pressure writes");
  }

  ret = get_req_for_page(&req, dev, page, DMA_TO_DEVICE);
  if (unlikely(ret))
    return ret;

  req->cqe.done = pmdfc_rdma_write_done;
  ret = pmdfc_rdma_post_rdma(q, req, &sge, roffset, IB_WR_RDMA_WRITE);

  return ret;
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
    pr_info_ratelimited("[ WARN ] back pressure happened on reads");
  }

  ret = get_req_for_page(&req, dev, page, DMA_TO_DEVICE);
  if (unlikely(ret))
    return ret;

  req->cqe.done = pmdfc_rdma_read_done;
  ret = pmdfc_rdma_post_rdma(q, req, &sge, roffset, IB_WR_RDMA_READ);
  return ret;
}

int pmdfc_rdma_write(struct page *page, u64 roffset)
{
  int ret;
  struct rdma_queue *q;

  //VM_BUG_ON_PAGE(!PageSwapCache(page), page);
  
  q = pmdfc_rdma_get_queue(smp_processor_id(), QP_WRITE_SYNC);
 
  ret = write_queue_add(q, page, roffset);
  BUG_ON(ret);
  drain_queue(q);
  return ret;
}
EXPORT_SYMBOL_GPL(pmdfc_rdma_write);

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

int pmdfc_rdma_poll_load(int cpu)
{
  struct rdma_queue *q = pmdfc_rdma_get_queue(cpu, QP_READ_SYNC);
  return drain_queue(q);
}
EXPORT_SYMBOL(pmdfc_rdma_poll_load);

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
