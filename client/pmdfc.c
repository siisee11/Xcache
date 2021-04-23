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
#include "pmdfc.h"
#include "timeperf.h"

#include "rdpma.h"

#define PMDFC_PUT 1
#define PMDFC_GET 1
//#define PMDFC_TIME_CHECK 1
#define PMDFC_HASHTABLE 1

//#define PMDFC_DEBUG 1

#ifdef CONFIG_DEBUG_FS
struct dentry *pmdfc_dentry; 
#endif

#ifdef PMDFC_HASHTABLE
#define BITS 22 // 16GB=4KBx4x2^20
#define NUM_PAGES (1UL << BITS)
#define REMOTE_BUF_SIZE (PAGE_SIZE * NUM_PAGES)
DEFINE_HASHTABLE(hash_head, BITS);
#endif

// Remote buffer address mapping
atomic_long_t mr_free_start;
extern long mr_free_end;

static int onesided;
//#define SAMPLE_RATE 10000 // Per-MB
#define SAMPLE_RATE 1000000
static long put_cnt, get_cnt;

/* list, lock and condition variables */
static LIST_HEAD(page_list_head);

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

static long get_longkey(long key, long index)
{
    long longkey;
    longkey = key << 32;
    longkey |= index;
    return longkey;
}

/*  Clean cache operations implementation */
static void pmdfc_cleancache_put_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index, struct page *page)
{
#if defined(PMDFC_PUT)
	struct tmem_oid oid = *(struct tmem_oid *)&key;
	struct ht_data *tmp;
    struct ht_data *cur;

	int ret = -1;
	uint64_t longkey;

#ifdef PMDFC_TIME_CHECK
	ktime_t start = ktime_get();
#endif



#if defined(PMDFC_DEBUG)
	/* Send page to server */
	printk(KERN_INFO "pmdfc: PUT PAGE pool_id=%d key=%llx,%llx,%llx index=%lx page=%p\n", pool_id, 
		(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2], index, page);

	pr_info("CLIENT-->SERVER: PMNET_MSG_PUTPAGE\n");
#endif

    
	longkey = get_longkey((long)oid.oid[0], index);


#ifdef PMDFC_HASHTABLE
    hash_for_each_possible(hash_head, cur, h_node, longkey) {
        if (cur->longkey == longkey) {
			goto exists;
        }
    }

    tmp = (struct ht_data *)kmalloc(sizeof(struct ht_data), GFP_ATOMIC);
    tmp->longkey = get_longkey((long)oid.oid[0], index);
	if (onesided)
		tmp->roffset = atomic_long_fetch_add_unless(&mr_free_start, PAGE_SIZE, mr_free_end);
    hash_add(hash_head, &tmp->h_node, tmp->longkey);
#endif

	if (onesided) 
		ret = rdpma_put_onesided(page, tmp->roffset, 1);
	else
		ret = rdpma_put(page, longkey, 1);

    if (put_cnt % SAMPLE_RATE == 0) {
        pr_info("pmdfc: PUT PAGE: inode=%lx, index=%lx, longkey=%llx\n",
                (long)oid.oid[0], index, longkey);
#ifdef PMDFC_TIME_CHECK
		fperf_print("put_page");
#endif
    }
    put_cnt++;

#if defined(PMDFC_DEBUG)
	printk(KERN_INFO "pmdfc: PUT PAGE success\n");
#endif

#ifdef PMDFC_TIME_CHECK
	fperf_save("put_page", ktime_to_ns(ktime_sub(ktime_get(), start)));
#endif

#endif /* PMDFC_PUT */
exists:
	return;
}

static int pmdfc_cleancache_get_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index, struct page *page)
{
#if defined(PMDFC_GET)
	struct tmem_oid oid = *(struct tmem_oid *)&key;
	int ret;
    struct ht_data *cur;

    long longkey = 0, roffset = 0;

#ifdef PMDFC_TIME_CHECK
	ktime_t start = ktime_get();
#endif

#if defined(PMDFC_DEBUG)
	/* Send get request and receive page */
	printk(KERN_INFO "pmdfc: GET PAGE pool_id=%d key=%llx,%llx,%llx index=%lx page=%p\n", pool_id, 
		(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2], index, page);
#endif
    
    longkey = get_longkey((long)oid.oid[0], index);

#ifdef PMDFC_HASHTABLE
    hash_for_each_possible(hash_head, cur, h_node, longkey) {
        if (cur->longkey == longkey) {
			if (onesided)
				roffset = cur->roffset;
			goto exists;
        }
    }

	goto not_exists;

exists:
#endif /* PMDFC_HASHTABLE */

	if (get_cnt % SAMPLE_RATE == 0) {
		pr_info("pmdfc: GET PAGE: inode=%lx, index=%lx, longkey=%ld\n",
				(long)oid.oid[0], index, longkey);
#ifdef PMDFC_TIME_CHECK
		fperf_print("get_page");
#endif
	}


	if (onesided) 
		ret = rdpma_get_onesided(page, roffset, 1);
	else
		ret = rdpma_get(page, longkey, 1);

	/* send Address of page */
	get_cnt++;
	if (ret == -1)
		goto not_exists;

#ifdef PMDFC_TIME_CHECK
	fperf_save("get_page", ktime_to_ns(ktime_sub(ktime_get(), start)));
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
    struct ht_data *cur;
	int status;

	bool isIn = false;

#ifdef PMDFC_HASHTABLE
    hash_for_each_possible(hash_head, cur, h_node, longkey) {
        if (cur->longkey == longkey) {
			isIn = true;
			break;
        }
    }

	if (!isIn) {
		goto out;
	}

    hash_del(&cur->h_node);
#endif

	pmnet_send_message(PMNET_MSG_INVALIDATE, (long)oid.oid[0], index, 0, 0,
		   0, &status);

#if defined(PMDFC_DEBUG)
	printk(KERN_INFO "pmdfc: FLUSH PAGE pool_id=%d key=%llu,%llu,%llu index=%ld \n", pool_id, 
			(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2], index);
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

	atomic_inc(&pool_id);
	printk(KERN_INFO "[ INFO ] pmdfc: INIT FS (pool_id=%d)\n", atomic_read(&pool_id));

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
	//unsigned int cpu;

#ifdef CONFIG_DEBUG_FS
	pmdfc_debugfs_init();
#endif

	pr_info("Hostname: \tapache1\n");
	pr_info("Transport: \t%s\n", "rdma");
	pr_info("Method:   \t%s\n", onesided? "onesided" : "twosided");
	
#ifdef PMDFC_HASHTABLE
    hash_init(hash_head);
    pr_info("[  OK  ] hashtable initialized BITS: %d, NUM_PAGES: %lu\n", 
            BITS, NUM_PAGES);
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
}

module_param(onesided, int, 0);

module_init(pmdfc_init);
module_exit(pmdfc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NAM JAEYOUN & Daegyu");
MODULE_DESCRIPTION("Cleancache backend driver");
