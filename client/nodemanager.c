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

void init_pmnm_cluster(void){
	struct pmnm_cluster *cluster = NULL;
	struct pmnm_node *server_node = NULL;
	struct pmnm_node *client_node = NULL;

	pr_info("nodemanager: init_pmnm_cluster\n");

	cluster = kzalloc(sizeof(struct pmnm_cluster), GFP_KERNEL);

	server_node = init_pmnm_node("pm_server", DEST_ADDR, PORT, 0);
	client_node = init_pmnm_node("pm_client", CLIENT_ADDR, CLIENT_PORT, 1);

	cluster->cl_nodes[0] = server_node;
	cluster->cl_nodes[1] = client_node;

	rwlock_init(&cluster->cl_nodes_lock);
	pmnm_single_cluster = cluster;

	return;
}

void exit_pmnm_cluster(void){
	pmnm_cluster_release();
}
