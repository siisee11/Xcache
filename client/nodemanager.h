/* SPDX-License-Identifier: GPL-2.0-or-later */
/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * nodemanager.h
 *
 * Function prototypes
 *
 * Copyright (C) 2004 Oracle.  All rights reserved.
 */

#ifndef PMCLUSTER_NODEMANAGER_H
#define PMCLUSTER_NODEMANAGER_H

#include "pmdfc_nodemanager.h"

/* This totally doesn't belong here. */
#include <linux/configfs.h>
#include <linux/rbtree.h>

enum pmnm_fence_method {
	PMNM_FENCE_RESET	= 0,
	PMNM_FENCE_PANIC,
	PMNM_FENCE_METHODS,	/* Number of fence methods */
};

struct pmnm_node {
	spinlock_t		nd_lock;
	struct config_item	nd_item;
	char			nd_name[PMNM_MAX_NAME_LEN+1]; /* replace? */
	__u8			nd_num;
	/* only one address per node, as attributes, for now. */
	__be32			nd_ipv4_address;
	__be16			nd_ipv4_port;
//	struct rb_node		nd_ip_node;
	/* there can be only one local node for now */
//	int			nd_local;

//	unsigned long		nd_set_attributes;
};

struct pmnm_cluster {
	struct config_group	cl_group;
	unsigned		cl_has_local:1;
	u8			cl_local_node;
	rwlock_t		cl_nodes_lock;
	struct pmnm_node  	*cl_nodes[PMNM_MAX_NODES];
	struct rb_root		cl_node_ip_tree;
	unsigned int		cl_idle_timeout_ms;
	unsigned int		cl_keepalive_delay_ms;
	unsigned int		cl_reconnect_delay_ms;
	enum pmnm_fence_method	cl_fence_method;

	/* this bitmap is part of a hack for disk bitmap.. will go eventually. - zab */
	unsigned long	cl_nodes_bitmap[BITS_TO_LONGS(PMNM_MAX_NODES)];
};

extern struct pmnm_cluster *pmnm_single_cluster;
struct pmnm_node *pmnm_get_node_by_num(u8 node_num);


void pmnm_node_get(struct pmnm_node *node);
void pmnm_node_put(struct pmnm_node *node);

#if 0

u8 pmnm_this_node(void);

int pmnm_configured_node_map(unsigned long *map, unsigned bytes);
struct pmnm_node *pmnm_get_node_by_ip(__be32 addr);

int pmnm_depend_item(struct config_item *item);
void pmnm_undepend_item(struct config_item *item);
int pmnm_depend_this_node(void);
void pmnm_undepend_this_node(void);
#endif

void init_pmnm_cluster(void);
void exit_pmnm_cluster(void);

#endif /* PMCLUSTER_NODEMANAGER_H */
