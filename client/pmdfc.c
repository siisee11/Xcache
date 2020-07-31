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

#define PMDFC_NETWORK 1
//#define PMDFC_DEBUG 1
#define PMDFC_GET 1
//#define PMDFC_RDMA 1
#define PMDFC_BLOOM_FILTER 1 
#define PMDFC_WORKQUEUE 1

#if !defined(PMDFC_NETWORK)
#undef PMDFC_WORKQUEUE
#endif

/* Allocation flags */
#define PMDFC_GFP_MASK  (GFP_ATOMIC | __GFP_NORETRY | __GFP_NOWARN)

#ifdef CONFIG_DEBUG_FS
	struct dentry *pmdfc_dentry; 
#endif

/* bloom filter */
/* TODO: is it thread safe? */
struct bloom_filter *bf;

/* Currently handled oid */
struct tmem_oid coid = {.oid[0]=-1UL, .oid[1]=-1UL, .oid[2]=-1UL};

struct kmem_cache* info_cache;

/* work queue */
static void pmdfc_remotify_fn(struct work_struct *work);
static struct workqueue_struct *pmdfc_wq;
static DECLARE_WORK(pmdfc_remotify_work, pmdfc_remotify_fn);

static LIST_HEAD(page_list_head);
DECLARE_WAIT_QUEUE_HEAD(pmdfc_worker_wq);
static atomic_t filled = {.count = 0};
static int done = 0;

/*
 * Counters available via /sys/kernel/debug/pmdfc (if debugfs is
 * properly configured.  These are for information only so are not protected
 * against increment races.
 */
static u64 pmdfc_total_gets;
static u64 pmdfc_actual_gets;
static u64 pmdfc_miss_gets;
static u64 pmdfc_hit_gets;

struct pmdfc_info 
{
	void *page;
	long key;
	pgoff_t index;
	struct list_head list;
};

static DEFINE_SPINLOCK(info_lock);

#if defined(PMDFC_WORKQUEUE)
/* worker function for workqueue */
static void pmdfc_remotify_fn(struct work_struct *work)
{
	struct pmdfc_info *info = NULL;
	unsigned long remain;
	void *page = NULL;
	pgoff_t index;
	long key; 
	unsigned long flags;

	int status, ret = 0;

	pr_info("pmdfc-remotify: workqueue worker running...\n");

	allow_signal(SIGUSR1);
	while (1)
	{
		wait_event_interruptible(pmdfc_worker_wq, atomic_read(&filled));

		/* module unloaded */
		if (unlikely(done == 1)) 
			return;

		spin_lock_irqsave(&info_lock, flags);

		/* if someone stole info then wait again */
		if (unlikely(list_empty(&page_list_head))) {
			atomic_andnot(0, &filled)
			spin_unlock_irqrestore(&info_lock, flags); 
			continue;
		}

		info = list_first_entry(&page_list_head, struct pmdfc_info, list);
		list_del_init(&info->list); 
		spin_unlock_irqrestore(&info_lock, flags); 

		page = info->page; 
		index = info->index;
		key = info->key; 
		ret = pmnet_send_message(PMNET_MSG_PUTPAGE, key, index, page, PAGE_SIZE,
		   0, &status);
		if ( ret < 0 )
			pr_info("pmdfc-remotify: pmnet_send_message_fail(ret=%d)\n", ret);

		kfree(page);
		kmem_cache_free(info_cache, info);
	}
}

#endif /* defined(PMDFC_WORKQUEUE) */


static struct pmdfc_info* __get_new_info(void* page, long key, pgoff_t index)
{
	struct pmdfc_info *info;

	info = kmem_cache_alloc(info_cache, GFP_ATOMIC);

	if(!info) {
		printk(KERN_ERR "memory allocation failed\n");
		return NULL;
	}

	info->page = page;
	info->key = key;
	info->index = index;

	return info;
}

/*  Clean cache operations implementation */
static void pmdfc_cleancache_put_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index, struct page *page)
{
	struct tmem_oid oid = *(struct tmem_oid *)&key;
	void *pg_from;
	void *shadow_page = NULL;
	struct pmdfc_info *info;
	unsigned long flags;

	int status = 0;
	int ret = -1;

	unsigned char *data = (unsigned char*)&key;
	unsigned char *idata = (unsigned char*)&index;

	BUG_ON(!irqs_disabled());

	/* bloom filter hash input data */
	data[4] = idata[0];
	data[5] = idata[1];
	data[6] = idata[2];
	data[7] = idata[3];

	if ( pool_id < 0 ) 
		return;

#if defined(PMDFC_DEBUG)
	/* Send page to server */
	printk(KERN_INFO "pmdfc: PUT PAGE pool_id=%d key=%llx,%llx,%llx index=%lx page=%p\n", pool_id, 
		(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2], index, page);

	pr_info("CLIENT-->SERVER: PMNET_MSG_PUTPAGE\n");
#endif

#if defined(PMDFC_NETWORK)
	/* get page virtual address */
	pg_from = page_address(page);

	/* copy page to shadow page */
	shadow_page = kmalloc(PAGE_SIZE, GFP_ATOMIC);
	if (shadow_page == NULL) {
		printk(KERN_ERR "shadow_page alloc failed! do nothing and return\n");
		return;
	}
	memcpy(shadow_page, pg_from, PAGE_SIZE);

	info = __get_new_info(shadow_page, (long)oid.oid[0], index);
	spin_lock_irqsave(&info_lock, flags);
	list_add_tail(&info->list, &page_list_head);
	spin_unlock_irqrestore(&info_lock, flags);
	atomic_and(1, &filled);
	wake_up_interruptible(&pmdfc_worker_wq);
#endif

#if defined(PMDFC_RDMA)
	ret = generate_write_request(&pg_from, (long)oid.oid[0], index, 1);
#endif 

#if defined(PMDFC_BLOOM_FILTER)
	ret = bloom_filter_add(bf, data, 24);
	if ( ret < 0 )
		pr_info("bloom_filter add fail\n");
#endif

#if defined(PMDFC_DEBUG)
	printk(KERN_INFO "pmdfc: PUT PAGE success\n");
#endif
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

#if defined(PMDFC_NETWORK)
	pmnet_send_recv_message(PMNET_MSG_GETPAGE, (long)oid.oid[0], index, 
			page, PAGE_SIZE, 0, &status);

	if (status != 0) {
		/* get page failed */
		pmdfc_miss_gets++;
		goto not_exists;
	} else {
		pmdfc_hit_gets++;
	}
#else
	goto not_exists;
#endif /* PMDFC_NETWORK end */

	return -1;

#if defined(PMDFC_RDMA)
	ret = generate_read_request(&response, (long)oid.oid[0], index, 1);

	/* copy page content from message */
	to_va = page_address(page);
	memcpy(to_va, response, PAGE_SIZE);
#endif /* PMDFC_RDMA end */


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

#if defined(PMDFC_NETWORK)
	pmnet_send_message(PMNET_MSG_INVALIDATE, (long)oid.oid[0], index, 0, 0,
		   0, &status);
#endif 
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

void pmdfc_debugfs_exit(void)
{
	debugfs_remove_recursive(pmdfc_dentry);
}

void pmdfc_debugfs_init(void)
{
	umode_t mode = S_IFREG|S_IRUSR;

	pmdfc_dentry = debugfs_create_dir("pmdfc", NULL);
	debugfs_create_u64("total_gets", 0444, pmdfc_dentry, &pmdfc_total_gets);
	debugfs_create_u64("actual_gets", 0444, pmdfc_dentry, &pmdfc_actual_gets);
	debugfs_create_u64("miss_gets", 0444, pmdfc_dentry, &pmdfc_miss_gets);
	debugfs_create_u64("hit_gets", 0444, pmdfc_dentry, &pmdfc_hit_gets);
}

static int __init pmdfc_init(void)
{
	int ret;

#ifdef CONFIG_DEBUG_FS
	pmdfc_debugfs_init();
#endif

	info_cache = kmem_cache_create("info_cache", sizeof(struct pmdfc_info),
			0, SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD, NULL);

#if defined(PMDFC_WORKQUEUE)
	/* TODO: why we use singlethread here?? */
	pmdfc_wq = create_singlethread_workqueue("pmdfc-remotify");
	if (pmdfc_wq == NULL) {
		printk(KERN_ERR "unable to launch pmdfc thread\n");
		return -ENOMEM;
	}
	queue_work(pmdfc_wq, &pmdfc_remotify_work);
#endif 

#if defined(PMDFC_BLOOM_FILTER)
	/* initialize bloom filter */
	bloom_filter_init();
	pr_info(" *** bloom filter | init | bloom_filter_init *** \n");
#endif

	ret = pmdfc_cleancache_register_ops();

	if (!ret) {
		printk(KERN_INFO ">> pmdfc: cleancache_register_ops success\n");
	} else {
		printk(KERN_INFO ">> pmdfc: cleancache_register_ops fail\n");
	}

	if (cleancache_enabled) {
		printk(KERN_INFO ">> pmdfc: cleancache_enabled\n");
	}
	else {
		printk(KERN_INFO ">> pmdfc: cleancache_disabled\n");
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

	if (info_cache)
		kmem_cache_destroy(info_cache);

#if defined(PMDFC_WORKQUEUE)
	done = 1;
	atomic_and(1, &filled);
	wake_up_interruptible(&pmdfc_worker_wq);
	/* cancle in_process works and destory workqueue */
	cancel_work_sync(&pmdfc_remotify_work);
	destroy_workqueue(pmdfc_wq);
#endif

#if defined(PMDFC_BLOOM_FILTER)
	bloom_filter_unref(bf);
#endif
}

module_init(pmdfc_init);
module_exit(pmdfc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NAM JAEYOUN");

