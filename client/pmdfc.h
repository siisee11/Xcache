#ifndef _PMDFC_H_
#define _PMDFC_H_

#define PMDFC_MAX_STORAGE 4

enum pmdfc_storage_flags {
	PS_locked,
	PS_full,
	PS_empty,
	PS_put,
	PS_send,
};

struct pmdfc_storage {
	unsigned long flags;
	void **page_storage;
	long *key_storage;
	unsigned int *index_storage;
	struct mutex 	lock;
	unsigned int 	bitmap_size;
	unsigned long 	*bitmap;
};

struct pmdfc_storage_cluster{
	struct pmdfc_storage *cl_storages[PMDFC_MAX_STORAGE];
	/* this bitmap is part of a hack for disk bitmap.. will go eventually. - zab */
//	unsigned long	cl_nodes_bitmap[BITS_TO_LONGS(PMDFC_MAX_STORAGE)];
};

#endif
