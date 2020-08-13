#ifndef _PMDFC_H_
#define _PMDFC_H_

struct pmdfc_storage {
	void **page_storage;
	struct mutex 	lock;
	unsigned int 	bitmap_size;
	unsigned long 	bitmap[0];
};
#endif
