#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/slab.h>
#include <linux/cpumask.h>
#include "rdma_conn.h"

struct pmdfc_rdma_ctrl *gctrl;
static int serverport;
static int numqueues;
int numcpus;
static char serverip[INET_ADDRSTRLEN];
static char clientip[INET_ADDRSTRLEN];
struct kmem_cache *req_cache;

long mr_free_end;
EXPORT_SYMBOL_GPL(mr_free_end);

module_param_named(sport, serverport, int, 0644);
module_param_named(nq, numqueues, int, 0644);
module_param_string(sip, serverip, INET_ADDRSTRLEN, 0644);
module_param_string(cip, clientip, INET_ADDRSTRLEN, 0644);


#ifdef SINGLE_TEST
static int SINGLE_QUEUE = 2;
#endif
// TODO: destroy ctrl

#define CONNECTION_TIMEOUT_MS 60000
#define QP_QUEUE_DEPTH 256
/* we don't really use recv wrs, so any small number should do */
#define QP_MAX_RECV_WR 4096
/* we mainly do send wrs */
int QP_MAX_SEND_WR = 4096;
#define CQ_NUM_CQES	(QP_MAX_SEND_WR)
#define POLL_BATCH_HIGH (QP_MAX_SEND_WR / 4)

static void pmdfc_rdma_addone(struct ib_device *dev)
{
	//  pr_info("[ INFO ] pmdfc_rdma_addone() = %s\n", dev->name);
	return;
}

static void pmdfc_rdma_removeone(struct ib_device *ib_device, void *client_data)
{
//	pr_info("[ INFO ] pmdfc_rdma_removeone()\n");
	return;
}

static struct ib_client pmdfc_rdma_ib_client = {
	.name   = "pmdfc_rdma",
	.add    = pmdfc_rdma_addone,
	.remove = pmdfc_rdma_removeone
};

static struct pmdfc_rdma_dev *pmdfc_rdma_get_device(struct rdma_queue *q)
{
	struct pmdfc_rdma_dev *rdev = NULL;

	if (!q->ctrl->rdev) {
		rdev = kzalloc(sizeof(*rdev), GFP_KERNEL);
		if (!rdev) {
			pr_err("[ FAIL ] no memory\n");
			goto out_err;
		}

		rdev->dev = q->cm_id->device;

		pr_info("[ INFO ] selecting device %s\n", rdev->dev->name);

#ifdef MLNX_OFED
		rdev->pd = ib_alloc_pd(rdev->dev); // protection domain
#else
		rdev->pd = ib_alloc_pd(rdev->dev, 0); // protection domain
#endif

		if (IS_ERR(rdev->pd)) {
			pr_err("[ FAIL ] ib_alloc_pd\n");
			goto out_free_dev;
		}

		if (!(rdev->dev->attrs.device_cap_flags &
					IB_DEVICE_MEM_MGT_EXTENSIONS)) {
			pr_err("[ FAIL ] memory registrations not supported\n");
			goto out_free_pd;
		}

		/* XXX: allocate memory region here */
		rdev->mr = rdev->pd->device->ops.get_dma_mr(rdev->pd, IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_READ);
		rdev->mr->pd = rdev->pd;
		rdev->mr_size = LOCAL_META_REGION_SIZE;
		rdev->local_mm = (uint64_t)kmalloc(rdev->mr_size, GFP_KERNEL);
		//	rdev->local_dma_addr = ib_reg_mr_addr((void*)rdev->local_mm, rdev->mr_size);
		rdev->local_dma_addr = ib_dma_map_single(rdev->dev, (void *)rdev->local_mm, rdev->mr_size, DMA_BIDIRECTIONAL);
		if (unlikely(ib_dma_mapping_error(rdev->dev, rdev->local_dma_addr))) {
			ib_dma_unmap_single(rdev->dev,
					rdev->local_dma_addr, rdev->mr_size, DMA_BIDIRECTIONAL);
			return -ENOMEM;
		}

		q->ctrl->clientmr.key = rdev->pd->local_dma_lkey;
		//	q->ctrl->clientmr->key = rdev->mr.rkey;
		q->ctrl->clientmr.baseaddr = rdev->local_dma_addr;
		q->ctrl->clientmr.mr_size = rdev->mr_size;

		pr_info("[ INFO ] local mr: key(%u), baseaddr(%llx), mr_size(%llu)\n", 
				q->ctrl->clientmr.key,
				q->ctrl->clientmr.baseaddr,
				q->ctrl->clientmr.mr_size);

		q->ctrl->rdev = rdev;
	}

	return q->ctrl->rdev;

out_free_pd:
	ib_dealloc_pd(rdev->pd);
out_free_dev:
	kfree(rdev);
out_err:
	return NULL;
}

static void pmdfc_rdma_qp_event(struct ib_event *e, void *c)
{
//	pr_info("pmdfc_rdma_qp_event\n");
	return ;
}

static int pmdfc_rdma_create_qp(struct rdma_queue *queue)
{
	struct pmdfc_rdma_dev *rdev = queue->ctrl->rdev;
	struct ib_qp_init_attr init_attr;
	int ret;

	//  pr_info("[ INFO ] start: %s\n", __FUNCTION__);

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.event_handler = pmdfc_rdma_qp_event;
	init_attr.cap.max_send_wr = QP_MAX_SEND_WR;
	init_attr.cap.max_recv_wr = QP_MAX_RECV_WR;
	init_attr.cap.max_recv_sge = 2; /* XXX */
	init_attr.cap.max_send_sge = 2; /* XXX */
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;
	//  init_attr.qp_type = IB_QPT_UD; /* XXX: RC->UC */
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = queue->cq;
	init_attr.recv_cq = queue->cq;
	/* just to check if we are compiling against the right headers */
	//init_attr.create_flags = IB_QP_EXP_CREATE_ATOMIC_BE_REPLY & 0;

	ret = rdma_create_qp(queue->cm_id, rdev->pd, &init_attr);
	if (ret) {
		pr_err("[ FAIL ] rdma_create_qp failed: %d\n", ret);
		return ret;
	}

	queue->qp = queue->cm_id->qp;
	return ret;
}

static void pmdfc_rdma_destroy_queue_ib(struct rdma_queue *q)
{
	struct pmdfc_rdma_dev *rdev;
	struct ib_device *ibdev;

	//  pr_info("start: %s\n", __FUNCTION__);

	rdev = q->ctrl->rdev;
	ibdev = rdev->dev;
	//rdma_destroy_qp(q->ctrl->cm_id);
	ib_free_cq(q->cq);
}

static int pmdfc_rdma_create_queue_ib(struct rdma_queue *q)
{
	struct ib_device *ibdev = q->ctrl->rdev->dev;
	int ret;
	int comp_vector = 0;

	//  pr_info("[ INFO ] start: %s\n", __FUNCTION__);

	/**
	 * __ib_alloc_cq        allocate a completion queue
	 * @dev:		device to allocate the CQ for
	 * @private:		driver private data, accessible from cq->cq_context
	 * @nr_cqe:		number of CQEs to allocate
	 * @comp_vector:	HCA completion vectors for this CQ
	 * @poll_ctx:		context to poll the CQ from.
	 *
	 * This is the proper interface to allocate a CQ for in-kernel users. A
	 * CQ allocated with this interface will automatically be polled from the
	 * specified context. The ULP must use wr->wr_cqe instead of wr->wr_id
	 * to use this CQ abstraction.
	 */
	if (q->qp_type == QP_READ_ASYNC)
		q->cq = ib_alloc_cq(ibdev, q, CQ_NUM_CQES, comp_vector, IB_POLL_SOFTIRQ);
	else
	    q->cq = ib_alloc_cq(ibdev, q, CQ_NUM_CQES, comp_vector, IB_POLL_DIRECT);
//		q->cq = ib_alloc_cq(ibdev, q, CQ_NUM_CQES, comp_vector, IB_POLL_SOFTIRQ);

	if (IS_ERR(q->cq)) {
		ret = PTR_ERR(q->cq);
		goto out_err;
	}

	ret = pmdfc_rdma_create_qp(q);
	if (ret)
		goto out_destroy_ib_cq;

	return 0;

out_destroy_ib_cq:
	ib_free_cq(q->cq);
out_err:
	return ret;
}

static int pmdfc_rdma_addr_resolved(struct rdma_queue *q)
{
	struct pmdfc_rdma_dev *rdev = NULL;
	int ret;

	//  pr_info("[ INFO ] start: %s\n", __FUNCTION__);

	rdev = pmdfc_rdma_get_device(q);
	if (!rdev) {
		pr_err("[ FAIL ] no device found\n");
		return -ENODEV;
	}

	ret = pmdfc_rdma_create_queue_ib(q);
	if (ret) {
		return ret;
	}

	ret = rdma_resolve_route(q->cm_id, CONNECTION_TIMEOUT_MS);
	if (ret) {
		pr_err("[ FAIL ] rdma_resolve_route failed\n");
		pmdfc_rdma_destroy_queue_ib(q);
	}

	return 0;
}

static int pmdfc_rdma_route_resolved(struct rdma_queue *q,
		struct rdma_conn_param *conn_params)
{
	struct rdma_conn_param param = {};
	int ret;

	param.qp_num = q->qp->qp_num;
	param.flow_control = 1;
	param.responder_resources = 16;
	param.initiator_depth = 16;
	param.retry_count = 7;
	param.rnr_retry_count = 7; /* XXX: 7 -> 0 */
	param.private_data = NULL;
	param.private_data_len = 0;

//	pr_info("[ INFO ] max_qp_rd_atom=%d max_qp_init_rd_atom=%d\n", q->ctrl->rdev->dev->attrs.max_qp_rd_atom, q->ctrl->rdev->dev->attrs.max_qp_init_rd_atom);

	ret = rdma_connect(q->cm_id, &param);
	if (ret) {
		pr_err("[ FAILED ] rdma_connect failed (%d)\n", ret);
		pmdfc_rdma_destroy_queue_ib(q);
	}

	return 0;
}

static int pmdfc_rdma_conn_established(struct rdma_queue *q)
{
	//  pr_info("[ INFO ] connection established\n");
	return 0;
}

static int pmdfc_rdma_cm_handler(struct rdma_cm_id *cm_id,
		struct rdma_cm_event *ev)
{
	struct rdma_queue *queue = cm_id->context;
	int cm_error = 0;

//	pr_info("[ INFO ] cm_handler msg: %s (%d) status %d id %p\n", rdma_event_msg(ev->event), ev->event, ev->status, cm_id);

	switch (ev->event) {
		case RDMA_CM_EVENT_ADDR_RESOLVED:
			cm_error = pmdfc_rdma_addr_resolved(queue);
			break;
		case RDMA_CM_EVENT_ROUTE_RESOLVED:
			cm_error = pmdfc_rdma_route_resolved(queue, &ev->param.conn);
			break;
		case RDMA_CM_EVENT_ESTABLISHED:
			queue->cm_error = pmdfc_rdma_conn_established(queue);
			/* complete cm_done regardless of success/failure */
			complete(&queue->cm_done);
			return 0;
		case RDMA_CM_EVENT_REJECTED:
			pr_err("[ FAIL ] connection rejected\n");
			break;
		case RDMA_CM_EVENT_ADDR_ERROR:
		case RDMA_CM_EVENT_ROUTE_ERROR:
		case RDMA_CM_EVENT_CONNECT_ERROR:
		case RDMA_CM_EVENT_UNREACHABLE:
			pr_err("[ FAIL ] CM error event %d\n", ev->event);
			cm_error = -ECONNRESET;
			break;
		case RDMA_CM_EVENT_DISCONNECTED:
		case RDMA_CM_EVENT_ADDR_CHANGE:
		case RDMA_CM_EVENT_TIMEWAIT_EXIT:
			pr_err("[ FAIL ] CM connection closed %d\n", ev->event);
			break;
		case RDMA_CM_EVENT_DEVICE_REMOVAL:
			/* device removal is handled via the ib_client API */
			break;
		default:
			pr_err("[ FAIL ] CM unexpected event: %d\n", ev->event);
			break;
	}

	if (cm_error) {
		queue->cm_error = cm_error;
		complete(&queue->cm_done);
	}

	return 0;
}

inline static int pmdfc_rdma_wait_for_cm(struct rdma_queue *queue)
{
	wait_for_completion_interruptible_timeout(&queue->cm_done,
			msecs_to_jiffies(CONNECTION_TIMEOUT_MS) + 1);
	return queue->cm_error;
}

static int pmdfc_rdma_init_queue(struct pmdfc_rdma_ctrl *ctrl,
		int idx)
{
	struct rdma_queue *queue;
	int ret;

	//  pr_info("[ INFO ] start: %s\n", __FUNCTION__);

	queue = &ctrl->queues[idx];
	queue->ctrl = ctrl; // point each other (queue, ctrl)
	init_completion(&queue->cm_done);
	atomic_set(&queue->pending, 0);
	spin_lock_init(&queue->cq_lock);
	queue->qp_type = get_queue_type(idx);

	queue->cm_id = rdma_create_id(&init_net, pmdfc_rdma_cm_handler, queue,
			RDMA_PS_TCP, IB_QPT_RC); // start rdma_cm XXX
	if (IS_ERR(queue->cm_id)) {
		pr_err("[ FAIL ] failed to create cm id: %ld\n", PTR_ERR(queue->cm_id));
		return -ENODEV;
	}

	queue->cm_error = -ETIMEDOUT;

	ret = rdma_resolve_addr(queue->cm_id, &ctrl->srcaddr, &ctrl->addr,
			CONNECTION_TIMEOUT_MS); // send to server
	if (ret) {
		pr_err("[ FAIL ] rdma_resolve_addr failed: %d\n", ret);
		goto out_destroy_cm_id;
	}

	ret = pmdfc_rdma_wait_for_cm(queue);
	if (ret) {
		pr_err("[ FAIL ] pmdfc_rdma_wait_for_cm failed\n");
		goto out_destroy_cm_id;
	}

	return 0;

out_destroy_cm_id:
	rdma_destroy_id(queue->cm_id);
	return ret;
}

static void pmdfc_rdma_stop_queue(struct rdma_queue *q)
{
	rdma_disconnect(q->cm_id);
}

static void pmdfc_rdma_free_queue(struct rdma_queue *q)
{
	rdma_destroy_qp(q->cm_id);
	ib_free_cq(q->cq);
	rdma_destroy_id(q->cm_id);
}

static int pmdfc_rdma_init_queues(struct pmdfc_rdma_ctrl *ctrl)
{
	int ret, i;
#ifdef SINGLE_TEST
	// for test, 0: read_sync, 1: write_sync
	for (i = 0; i < SINGLE_QUEUE; ++i) {
		ret = pmdfc_rdma_init_queue(ctrl, i);
		if (ret) {
			pr_err("[ FAIL ] failed to initialized queue: %d\n", i);
			goto out_free_queues;
		}
	}
#else
	// usually numqueues is same as numcpus
	for (i = 0; i < numqueues; ++i) {
		ret = pmdfc_rdma_init_queue(ctrl, i);
		if (ret) {
			pr_err("[ FAIL ] failed to initialized queue: %d\n", i);
			goto out_free_queues;
		}
	}
#endif

	return 0;

out_free_queues:
	for (i--; i >= 0; i--) {
		pmdfc_rdma_stop_queue(&ctrl->queues[i]);
		pmdfc_rdma_free_queue(&ctrl->queues[i]);
	}

	return ret;
}

static void pmdfc_rdma_stopandfree_queues(struct pmdfc_rdma_ctrl *ctrl)
{
	int i;
	for (i = 0; i < numqueues; ++i) {
		pmdfc_rdma_stop_queue(&ctrl->queues[i]);
		pmdfc_rdma_free_queue(&ctrl->queues[i]);
	}
}

static int pmdfc_rdma_parse_ipaddr(struct sockaddr_in *saddr, char *ip)
{
	u8 *addr = (u8 *)&saddr->sin_addr.s_addr;
	size_t buflen = strlen(ip);

	//  pr_info("[ INFO ] start: %s\n", __FUNCTION__);

	if (buflen > INET_ADDRSTRLEN)
		return -EINVAL;
	if (in4_pton(ip, buflen, addr, '\0', NULL) == 0)
		return -EINVAL;
	saddr->sin_family = AF_INET;
	return 0;
}

static int pmdfc_rdma_create_ctrl(struct pmdfc_rdma_ctrl **c)
{
	int ret;
	struct pmdfc_rdma_ctrl *ctrl;
	//  pr_info("[ INFO ] will try to connect to %s:%d\n", serverip, serverport); // from module parm

	*c = kzalloc(sizeof(struct pmdfc_rdma_ctrl), GFP_KERNEL); // global ctrl
	if (!*c) {
		pr_err("[ FAIL ] no mem for ctrl\n");
		return -ENOMEM;
	}
	ctrl = *c;

	ctrl->queues = kzalloc(sizeof(struct rdma_queue) * numqueues, GFP_KERNEL);
	ret = pmdfc_rdma_parse_ipaddr(&(ctrl->addr_in), serverip);
	if (ret) {
		pr_err("[ FAIL ] pmdfc_rdma_parse_ipaddr, serverip failed: %d\n", ret);
		return -EINVAL;
	}
	ctrl->addr_in.sin_port = cpu_to_be16(serverport);

	ret = pmdfc_rdma_parse_ipaddr(&(ctrl->srcaddr_in), clientip);
	if (ret) {
		pr_err("[ FAIL ] pmdfc_rdma_parse_ipaddr, clinetip failed: %d\n", ret);
		return -EINVAL;
	}
	/* no need to set the port on the srcaddr */

	return pmdfc_rdma_init_queues(ctrl);
}

static void __exit rdma_connection_cleanup_module(void)
{
	pmdfc_rdma_stopandfree_queues(gctrl);
	ib_unregister_client(&pmdfc_rdma_ib_client);
	kfree(gctrl);
	gctrl = NULL;
	if (req_cache) {
		kmem_cache_destroy(req_cache);
	}
}

static void pmdfc_rdma_recv_remotemr_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct rdma_req *qe =
		container_of(wc->wr_cqe, struct rdma_req, cqe);
	struct rdma_queue *q = cq->cq_context;
	struct pmdfc_rdma_ctrl *ctrl = q->ctrl;
	struct ib_device *ibdev = q->ctrl->rdev->dev;

	if (unlikely(wc->status != IB_WC_SUCCESS)) {
		pr_err("[ FAIL ] pmdfc_rdma_recv_done status is not success\n");
		return;
	}
	ib_dma_unmap_single(ibdev, qe->dma, sizeof(struct pmdfc_rdma_memregion),
			DMA_FROM_DEVICE); 
	mr_free_end = ctrl->servermr.mr_size;

	pr_info("[ INFO ] servermr baseaddr=%llx, key=%u, mr_size=%lld (GB) n", ctrl->servermr.baseaddr,
			ctrl->servermr.key, ctrl->servermr.mr_size/1024/1024/1024);
	complete_all(&qe->done);
}

/* XXX */
static void pmdfc_rdma_send_localmr_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct rdma_req *qe =
		container_of(wc->wr_cqe, struct rdma_req, cqe);
	struct rdma_queue *q = cq->cq_context;
	struct pmdfc_rdma_ctrl *ctrl = q->ctrl;
	struct ib_device *ibdev = q->ctrl->rdev->dev;

	if (unlikely(wc->status != IB_WC_SUCCESS)) {
		pr_err("[ FAIL ] pmdfc_rdma_recv_done status is not success\n");
		return;
	}
	ib_dma_unmap_single(ibdev, qe->dma, sizeof(struct pmdfc_rdma_memregion),
			DMA_FROM_DEVICE); 
	mr_free_end = ctrl->servermr.mr_size;

	pr_info("[ INFO ] localmr baseaddr=%llx, key=%u, mr_size=%lld (GB)\n", ctrl->clientmr.baseaddr,
			ctrl->clientmr.key, ctrl->clientmr.mr_size/1024/1024/1024);
	complete_all(&qe->done);
}

static int pmdfc_rdma_post_recv(struct rdma_queue *q, struct rdma_req *qe,
		size_t bufsize)
{
	const struct ib_recv_wr *bad_wr;
	struct ib_recv_wr wr = {};
	struct ib_sge sge;
	int ret;

	sge.addr = qe->dma;
	sge.length = bufsize;
	sge.lkey = q->ctrl->rdev->pd->local_dma_lkey;

	wr.next    = NULL;
	wr.wr_cqe  = &qe->cqe;
	wr.sg_list = &sge;
	wr.num_sge = 1;

	ret = ib_post_recv(q->qp, &wr, &bad_wr);
	if (ret) {
		pr_err("[ FAIL ] ib_post_recv failed: %d\n", ret);
	}
	return ret;
}

/* XXX */
static int pmdfc_rdma_post_send(struct rdma_queue *q, struct rdma_req *qe,
		size_t bufsize)
{
	const struct ib_send_wr *bad_wr;
	struct ib_send_wr wr = {};
	struct ib_sge sge;
	int ret;

	sge.addr = qe->dma;
	sge.length = bufsize;
	sge.lkey = q->ctrl->rdev->pd->local_dma_lkey;

	wr.next    = NULL;
	wr.wr_cqe  = &qe->cqe;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.opcode     = IB_WR_SEND;
	wr.send_flags = IB_SEND_SIGNALED;

	ret = ib_post_send(q->qp, &wr, &bad_wr);
	if (ret) {
		pr_err("[ FAIL ] ib_post_recv failed: %d\n", ret);
	}
	return ret;
}

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
		pr_err("[ FAIL ] ib_dma_mapping_error\n");
		ret = -ENOMEM;
		kmem_cache_free(req_cache, req);
		goto out;
	}

	ib_dma_sync_single_for_device(dev, (*req)->dma, size, dir);
out:
	return ret;
}

inline static void pmdfc_rdma_wait_completion(struct ib_cq *cq,
		struct rdma_req *qe)
{
	ndelay(1000);
	while (!completion_done(&qe->done)) {
		ndelay(250);
		/**
		 * ib_process_direct_cq - process a CQ in caller context
		 * @cq:		CQ to process
		 * @budget:	number of CQEs to poll for
		 *
		 * This function is used to process all outstanding CQ entries on a
		 * %IB_POLL_DIRECT CQ.  It does not offload CQ processing to a different
		 * context and does not ask for completion interrupts from the HCA.
		 *
		 * Note: for compatibility reasons -1 can be passed in %budget for unlimited
		 * polling.  Do not use this feature in new code, it will be removed soon.
		 */
		ib_process_cq_direct(cq, 1);
	}
}

static int pmdfc_rdma_recv_remotemr(struct pmdfc_rdma_ctrl *ctrl)
{
	struct rdma_req *qe;
	int ret;
	struct ib_device *dev;

	//  pr_info("[ INFO ] start: %s\n", __FUNCTION__);
	dev = ctrl->rdev->dev;

	ret = get_req_for_buf(&qe, dev, &(ctrl->servermr), sizeof(ctrl->servermr),
			DMA_FROM_DEVICE);
	if (unlikely(ret))
		goto out;

	qe->cqe.done = pmdfc_rdma_recv_remotemr_done;

	ret = pmdfc_rdma_post_recv(&(ctrl->queues[0]), qe, sizeof(struct pmdfc_rdma_memregion));

	if (unlikely(ret))
		goto out_free_qe;

	/* this delay doesn't really matter, only happens once */
	pmdfc_rdma_wait_completion(ctrl->queues[0].cq, qe);

out_free_qe:
	kmem_cache_free(req_cache, qe);
out:
	return ret;
}

static int pmdfc_rdma_send_localmr(struct pmdfc_rdma_ctrl *ctrl)
{
	struct rdma_req *qe;
	int ret;
	struct ib_device *dev;

	//  pr_info("[ INFO ] start: %s\n", __FUNCTION__);
	dev = ctrl->rdev->dev;

	ret = get_req_for_buf(&qe, dev, &(ctrl->clientmr), sizeof(ctrl->clientmr),
			DMA_TO_DEVICE);
	if (unlikely(ret))
		goto out;

	qe->cqe.done = pmdfc_rdma_send_localmr_done;

	ret = pmdfc_rdma_post_send(&(ctrl->queues[0]), qe, sizeof(struct pmdfc_rdma_memregion));

	if (unlikely(ret))
		goto out_free_qe;

	/* this delay doesn't really matter, only happens once */
	pmdfc_rdma_wait_completion(ctrl->queues[0].cq, qe);

out_free_qe:
	kmem_cache_free(req_cache, qe);
out:
	return ret;
}


/* idx is absolute id (i.e. > than number of cpus) */
inline enum qp_type get_queue_type(unsigned int idx)
{
#ifdef SINGLE_TEST
	// just for test
	if (idx == 0)
		return QP_READ_SYNC;
	else if (idx == 1)
		return QP_WRITE_SYNC;
#else
	if (idx < numcpus / 2)
		return QP_READ_SYNC; // read page
	else if (idx >= numcpus / 2)
		return QP_WRITE_SYNC; // write page
#endif
	BUG();
	return QP_READ_SYNC;
}

static int __init rdma_connection_init_module(void)
{
	int ret;

	//  pr_info("[ INFO ] start: %s\n", __FUNCTION__);
	pr_info("[ INFO ] * RDMA BACKEND *");

	numcpus = num_online_cpus();
	//numqueues = numcpus * 3; // prefetch, read, write

	req_cache = kmem_cache_create("pmdfc_req_cache", sizeof(struct rdma_req), 0,
			SLAB_TEMPORARY | SLAB_HWCACHE_ALIGN, NULL);

	if (!req_cache) {
		pr_err("[ FAIL ] no memory for cache allocation\n");
		return -ENOMEM;
	}

	ib_register_client(&pmdfc_rdma_ib_client);
	ret = pmdfc_rdma_create_ctrl(&gctrl);
	if (ret) {
		pr_err("[ FAIL ] could not create ctrl\n");
		ib_unregister_client(&pmdfc_rdma_ib_client);
		return -ENODEV;
	}

	ret = pmdfc_rdma_recv_remotemr(gctrl);
	if (ret) {
		pr_err("[ FAIL ] could not setup remote memory region\n");
		ib_unregister_client(&pmdfc_rdma_ib_client);
		return -ENODEV;
	}

#if 0
	ret = pmdfc_rdma_send_localmr(gctrl);
	if (ret) {
		pr_err("[ FAIL ] could not send local memory region\n");
		ib_unregister_client(&pmdfc_rdma_ib_client);
		return -ENODEV;
	}
#endif

	pr_info("[ PASS ] ctrl is ready for reqs\n");
	return 0;
}

module_init(rdma_connection_init_module);
module_exit(rdma_connection_cleanup_module);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("rdma connection using rdma_cm");
MODULE_AUTHOR("Daegyu");
