#ifndef _HOTRING_H
#define _HOTRING_H

#include <stdbool.h> 
#include <stdio.h>
#include <stdint.h>
#include "list.h"
#include "rcupdate.h"

#define NBITS	(32)
#define INCOME_THRESHOLD	(5)

struct uint48 {
	    uint64_t x:48;
} __attribute__((packed));

int kmalloc_verbose;
int test_verbose;


#define printv(verbosity_level, fmt, ...) \
    if(test_verbose >= verbosity_level) \
        printf(fmt, ##__VA_ARGS__)


#define BITS_PER_LONG 	(64)
#define ARRAY_SIZE(a) sizeof(a)/sizeof(a[0])

struct hash_iter {
	unsigned long	index;
	unsigned long	next_index;
	struct hash_node *node;
};

struct head {
	union {
		struct {
			uint64_t __rcu addr : 48;
			unsigned short counter : 15;  	/* counter */
			unsigned short active : 1; 		/* active bit */
		};
		struct hash_node *node;
	};
} __attribute__((packed));

struct hash {
	unsigned long n;		/* n-bit hash value */
	unsigned long k;		/* table part */
	unsigned long size;
	void __rcu **slots;
};

struct hash_node {
	union {
		struct list_head list;
		struct rcu_head rcu_head;
	};
	unsigned long tag;
	unsigned long key;
	void *value;
};

struct item {
   int index;
   int data;
};

/* hash function */
static inline unsigned long hashCode(struct hash *h, unsigned long key, unsigned long *tag) {
	unsigned long hash_value = key % ((unsigned long) 1 << h->n);
	*tag = hash_value % (1 << (h->n - h->k));
	return key / (1 << (h->n - h->k));
}

struct hash *hotring_alloc(unsigned long, unsigned long );
int hotring_insert(struct hash *, unsigned long , void *);
int hotring_get(struct hash **, unsigned long ,
			  struct hash_node **, struct hash_node **);
bool hotring_delete(struct hash *, unsigned long );
void display(struct hash *);
struct hash *hotring_rehash(struct hash *);

/**
 * hash_iter_init - initialize radix tree iterator
 *
 * @iter:	pointer to iterator state
 * @start:	iteration starting index
 * Returns:	NULL
 */
static __always_inline void **
hash_iter_init(struct hash_iter *iter, unsigned long start)
{
	iter->index = 0;
	iter->next_index = start;
	return NULL;
}

static inline unsigned long
__hash_iter_add(struct hash_iter *iter, unsigned long slots)
{
	return iter->index + slots;
}

static __always_inline void **hash_next_slot(void **slot,
				struct hash_iter *iter)
{
	long count = 20;

	while (--count > 0) {
		slot++;
		iter->index = __hash_iter_add(iter, 1);

		if (*slot)
			goto found;
	}
	return NULL;

 found:
	return slot;
}

/**
 * hash_for_each_slot - iterate over non-empty slots
 *
 * @slot:	the void** variable for pointer to slot
 * @root:	the struct hash pointer
 * @iter:	the struct hash_iter pointer
 * @start:	iteration starting index
 *
 * @slot points to radix tree slot, @iter->index contains its index.
 */
#define hash_for_each_slot(slot, root, iter, start)		\
	for (slot = hash_iter_init(iter, start) ;			\
	     slot || (slot = hash_next_chunk(root, iter, 0)) ;	\
	     slot = hash_next_slot(slot, iter, 0))


#endif
