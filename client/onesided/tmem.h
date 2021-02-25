/*
 * tmem.h
 *
 * Transcendent Memory
 *
 * Copyright (c) 2019, Jaeyoun Nam, SKKU.
 */

#include <linux/types.h>
#include <linux/highmem.h>
#include <linux/hash.h>
#include <linux/atomic.h>


#define TMEM_HASH_BUCKET_BITS	8
#define TMEM_HASH_BUCKETS	(1<<TMEM_HASH_BUCKET_BITS)

struct tmem_hashbucket {
	struct rb_root obj_rb_root;
	spinlock_t lock;
};


/*
 * An object id ("oid") is from (cleancache_filekey)
 */
struct tmem_oid {
	uint64_t oid[3];
};

/*
 * oid operations
 */

static inline void tmem_oid_set_invalid(struct tmem_oid *oidp)
{
	oidp->oid[0] = oidp->oid[1] = oidp->oid[2] = -1UL;
}

static inline bool tmem_oid_valid(struct tmem_oid *oidp)
{
	return oidp->oid[0] != -1UL || oidp->oid[1] != -1UL ||
		oidp->oid[2] != -1UL;
}

static inline void tmem_oid_print(struct tmem_oid *oidp)
{
	printk(KERN_INFO ".oid[0]=%llx .oid[1]=%llx .oid[2]=%llx\n", 
			(long long)oidp->oid[0], (long long)oidp->oid[1], (long long)oidp->oid[2]);
}

/*
 * If same return 0
 */
static inline int tmem_oid_compare(struct tmem_oid *left,
					struct tmem_oid *right)
{
	int ret;

	if (left->oid[2] == right->oid[2]) {
		if (left->oid[1] == right->oid[1]) {
			if (left->oid[0] == right->oid[0])
				ret = 0;
			else if (left->oid[0] < right->oid[0])
				ret = -1;
			else
				return 1;
		} else if (left->oid[1] < right->oid[1])
			ret = -1;
		else
			ret = 1;
	} else if (left->oid[2] < right->oid[2])
		ret = -1;
	else
		ret = 1;
	return ret;
}


