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
#include <linux/if.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/if_arp.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <net/addrconf.h>

#include "rdpma.h"
#include "ib.h"
#include "ib_mr.h"

struct workqueue_struct *rdpma_ib_wq;
EXPORT_SYMBOL_GPL(rdpma_ib_wq);

struct ib_device *ibdev;

static unsigned int rdpma_ib_mr_1m_pool_size = RDPMA_MR_1M_POOL_SIZE;
static unsigned int rdpma_ib_mr_8k_pool_size = RDPMA_MR_8K_POOL_SIZE;
unsigned int rdpma_ib_retry_count = RDPMA_IB_DEFAULT_RETRY_COUNT;
static atomic_t rdpma_ib_unloading;

struct ib_client rdpma_ib_client = {
	.name   = "rdpma_ib_client",
	.add    = rdpma_ib_add_one,
	.remove = rdpma_ib_remove_one
};

module_param(rdpma_ib_mr_1m_pool_size, int, 0444);
MODULE_PARM_DESC(rdpma_ib_mr_1m_pool_size, " Max number of 1M mr per HCA");
module_param(rdpma_ib_mr_8k_pool_size, int, 0444);
MODULE_PARM_DESC(rdpma_ib_mr_8k_pool_size, " Max number of 8K mr per HCA");
module_param(rdpma_ib_retry_count, int, 0444);
MODULE_PARM_DESC(rdpma_ib_retry_count, " Number of hw retries before reporting an error");

/*
 * we have a clumsy combination of RCU and a rwsem protecting this list
 * because it is used both in the get_mr fast path and while blocking in
 * the FMR flushing path.
 */
DECLARE_RWSEM(rdpma_ib_devices_lock);
struct list_head rdpma_ib_devices;

/*
 * rdpma_ib_destroy_mr_pool() blocks on a few things and mrs drop references
 * from interrupt context so we push freing off into a work struct in krdpmad.
 */
static void rdpma_ib_dev_free(struct work_struct *work)
{
	struct rdpma_ib_ipaddr *i_ipaddr, *i_next;
	struct rdpma_ib_device *rdpma_ibdev = container_of(work,
					struct rdpma_ib_device, free_work);

	if (rdpma_ibdev->pd)
		ib_dealloc_pd(rdpma_ibdev->pd);

	list_for_each_entry_safe(i_ipaddr, i_next, &rdpma_ibdev->ipaddr_list, list) {
		list_del(&i_ipaddr->list);
		kfree(i_ipaddr);
	}

	kfree(rdpma_ibdev->vector_load);

	kfree(rdpma_ibdev);
}

void rdpma_ib_dev_put(struct rdpma_ib_device *rdpma_ibdev)
{
	BUG_ON(refcount_read(&rdpma_ibdev->refcount) == 0);
	if (refcount_dec_and_test(&rdpma_ibdev->refcount))
		queue_work(rdpma_ib_wq, &rdpma_ibdev->free_work);
}

static void rdpma_ib_add_one(struct ib_device *device)
{
	struct rdpma_ib_device *rdpma_ibdev;
	bool has_fr, has_fmr;

	/* TODO: What is those strange names?? */
	pr_info("rdpma_ib_add_one called for device %s\n", device->name);

	/* Only handle IB (no iWARP) devices */
	if (device->node_type != RDMA_NODE_IB_CA) {
		pr_info("Only handle IB (no iWARP) devices");
        return;
	}

	rdpma_ibdev = kzalloc_node(sizeof(struct rdpma_ib_device), 
                            GFP_KERNEL, ibdev_to_node(device));
	
    if (!rdpma_ibdev)
		return;

	spin_lock_init(&rdpma_ibdev->spinlock);
	refcount_set(&rdpma_ibdev->refcount, 1);
	//INIT_WORK(&rdpma_ibdev->free_work, rdpma_ib_dev_free);

	rdpma_ibdev->max_wrs = device->attrs.max_qp_wr;
	rdpma_ibdev->max_sge = min(device->attrs.max_send_sge, RDPMA_IB_MAX_SGE);

	has_fr = (device->attrs.device_cap_flags &
		  IB_DEVICE_MEM_MGT_EXTENSIONS);
	has_fmr = (device->ops.alloc_fmr && device->ops.dealloc_fmr &&
		   device->ops.map_phys_fmr && device->ops.unmap_fmr);
	rdpma_ibdev->use_fastreg = (has_fr && !has_fmr);

	rdpma_ibdev->fmr_max_remaps = device->attrs.max_map_per_fmr?: 32;
	rdpma_ibdev->max_1m_mrs = device->attrs.max_mr ?
		min_t(unsigned int, (device->attrs.max_mr / 2),
		      rdpma_ib_mr_1m_pool_size) : rdpma_ib_mr_1m_pool_size;

	rdpma_ibdev->max_8k_mrs = device->attrs.max_mr ?
		min_t(unsigned int, ((device->attrs.max_mr / 2) * RDPMA_MR_8K_SCALE),
		      rdpma_ib_mr_8k_pool_size) : rdpma_ib_mr_8k_pool_size;

	rdpma_ibdev->max_initiator_depth = device->attrs.max_qp_init_rd_atom;
	rdpma_ibdev->max_responder_resources = device->attrs.max_qp_rd_atom;

	rdpma_ibdev->vector_load = kcalloc(device->num_comp_vectors,
					 sizeof(int),
					 GFP_KERNEL);
	if (!rdpma_ibdev->vector_load) {
		pr_err("rdpma/IB: %s failed to allocate vector memory\n",
			__func__);
		goto put_dev;
	}

	rdpma_ibdev->dev = device;
	rdpma_ibdev->pd = ib_alloc_pd(device, 0);
	if (IS_ERR(rdpma_ibdev->pd)) {
		rdpma_ibdev->pd = NULL;
		goto put_dev;
	}

#if 0
	rdpma_ibdev->mr_1m_pool =
		rdpma_ib_create_mr_pool(rdpma_ibdev, rdpma_IB_MR_1M_POOL);
	if (IS_ERR(rdpma_ibdev->mr_1m_pool)) {
		rdpma_ibdev->mr_1m_pool = NULL;
		goto put_dev;
	}

	rdpma_ibdev->mr_8k_pool =
		rdpma_ib_create_mr_pool(rdpma_ibdev, rdpma_IB_MR_8K_POOL);
	if (IS_ERR(rdpma_ibdev->mr_8k_pool)) {
		rdpma_ibdev->mr_8k_pool = NULL;
		goto put_dev;
	}
#endif

	rdpmadebug("rdpma/IB: max_mr = %d, max_wrs = %d, max_sge = %d, fmr_max_remaps = %d, max_1m_mrs = %d, max_8k_mrs = %d\n",
		 device->attrs.max_fmr, rdpma_ibdev->max_wrs, rdpma_ibdev->max_sge,
		 rdpma_ibdev->fmr_max_remaps, rdpma_ibdev->max_1m_mrs,
		 rdpma_ibdev->max_8k_mrs);

	rdpmadebug("rdpma/IB: %s: %s supported and preferred\n",
		device->name,
		rdpma_ibdev->use_fastreg ? "FRMR" : "FMR");

	INIT_LIST_HEAD(&rdpma_ibdev->ipaddr_list);
	INIT_LIST_HEAD(&rdpma_ibdev->conn_list);

	down_write(&rdpma_ib_devices_lock);
	list_add_tail_rcu(&rdpma_ibdev->list, &rdpma_ib_devices);
	up_write(&rdpma_ib_devices_lock);
	refcount_inc(&rdpma_ibdev->refcount);

	ib_set_client_data(device, &rdpma_ib_client, rdpma_ibdev);
	refcount_inc(&rdpma_ibdev->refcount);

//	rdpma_ib_nodev_connect();

	if (strcmp(device->name, "mlx5_0") == 0) {
		ibdev = device;
	}

put_dev:
	rdpma_ib_dev_put(rdpma_ibdev);
}

/*
 * New connections use this to find the device to associate with the
 * connection.  It's not in the fast path so we're not concerned about the
 * performance of the IB call.  (As of this writing, it uses an interrupt
 * blocking spinlock to serialize walking a per-device list of all registered
 * clients.)
 *
 * RCU is used to handle incoming connections racing with device teardown.
 * Rather than use a lock to serialize removal from the client_data and
 * getting a new reference, we use an RCU grace period.  The destruction
 * path removes the device from client_data and then waits for all RCU
 * readers to finish.
 *
 * A new connection can get NULL from this if its arriving on a
 * device that is in the process of being removed.
 */
struct rdpma_ib_device *rdpma_ib_get_client_data(struct ib_device *device)
{
	struct rdpma_ib_device *rdpma_ibdev;

	rcu_read_lock();
	rdpma_ibdev = ib_get_client_data(device, &rdpma_ib_client);
	if (rdpma_ibdev)
		refcount_inc(&rdpma_ibdev->refcount);
	rcu_read_unlock();
	return rdpma_ibdev;
}

/*
 * The IB stack is letting us know that a device is going away.  This can
 * happen if the underlying HCA driver is removed or if PCI hotplug is removing
 * the pci function, for example.
 *
 * This can be called at any time and can be racing with any other rdpma path.
 */
static void rdpma_ib_remove_one(struct ib_device *device, void *client_data)
{
	struct rdpma_ib_device *rdpma_ibdev = client_data;

	if (!rdpma_ibdev)
		return;

//	rdpma_ib_dev_shutdown(rdpma_ibdev);

	/* stop connection attempts from getting a reference to this device. */
	ib_set_client_data(device, &rdpma_ib_client, NULL);

	down_write(&rdpma_ib_devices_lock);
	list_del_rcu(&rdpma_ibdev->list);
	up_write(&rdpma_ib_devices_lock);

	/*
	 * This synchronize rcu is waiting for readers of both the ib
	 * client data and the devices list to finish before we drop
	 * both of those references.
	 */
	synchronize_rcu();
	rdpma_ib_dev_put(rdpma_ibdev);
	rdpma_ib_dev_put(rdpma_ibdev);
}

/*
static struct class client_class = {
	.name = "rdpma_client_class"
};
*/



static void rdpma_ib_unregister_client(void)
{
	ib_unregister_client(&rdpma_ib_client);
	/* wait for rdpma_ib_dev_free() to complete */
	flush_workqueue(rdpma_ib_wq);
}

void rdpma_ib_exit(void)
{
//	rdpma_ib_set_unloading();
	synchronize_rcu();
	rdpma_ib_unregister_client();
#if 0
	rdpma_ib_destroy_nodev_conns();
	rdpma_ib_sysctl_exit();
	rdpma_ib_recv_exit();
	rdpma_trans_unregister(&rdpma_ib_transport);
	rdpma_ib_mr_exit();
#endif
}

int rdpma_ib_init(void)
{
	int ret;
	struct rdpma_ib_device *rdpma_ibdev;
	
	pr_info("RDPMA IB module init....\n");

	//rdpma_ib_wq = create_singlethread_workqueue("krdsd");
	//if (!rdpma_ib_wq)
	//	return -ENOMEM;

	INIT_LIST_HEAD(&rdpma_ib_devices);

#if 0
	ret = rdpma_ib_mr_init();
	if (ret)
		goto out;
#endif

	ret = ib_register_client(&rdpma_ib_client);
	if(ret){
		pr_err("ib_register failed\n");
		return -1;
	}

#if 0
	ret = rdpma_ib_sysctl_init();
	if (ret)
		goto out_ibreg;

	ret = rdpma_ib_recv_init();
	if (ret)
		goto out_sysctl;
#endif

	goto out;

#if 0
out_sysctl:
	rdpma_ib_sysctl_exit();
out_ibreg:
	rdpma_ib_unregister_client();
out_mr_exit:
	rdpma_ib_mr_exit();
#endif
out:
	return ret;
}

MODULE_LICENSE("GPL");
