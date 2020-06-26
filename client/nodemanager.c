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

	pr_info("init_pmnm_node: name= %s, ip=%s, port=%d\n", name, ip, port);

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

	pr_info("pmnm: Registering node %s\n", name);

	return node;
}

static void pmnm_cluster_release(void)
{
	kfree(pmnm_single_cluster);
}

/* refer to o2nm_cluster_group_make_group */
void init_pmnm_cluster(void){
	struct pmnm_cluster *cluster = NULL;
	struct pmnm_node *server_node = NULL;
	struct pmnm_node *client_node = NULL;

	pr_info("nodemanager: init_pmnm_cluster\n");

	cluster = kzalloc(sizeof(struct pmnm_cluster), GFP_KERNEL);
	if (cluster == NULL)
		goto out;

	/* 0 is server, 1 is client node */
	server_node = init_pmnm_node("pm_server", DEST_ADDR, PORT, 0);
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
