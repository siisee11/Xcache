#include <linux/cpu.h>
#include <linux/cleancache.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/uuid.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <linux/debugfs.h>
#include <asm/delay.h>

#include "tmem.h"
#include "tcp.h"
#include "bloom_filter.h"
#include "pmdfc.h"
#include "rdpma/rdpma.h"

#define PMDFC_PUT 1
#define PMDFC_GET 1
#define PMDFC_BLOOM_FILTER 1 
#define PMDFC_REMOTIFY 1
#define PMDFC_NETWORK 1

#define PMDFC_DEBUG 1

/* Allocation flags */
#define PMDFC_GFP_MASK  (GFP_ATOMIC | __GFP_NORETRY | __GFP_NOWARN)

#ifdef CONFIG_DEBUG_FS
struct dentry *pmdfc_dentry; 
#endif

static int rdma;

/* bloom filter */
/* TODO: is it thread safe? */
struct bloom_filter *bf;

/* work queue */
static void pmdfc_remotify_fn(struct work_struct *work);
static struct workqueue_struct *pmdfc_wq;
static DECLARE_WORK(pmdfc_remotify_work, pmdfc_remotify_fn);

/* list, lock and condition variables */
static LIST_HEAD(page_list_head);
DECLARE_WAIT_QUEUE_HEAD(pmdfc_worker_wq);
static int done = 0;

/* Global page storage */
struct pmdfc_storage_cluster *ps_cluster = NULL;

/*
 * Counters available via /sys/kernel/debug/pmdfc (if debugfs is
 * properly configured.  These are for information only so are not protected
 * against increment races.
 * HEY, is BF works?
 */
static u64 pmdfc_total_gets;
static u64 pmdfc_actual_gets;
static u64 pmdfc_miss_gets;
static u64 pmdfc_hit_gets;
static u64 pmdfc_drop_puts;

/* worker function for workqueue */
static void pmdfc_remotify_fn(struct work_struct *work)
{
	struct pmdfc_storage *storage = NULL;
	pgoff_t *indexes;
	long key;
	unsigned int index;
	int i;

	int status, ret = 0;
//	int cpu = -1;

//	cpu = smp_processor_id();
//	pr_info("pmdfc-remotify: workqueue worker running on CPU %u...\n", cpu);

	/* It must sleepable */
	BUG_ON(irqs_disabled());

	/* TODO: DEADLOCK AGAIN? */
	/*
	 * 1. Find full storage 
	 * If exist, send whole pages in storage with corresponding key and index.
	 * If not, just return.
	 */
	unsigned int bit = 0;

	for ( i = 0 ; i < PMDFC_MAX_STORAGE ; i++) {
		if (test_bit(PS_full, &ps_cluster->cl_storages[i]->flags))
			break;
	}

	if ( i > PMDFC_MAX_STORAGE - 1 )
		goto not_found;

	storage = ps_cluster->cl_storages[i];

	/* Don't stop it now~ */
	while(1) {
retry:
		/* TODO: BUT it may need lock or something? */
		bit = find_next_bit(storage->bitmap, PMDFC_STORAGE_SIZE, bit);
		if ( bit >= PMDFC_STORAGE_SIZE ) {
			/* No page, wait for next round */
			goto out;
		}

		/* XXX: after find bit and sleep and wake up after occupied_bit free would cause error */
		if ( bit < PMDFC_STORAGE_SIZE ) {
			key = storage->key_storage[bit];
			index = storage->index_storage[bit];
			if (!rdma) {
				/* TCP networking */
#ifdef PMDFC_NETWORK
				ret = pmnet_send_message(PMNET_MSG_PUTPAGE, key, index, 
					storage->page_storage[bit], PAGE_SIZE, 0, &status);
#endif
				clear_bit(bit, storage->bitmap);
			} else {
				/* RDMA networking */
				ret = rdpma_write_message(MSG_WRITE_REQUEST, key, index, bit,
						storage->page_storage[bit], PAGE_SIZE, 0, &status);
				clear_bit(bit, storage->bitmap);
			}
		} else {
			goto out;
		}
	}
out:
	clear_bit(PS_full, &storage->flags);
	set_bit(PS_empty, &storage->flags);
not_found:
	return;
}


/*  Clean cache operations implementation */
static void pmdfc_cleancache_put_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index, struct page *page)
{
#if defined(PMDFC_PUT)
	struct tmem_oid oid = *(struct tmem_oid *)&key;
	void *pg_from;
	int i;

	int status = 0;
	int ret = -1;

	unsigned char *data = (unsigned char*)&key;
	unsigned char *idata = (unsigned char*)&index;

	/* Here is irq disabled context */
	BUG_ON(!irqs_disabled());

	/* bloom filter hash input data */
	data[4] = idata[0];
	data[5] = idata[1];
	data[6] = idata[2];
	data[7] = idata[3];

	if ( pool_id < 0 ) 
		return;

	/* Check whether oid.oid[1] use */
	BUG_ON(oid.oid[1] != 0);
	BUG_ON(oid.oid[2] != 0);

#if defined(PMDFC_DEBUG)
	/* Send page to server */
	printk(KERN_INFO "pmdfc: PUT PAGE pool_id=%d key=%llx,%llx,%llx index=%lx page=%p\n", pool_id, 
		(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2], index, page);

	pr_info("CLIENT-->SERVER: PMNET_MSG_PUTPAGE\n");
#endif

	/* get page virtual address */
	pg_from = page_address(page);

	/* 
	 * 1. Find empty slot.
	 * If exist, then copy page content to it.
	 * If not, then just drop that page and return.
	 */
	unsigned int bit;
	struct pmdfc_storage *storage;
	i = 0;

	BUG_ON(ps_cluster == NULL);
	BUG_ON(ps_cluster->cl_storages[0] == NULL);

	for ( i = 0 ; i < PMDFC_MAX_STORAGE ; i++) {
		if (test_bit(PS_put, &ps_cluster->cl_storages[i]->flags))
			break;
	}

	if ( i > PMDFC_MAX_STORAGE - 1 ) {
		/* Cannot find PS_put, So make PS_empty buffer PS_put */
		for ( i = 0 ; i < 4 ; i++) {
			if (test_bit(PS_empty, &ps_cluster->cl_storages[i]->flags)) {
				set_bit(PS_put, &ps_cluster->cl_storages[i]->flags);
				clear_bit(PS_empty, &ps_cluster->cl_storages[i]->flags);
				break;
			}
		}
	}

	/* Cannot find PS_put and also PS_empty doesn't exist */
	if ( i > PMDFC_MAX_STORAGE - 1 ) {
		pmdfc_drop_puts++;
		goto out;
	}

	storage = ps_cluster->cl_storages[i];
	/* TODO: It may need lock */
retry:
	bit = find_first_zero_bit(storage->bitmap, PMDFC_STORAGE_SIZE);
	if ( bit < PMDFC_STORAGE_SIZE) {
		if (!test_and_set_bit(bit, storage->bitmap)) {
//			pr_info("PMDFC_PREALLOC: put page to storage[%u]\n", bit);
			memcpy(storage->page_storage[bit], pg_from, PAGE_SIZE);	
			storage->key_storage[bit] = (long)oid.oid[0];
			storage->index_storage[bit] = index;
			set_bit(bit, storage->bitmap);
		} else 
			goto retry;
	} else {
		/*
		 * 2. Queue work to workqueue when it full.
		 */
		clear_bit(PS_put, &storage->flags);
		set_bit(PS_full, &storage->flags);
		queue_work(pmdfc_wq, &pmdfc_remotify_work);
		goto out;
	}


#if defined(PMDFC_BLOOM_FILTER)
	ret = bloom_filter_add(bf, data, 24);
	if ( ret < 0 )
		pr_info("bloom_filter add fail\n");
#endif

#if defined(PMDFC_DEBUG)
	printk(KERN_INFO "pmdfc: PUT PAGE success\n");
#endif
out:

#endif /* PMDFC_PUT */
	return;
}

static int pmdfc_cleancache_get_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index, struct page *page)
{
#if defined(PMDFC_GET)
	struct tmem_oid oid = *(struct tmem_oid *)&key;
	char *to_va;
	char response[4096];
	int ret;

	int status;
	bool isIn = false;

	/* hash input data */
	unsigned char *data = (unsigned char*)&key;
	unsigned char *idata = (unsigned char*)&index;

	data[4] = idata[0];
	data[5] = idata[1];
	data[6] = idata[2];
	data[7] = idata[3];

	/* Check whether oid.oid[1] use */
	BUG_ON(oid.oid[1] != 0);
	BUG_ON(oid.oid[2] != 0);

	BUG_ON(page == NULL);

#if defined(PMDFC_BLOOM_FILTER)
	/* page is in or not? */
	bloom_filter_check(bf, data, 24, &isIn);

	pmdfc_total_gets++;

	/* This page is not exist in PM */
	if ( !isIn )
		goto not_exists;

	pmdfc_actual_gets++;
#endif

#if defined(PMDFC_DEBUG)
	/* Send get request and receive page */
	printk(KERN_INFO "pmdfc: GET PAGE pool_id=%d key=%llx,%llx,%llx index=%lx page=%p\n", pool_id, 
		(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2], index, page);
#endif

	if (!rdma) {

#ifdef PMDFC_NETWORK
		/* TCP networking */
		pmnet_send_recv_message(PMNET_MSG_GETPAGE, (long)oid.oid[0], index, 
				page_address(page), PAGE_SIZE, 0, &status);

		if (status != 0) {
			/* get page failed */
			pmdfc_miss_gets++;
			goto not_exists;
		} else {
			pmdfc_hit_gets++;
		}
#else
		goto not_exists;
#endif
	} 
	else {
		/* RDMA networking */
		ret = rdpma_read_message(MSG_READ_REQUEST, (long)oid.oid[0], index,
				page_address(page), PAGE_SIZE, 0, &status);

		if (status != 0) {
			/* get page failed */
			pmdfc_miss_gets++;
			goto not_exists;
		} else {
			pmdfc_hit_gets++;
		}
	}

#if defined(PMDFC_DEBUG)
	printk(KERN_INFO "pmdfc: GET PAGE success\n");
#endif

	return 0;

not_exists:
#endif  /* defined(PMDFC_GET) */
	return -1;
}

static void pmdfc_cleancache_flush_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index)
{
#if defined(PMDFC_FLUSH)
	struct tmem_oid oid = *(struct tmem_oid *)&key;
	int status;

	bool isIn = false;

	/* hash input data */
	unsigned char *data = (unsigned char*)&key;
	data[0] += index;
	
	bloom_filter_check(bf, data, 8, &isIn);

#if defined(PMDFC_DEBUG)
	printk(KERN_INFO "pmdfc: FLUSH PAGE pool_id=%d key=%llu,%llu,%llu index=%ld \n", pool_id, 
			(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2], index);
#endif

	if (!isIn) {
		goto out;
	}

	pmnet_send_message(PMNET_MSG_INVALIDATE, (long)oid.oid[0], index, 0, 0,
		   0, &status);
out:
	return;
#endif
}

static void pmdfc_cleancache_flush_inode(int pool_id,
		struct cleancache_filekey key)
{
#if defined(PMDFC_FLUSH)
#if defined(PMDFC_DEBUG)
	struct tmem_oid oid = *(struct tmem_oid *)&key;
 
	printk(KERN_INFO "pmdfc: FLUSH INODE pool_id=%d key=%llu,%llu,%llu \n", pool_id, 
			(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2]);
#endif
#endif
}

static void pmdfc_cleancache_flush_fs(int pool_id)
{
#if defined(PMDFC_FLUSH)
#if defined(PMDFC_DEBUG)
	printk(KERN_INFO "pmdfc: FLUSH FS\n");
#endif
#endif
}

static int pmdfc_cleancache_init_fs(size_t pagesize)
{
	static atomic_t pool_id = ATOMIC_INIT(0);

	printk(KERN_INFO "pmdfc: INIT FS\n");
	atomic_inc(&pool_id);

	return atomic_read(&pool_id);
}

static int pmdfc_cleancache_init_shared_fs(uuid_t *uuid, size_t pagesize)
{
	printk(KERN_INFO "pmdfc: FLUSH INIT SHARED\n");
	return -1;
}

static const struct cleancache_ops pmdfc_cleancache_ops = {
	.put_page = pmdfc_cleancache_put_page,
	.get_page = pmdfc_cleancache_get_page,
	.invalidate_page = pmdfc_cleancache_flush_page,
	.invalidate_inode = pmdfc_cleancache_flush_inode,
	.invalidate_fs = pmdfc_cleancache_flush_fs,
	.init_shared_fs = pmdfc_cleancache_init_shared_fs,
	.init_fs = pmdfc_cleancache_init_fs
};

static int pmdfc_cleancache_register_ops(void)
{
	int ret;

	ret = cleancache_register_ops(&pmdfc_cleancache_ops);

	return ret;
}

static int bloom_filter_init(void)
{
//	bf = bloom_filter_new(12364167);
	bf = bloom_filter_new(10000000);
	bloom_filter_add_hash_alg(bf, "md5");
	bloom_filter_add_hash_alg(bf, "sha1");
	bloom_filter_add_hash_alg(bf, "sha224");
#if 0
	bloom_filter_add_hash_alg(bf, "sha256");
	bloom_filter_add_hash_alg(bf, "sha384");
	bloom_filter_add_hash_alg(bf, "ccm");
	bloom_filter_add_hash_alg(bf, "rsa");
	bloom_filter_add_hash_alg(bf, "crc32c");
	bloom_filter_add_hash_alg(bf, "jitterentropy_rng");
#endif

	return 0;
}

/**
 * init_ps_cluster - Initialize storage cluster
 *
 * ps_cluster will store page content, key and index
 */
static int init_ps_cluster(void){
	struct pmdfc_storage_cluster *cluster = NULL;
	struct pmdfc_storage *storage = NULL;
	int i, j;

//	pr_info("pmdfc: init_ps_cluster\n");

	cluster = kzalloc(sizeof(struct pmdfc_storage_cluster), GFP_KERNEL);
	if (cluster == NULL) {
		pr_err("pmdfc: cannot allocate cluster\n");
		return -ENOMEM;
	}

	for ( i = 0 ; i < PMDFC_MAX_STORAGE ; i++) {
		storage = kzalloc(sizeof(struct pmdfc_storage), GFP_KERNEL);

		/* TODO: NUMA awareness */
//		int numa = i < 2 ? 0 : 1;
		unsigned long bitmap_size = BITS_TO_LONGS(PMDFC_STORAGE_SIZE) * sizeof(unsigned long);
		long *key_storage = kzalloc(PMDFC_STORAGE_SIZE * sizeof(long), GFP_KERNEL);
		unsigned int *index_storage = kzalloc(PMDFC_STORAGE_SIZE * sizeof(unsigned int), GFP_KERNEL);
		unsigned long *bitmap = kzalloc(bitmap_size, GFP_KERNEL);
		void **page_storage = kzalloc(PMDFC_STORAGE_SIZE * sizeof(void*), GFP_KERNEL);
		if (!page_storage){
			pr_err("pmdfc: cannot allocate page_storage\n");
			return -ENOMEM;
		}
		for ( j = 0 ; j < PMDFC_STORAGE_SIZE; j++) {
			page_storage[j] = kzalloc(PAGE_SIZE, GFP_KERNEL);
		}

//		mutex_init(&pmdfc_storages[i].lock);
		storage->bitmap_size = PMDFC_STORAGE_SIZE;
		storage->flags = 0;
		set_bit(PS_empty, &storage->flags);
		storage->page_storage = page_storage;
		storage->key_storage = key_storage;
		storage->index_storage = index_storage;
		storage->bitmap = bitmap;
		bitmap_zero(storage->bitmap, storage->bitmap_size);
		
		cluster->cl_storages[i] = storage;
//		pr_info("pmdfc: cluster->cl_storages[%d]=%p\n", i, cluster->cl_storages[i]);
	}

	ps_cluster = cluster;

	return 0;
}

void pmdfc_debugfs_exit(void)
{
	debugfs_remove_recursive(pmdfc_dentry);
}

void pmdfc_debugfs_init(void)
{
	pmdfc_dentry = debugfs_create_dir("pmdfc", NULL);
	debugfs_create_u64("total_gets", 0444, pmdfc_dentry, &pmdfc_total_gets);
	debugfs_create_u64("actual_gets", 0444, pmdfc_dentry, &pmdfc_actual_gets);
	debugfs_create_u64("miss_gets", 0444, pmdfc_dentry, &pmdfc_miss_gets);
	debugfs_create_u64("hit_gets", 0444, pmdfc_dentry, &pmdfc_hit_gets);
	debugfs_create_u64("drop_puts", 0444, pmdfc_dentry, &pmdfc_drop_puts);
}

static int __init pmdfc_init(void)
{
	int ret;
	unsigned int cpu;
	int i;

#ifdef CONFIG_DEBUG_FS
	pmdfc_debugfs_init();
#endif

	pr_info("Hostname: \tapache1\n");
	pr_info("Transport: \t%s\n", rdma ? "rdma" : "tcp");

	/*
	 * PREALLOC - make free spaces while load module 
	 * Two buffers for a one CPU node
	 * Since our testbed has two CPU node, we would create 4 buffers.
	 */
	ret = init_ps_cluster();
	if (ret != 0) {
		printk(KERN_ERR "unable to init ps_cluster\n");
		return ret;
	}
	BUG_ON(ps_cluster == NULL);
	pr_info("[  OK  ] ps_cluster initialized\n");

	/* TODO: why we use singlethread here?? */
//	pmdfc_wq = create_singlethread_workqueue("pmdfc-remotify");
//	pmdfc_wq = alloc_ordered_workqueue("pmdfc-remotify", WQ_UNBOUND | WQ_MEM_RECLAIM);
	pmdfc_wq = alloc_workqueue("pmdfc-remotify", WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_HIGHPRI, 0);
	if (pmdfc_wq == NULL) {
		printk(KERN_ERR "unable to launch pmdfc thread\n");
		return -ENOMEM;
	}
//	queue_work(pmdfc_wq, &pmdfc_remotify_work);

#if defined(PMDFC_BLOOM_FILTER)
	/* initialize bloom filter */
	bloom_filter_init();
	pr_info("[  OK  ] bloom filter initialized\n");
#endif

	ret = pmdfc_cleancache_register_ops();

	if (!ret) {
		printk(KERN_INFO "[  OK  ] cleancache_register_ops success\n");
	} else {
		printk(KERN_INFO "[ FAIL ] cleancache_register_ops fail\n");
	}

	if (cleancache_enabled) {
		printk(KERN_INFO "[  OK  ] cleancache_enabled\n");
	}
	else {
		printk(KERN_INFO "[ FAIL ] cleancache_disabled\n");
	}

	return 0;
}

/* 
 * TODO: how to exit normally??? 
 * -> We cannot. No exit for cleancache.
 * Just make operations do nothing.
 */
static void pmdfc_exit(void)
{
#if defined(PMDFC_DEBUG_FS)
	pmdfc_debugfs_exit();
#endif

	done = 1;
	wake_up_interruptible(&pmdfc_worker_wq);
	/* cancle in_process works and destory workqueue */
	cancel_work_sync(&pmdfc_remotify_work);
	destroy_workqueue(pmdfc_wq);

#if defined(PMDFC_BLOOM_FILTER)
	bloom_filter_unref(bf);
#endif
}

module_param(rdma, int, 0);

module_init(pmdfc_init);
module_exit(pmdfc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NAM JAEYOUN");
