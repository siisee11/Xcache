#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>


struct ib_client comm_client = {
    .name   = "rdma_client",
    //.add    = rdpma_ib_add_one,
    //.remove = rdpma_ib_remove_one
};

static int rdma_create_conn(struct connection **connection, struct donor_info *donor)
{
	int ret;
	bool wait_for_cq;
	struct rdma_conn_param param;
	struct connection *conn;
	cycles_t lat_avg;

	dprintk(MODULE_NAME ": %s called\n", __func__);

	conn = kzalloc(sizeof(struct connection), GFP_KERNEL);
	if (!conn) {
		ret = -ENOMEM;
		goto _create_conn_error;
	}

	conn->ip = donor->addr;
	memset(&param, 0, sizeof(struct rdma_conn_param));
	param.responder_resources = 1;
	param.initiator_depth = 1;
	param.retry_count = RDMA_CONN_RETRY_COUNT;

	conn->local.sin_family = AF_INET;
	conn->local.sin_addr.s_addr = htonl(INADDR_ANY);
	conn->local.sin_port = htons(0);

	conn->remote.sin_family = AF_INET;
	conn->remote.sin_addr.s_addr = donor->addr;
	conn->remote.sin_port = htons(donor->port==0?DONOR_PORT:donor->port);

	init_waitqueue_head(&conn->context.msg_wq);
	init_completion(&(conn->completion));
	conn->context.msg_state = RDMA_INIT;

	conn->cm_id = rdma_create_id(&init_net, cm_event_handler, conn,
			RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(conn->cm_id))
		return -1;

	ret = rdma_resolve_addr(conn->cm_id,
			(struct sockaddr *)&(conn->local),
			(struct sockaddr *)&(conn->remote),
			RDMA_TIMEOUT_MS);
	dprintk(MODULE_NAME ": %s: rdma_resolve_addr: %d\n",
			__func__, ret);

	*connection = conn;

	wait_event_interruptible(conn->context.msg_wq,
			conn->context.msg_state == RDMA_READY);
	wait_for_cq = true;
	while (wait_for_cq) {
		if (conn->context.qp_attr.send_cq &&
				conn->context.qp_attr.recv_cq)
			wait_for_cq = false;
		cond_resched();
	}

	atomic_set(&conn->refc, 0);
	lat_avg = test_read_latency(conn);
	printk("donor %x connected (lat_avg: %lld)", donor->addr, lat_avg);
_create_conn_error:
	return atomic_read(&conn->is_connected) == 0; 

}

int create_mr(bvma_t *bvma, struct donor_info *donor, int *mr_id)
{
	int ret, mrid;
	struct connection *conn;
	memreg_t *mr;

	ret = rdma_create_conn(&conn, donor);
	if (!ret) {
		atomic_inc(&conn->refc);
	} else {
		pr_err("[ FAILED ] failed to connect to the donor\n");
		ret = -EINVAL;
		goto err;
	}
	//conn->bvma = bvma;
	mr = kzalloc(sizeof(memreg_t), GFP_KERNEL);
	if (!mr) {
		ret = -ENOMEM;
		goto err_alloc_mr;
	}

	mrid = alloc_mrid(bvma, mr);
	if (mrid < 0) {
		ret = -EBUSY;
		goto err_alloc_mrid;
	}

	// initialize a memreg
	mr->id = (u8)mrid;
	mr->conn = conn;
	mr->ops = dma_ops;
	conn->memreg = mr;
	if (donor->dev_type == DONOR_DEV_RDMA) {
		mr->addr = donor->addr;
		mr->port = donor->port;
		mr->type = MR_RDMA;
		mr->dma_order = 0;
	} else {
		mr->type = MR_NVME;
		mr->dma_order = CONFIG_SUBBLOCK_ORDER(bvma);
	}

	/* It assumes that size is in megabyte.
	 * Therefore, it is converted to pfn.  */
	mr->size = MB_TO_PAGE(donor->size);
	atomic_set(&mr->allocated, 0);
	atomic64_set(&mr->wr_len, 0);

	snprintf(str_buffer, 16, "memreg%d:%d", bvma->id, mr->id);
	mr->wr_cache = kmem_cache_create(str_buffer, sizeof(work_req_t),
			0, 0, dma_ops->wr_ctor);
	if (!mr->wr_cache) {
		ret = -ENOMEM;
		goto err_alloc_wr_cache;
	}
	mr->bvma = bvma;

	init_waitqueue_head(&mr->wbd_wq);
	init_completion(&mr->wbd_comp);
	mr->wbd_sched = false;
	snprintf(str_buffer, 16, "wbd%d:%d", bvma->id, mr->id);
	mr->wbd = kthread_run(wbd_thread_func, mr, str_buffer);
	// end of initialization
	if (mr_id)
		*mr_id = mr->id;

	return 0;
err_alloc_wr_cache:
	free_mrid(bvma, mrid, false);
err_alloc_mrid:
	kfree(mr);
err_alloc_mr:
	(dma_ops->destroy_conn)(conn);
err:
	return ret;
} 


static void rdpma_ib_unregister_client(void)
{
    ib_unregister_client(&rdpma_ib_client);
}
static int __init init_my_rdma(void)
{
    int i;
    int ret = 0;
    struct donor_info _donor, *donor;

    ret = ib_register_client(&comm_client);
    
    if (ret) {
        pr_err("[ FAILED ] ib_register_client failed\n")
    }
    
    ret = create_mr(bvma, donor, NULL);
    if (donor != &_donor) {
        kree(donor);
    }

    pr_info("[  OK  ] ib_register_client successfully registered\n");

    return 0;
}

static void __exit exit_my_rdma(void)
{
    ib_unregister_client(&comm_client);
    return;
}

MODULE_LICENSE("GPL");
module_init(init_my_rdma)
module_exit(exit_my_rdma)
