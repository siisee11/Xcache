#ifndef _PMDFC_H_
#define _PMDFC_H_

#include <linux/hashtable.h>
#include <linux/atomic.h>

#define PMDFC_MAX_STORAGE 8 			/* Number of storages */
#define PMDFC_STORAGE_SIZE  4096 		/* Number of page per each storage */

enum pmdfc_storage_flags {
	PS_locked,
	PS_full, 	/* Need to be process */
	PS_empty, 	/* Initial State */
	PS_put, 	/* Only one PS can have this flag */
	PS_send,
};

struct pmdfc_storage {
	unsigned long flags;
	void **page_storage;
	long *key_storage;
	unsigned int *index_storage;
	long *roffset_storage;
	struct mutex 	lock;
	unsigned int 	bitmap_size;
	unsigned long 	*bitmap;
};

struct pmdfc_storage_cluster{
	struct pmdfc_storage *cl_storages[PMDFC_MAX_STORAGE];
	/* this bitmap is part of a hack for disk bitmap.. will go eventually. - zab */
//	unsigned long	cl_nodes_bitmap[BITS_TO_LONGS(PMDFC_MAX_STORAGE)];
};

extern struct pmdfc_storage_cluster *ps_cluster;

struct ht_data {
    uint64_t longkey;
    uint64_t roffset;
    struct hlist_node h_node;
};

#endif
