/*
 * Copyright (c) 2016 Oracle.  All rights reserved.
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
 */
#ifndef _RDPMA_IB_MR_H
#define _RDPMA_IB_MR_H

#include <linux/kernel.h>

#include "rdpma.h"
#include "ib.h"

#define RDPMA_MR_1M_POOL_SIZE		(8192 / 2)
#define RDPMA_MR_1M_MSG_SIZE		256
#define RDPMA_MR_8K_MSG_SIZE		2
#define RDPMA_MR_8K_SCALE			(256 / (RDPMA_MR_8K_MSG_SIZE + 1))
#define RDPMA_MR_8K_POOL_SIZE		(RDPMA_MR_8K_SCALE * (8192 / 2))

struct rdpma_ib_fmr {
	struct ib_fmr		*fmr;
};

enum rdpma_ib_fr_state {
	FRMR_IS_FREE,	/* mr invalidated & ready for use */
	FRMR_IS_INUSE,	/* mr is in use or used & can be invalidated */
	FRMR_IS_STALE,	/* Stale MR and needs to be dropped  */
};

struct rdpma_ib_frmr {
	struct ib_mr		*mr;
	enum rdpma_ib_fr_state	fr_state;
	bool			fr_inv;
	wait_queue_head_t	fr_inv_done;
	bool			fr_reg;
	wait_queue_head_t	fr_reg_done;
	struct ib_send_wr	fr_wr;
	unsigned int		dma_npages;
	unsigned int		sg_byte_len;
};

/* This is stored as mr->r_trans_private. */
struct rdpma_ib_mr {
	struct rdpma_ib_device		*device;
	struct rdpma_ib_mr_pool		*pool;
	struct rdpma_ib_connection	*ic;

	struct llist_node		llnode;

	/* unmap_list is for freeing */
	struct list_head		unmap_list;
	unsigned int			remap_count;

	struct scatterlist		*sg;
	unsigned int			sg_len;
	int				sg_dma_len;

	union {
		struct rdpma_ib_fmr	fmr;
		struct rdpma_ib_frmr	frmr;
	} u;
};

/* Our own little MR pool */
struct rdpma_ib_mr_pool {
	unsigned int            pool_type;
	struct mutex		flush_lock;	/* serialize fmr invalidate */
	struct delayed_work	flush_worker;	/* flush worker */

	atomic_t		item_count;	/* total # of MRs */
	atomic_t		dirty_count;	/* # dirty of MRs */

	struct llist_head	drop_list;	/* MRs not reached max_maps */
	struct llist_head	free_list;	/* unused MRs */
	struct llist_head	clean_list;	/* unused & unmapped MRs */
	wait_queue_head_t	flush_wait;
	spinlock_t		clean_lock;	/* "clean_list" concurrency */

	atomic_t		free_pinned;	/* memory pinned by free MRs */
	unsigned long		max_items;
	unsigned long		max_items_soft;
	unsigned long		max_free_pinned;
	struct ib_fmr_attr	fmr_attr;
	bool			use_fastreg;
};
#endif
