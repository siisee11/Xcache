// SPDX-License-Identifier: GPL-2.0-or-later
/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * Copyright (C) 2020 JY N.  All rights reserved.
 */

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/configfs.h>

#include "tcp.h"
#include "nodemanager.h"
#include "masklog.h"
#include "sys.h"

static unsigned int inet_addr(const char *str)
{
	int a,b,c,d;
	char arr[4];
	sscanf(str,"%d.%d.%d.%d",&a,&b,&c,&d);
	arr[0] = a; arr[1] = b; arr[2] = c; arr[3] = d;
	return *(unsigned int*)arr;
}

struct pmnm_cluster *pmnm_single_cluster = NULL;

struct pmnm_node *pmnm_get_node_by_num(u8 node_num)
{
	struct pmnm_node *node = NULL;

	if (node_num >= PMNM_MAX_NODES || pmnm_single_cluster == NULL)
		goto out;

	read_lock(&pmnm_single_cluster->cl_nodes_lock);
	node = pmnm_single_cluster->cl_nodes[node_num];
	read_unlock(&pmnm_single_cluster->cl_nodes_lock);
out:
	return node;
}
EXPORT_SYMBOL_GPL(pmnm_get_node_by_num);

int pmnm_configured_node_map(unsigned long *map, unsigned bytes)
{
	struct pmnm_cluster *cluster = pmnm_single_cluster;

	BUG_ON(bytes < (sizeof(cluster->cl_nodes_bitmap)));

	if (cluster == NULL)
		return -EINVAL;

	read_lock(&cluster->cl_nodes_lock);
	memcpy(map, cluster->cl_nodes_bitmap, sizeof(cluster->cl_nodes_bitmap));
	read_unlock(&cluster->cl_nodes_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(pmnm_configured_node_map);

void pmnm_node_put(struct pmnm_node *node)
{
	config_item_put(&node->nd_item);
}
EXPORT_SYMBOL_GPL(pmnm_node_put);

void pmnm_node_get(struct pmnm_node *node)
{
	config_item_get(&node->nd_item);
}
EXPORT_SYMBOL_GPL(pmnm_node_get);


struct pmnm_node *init_pmnm_node(const char *name, 
		const char *ip, unsigned int port, int num)
{
	struct pmnm_node *node = NULL;

//	pr_info("init_pmnm_node: name= %s, ip=%s, port=%d\n", name, ip, port);

	if (strlen(name) > PMNM_MAX_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	node = kzalloc(sizeof(struct pmnm_node), GFP_KERNEL);
	if (node == NULL)
		return ERR_PTR(-ENOMEM);

	strcpy(node->nd_name, name); 
	
	node->nd_num = num;
	node->nd_ipv4_address = inet_addr(ip);
	node->nd_ipv4_port = htons(port);

	/* XXX: This printk hang why?? */
#if 0
	printk(KERN_NOTICE "init_pmnm_node: node %s (num %u) at %pI4:%u\n",
			node->nd_name, node->nd_num, node->nd_ipv4_address, 
			ntohs(node->nd_ipv4_port)); 
#endif

//	pr_info("pmnm: Registering node %s\n", name);

	return node;
}

static void pmnm_cluster_release(void)
{
	kfree(pmnm_single_cluster);
}

static int param_port = 0;
static char param_ip[64];
module_param_named(port, param_port, int, 0444);
module_param_string(ip, param_ip, sizeof(param_ip), 0444);

/* refer to o2nm_cluster_group_make_group */
void init_pmnm_cluster(void){
	struct pmnm_cluster *cluster = NULL;
	struct pmnm_node *server_node = NULL;
	struct pmnm_node *client_node = NULL;

	cluster = kzalloc(sizeof(struct pmnm_cluster), GFP_KERNEL);
	if (cluster == NULL)
		goto out;

	/* 0 is server, 1 is client node */
	server_node = init_pmnm_node("pm_server", param_ip, param_port, 0);
	client_node = init_pmnm_node("pm_client", CLIENT_ADDR, CLIENT_PORT, 1);

	cluster->cl_nodes[0] = server_node;
	cluster->cl_nodes[1] = client_node;

	rwlock_init(&cluster->cl_nodes_lock);

	cluster->cl_reconnect_delay_ms = PMNET_RECONNECT_DELAY_MS_DEFAULT;
	cluster->cl_idle_timeout_ms    = PMNET_IDLE_TIMEOUT_MS_DEFAULT;
	cluster->cl_keepalive_delay_ms = PMNET_KEEPALIVE_DELAY_MS_DEFAULT;
	cluster->cl_fence_method       = PMNM_FENCE_RESET;

	pmnm_single_cluster = cluster;

	return;

out:
	kfree(cluster);
	return;
}

void exit_pmnm_cluster(void){
	pmnm_cluster_release();
}


static void __exit exit_pmnm(void)
{
	/* XXX sync with hb callbacks and shut down hb? */
//	o2net_unregister_hb_callbacks();
//	configfs_unregister_subsystem(&o2nm_cluster_group.cs_subsys);
	pmcb_sys_shutdown();

	pmnet_exit();
	exit_pmnm_cluster();
//	o2hb_exit();
}

static int __init init_pmnm(void)
{
	int ret = -1;

//	pmhb_init();
	init_pmnm_cluster();

	ret = pmnet_init();
	if (ret)
		goto out_pmhb;

#if 0
	ret = pmnet_register_hb_callbacks();
	if (ret)
		goto out_pmnet;

	config_group_init(&pmnm_cluster_group.cs_subsys.su_group);
	mutex_init(&pmnm_cluster_group.cs_subsys.su_mutex);
	ret = configfs_register_subsystem(&pmnm_cluster_group.cs_subsys);
	if (ret) {
		printk(KERN_ERR "nodemanager: Registration returned %d\n", ret);
		goto out_callbacks;
	}
#endif

	ret = pmcb_sys_init();
	if (!ret)
		goto out;

#if 0
	configfs_unregister_subsystem(&pmnm_cluster_group.cs_subsys);
out_callbacks:
	pmnet_unregister_hb_callbacks();
#endif
out_pmnet:
	pmnet_exit();
out_pmhb:
//	pmhb_exit();
out:
	return ret;
}

MODULE_AUTHOR("JY");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PMDFC management");

module_init(init_pmnm)
module_exit(exit_pmnm)
