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
#include "julee.h"
#include "timeperf.h"

#include "rdpma.h"

#define julee_PUT 1
#define julee_GET 1
//#define julee_TIME_CHECK 1
//#define julee_HASHTABLE 1

//#define julee_DEBUG 1

#ifdef CONFIG_DEBUG_FS
struct dentry *julee_dentry; 
#endif

#ifdef julee_HASHTABLE
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
 * Counters available via /sys/kernel/debug/julee (if debugfs is
 * properly configured.  These are for information only so are not protected
 * against increment races.
 * HEY, is BF works?
 */
static u64 julee_total_gets;
static u64 julee_actual_gets;
static u64 julee_miss_gets;
static u64 julee_hit_gets;
static u64 julee_drop_puts;

static long get_longkey(long key, long index)
{
    long longkey;
    longkey = key << 32;
    longkey |= index;
    return longkey;
}

/*  Clean cache operations implementation */
static void julee_cleancache_put_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index, struct page *page)
{
#if defined(julee_PUT)
	struct tmem_oid oid = *(struct tmem_oid *)&key;
	struct ht_data *tmp;
    struct ht_data *cur;

	int ret = -1;
	uint64_t longkey;

#ifdef julee_TIME_CHECK
	ktime_t start = ktime_get();
#endif



#if defined(julee_DEBUG)
	/* Send page to server */
	printk(KERN_INFO "julee: PUT PAGE pool_id=%d key=%llx,%llx,%llx index=%lx page=%p\n", pool_id, 
		(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2], index, page);

	pr_info("CLIENT-->SERVER: PMNET_MSG_PUTPAGE\n");
#endif

    
	longkey = get_longkey((long)oid.oid[0], index);


#ifdef julee_HASHTABLE
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
        pr_info("julee: PUT PAGE: inode=%lx, index=%lx, longkey=%llx\n",
                (long)oid.oid[0], index, longkey);
#ifdef julee_TIME_CHECK
		fperf_print("put_page");
#endif
    }
    put_cnt++;

#if defined(julee_DEBUG)
	printk(KERN_INFO "julee: PUT PAGE success\n");
#endif

#ifdef julee_TIME_CHECK
	fperf_save("put_page", ktime_to_ns(ktime_sub(ktime_get(), start)));
#endif

#endif /* julee_PUT */
exists:
	return;
}

static int julee_cleancache_get_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index, struct page *page)
{
#if defined(julee_GET)
	struct tmem_oid oid = *(struct tmem_oid *)&key;
	int ret;
    struct ht_data *cur;

    long longkey = 0, roffset = 0;

#ifdef julee_TIME_CHECK
	ktime_t start = ktime_get();
#endif

#if defined(julee_DEBUG)
	/* Send get request and receive page */
	printk(KERN_INFO "julee: GET PAGE pool_id=%d key=%llx,%llx,%llx index=%lx page=%p\n", pool_id, 
		(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2], index, page);
#endif
    
    longkey = get_longkey((long)oid.oid[0], index);

#ifdef julee_HASHTABLE
    hash_for_each_possible(hash_head, cur, h_node, longkey) {
        if (cur->longkey == longkey) {
			if (onesided)
				roffset = cur->roffset;
			goto exists;
        }
    }

	goto not_exists;

exists:
#endif /* julee_HASHTABLE */

	if (get_cnt % SAMPLE_RATE == 0) {
		pr_info("julee: GET PAGE: inode=%lx, index=%lx, longkey=%ld\n",
				(long)oid.oid[0], index, longkey);
#ifdef julee_TIME_CHECK
		fperf_print("get_page");
#endif
	}


	/* get page from server */
	if (onesided) 
		ret = rdpma_get_onesided(page, roffset, 1);
	else
		ret = rdpma_get(page, longkey, 1);

	get_cnt++;
	if (ret == -1)
		goto not_exists;

#ifdef julee_TIME_CHECK
	fperf_save("get_page", ktime_to_ns(ktime_sub(ktime_get(), start)));
#endif

	return 0;

not_exists:
#endif  /* defined(julee_GET) */

	return -1;
}

static void julee_cleancache_flush_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index)
{
#if defined(julee_FLUSH)
	struct tmem_oid oid = *(struct tmem_oid *)&key;
    struct ht_data *cur;
	int status;

	bool isIn = false;

#ifdef julee_HASHTABLE
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

#if defined(julee_DEBUG)
	printk(KERN_INFO "julee: FLUSH PAGE pool_id=%d key=%llu,%llu,%llu index=%ld \n", pool_id, 
			(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2], index);
#endif


out:
	return;
#endif
}

static void julee_cleancache_flush_inode(int pool_id,
		struct cleancache_filekey key)
{
#if defined(julee_FLUSH)
#if defined(julee_DEBUG)
	struct tmem_oid oid = *(struct tmem_oid *)&key;
 
	printk(KERN_INFO "julee: FLUSH INODE pool_id=%d key=%llu,%llu,%llu \n", pool_id, 
			(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2]);
#endif
#endif
}

static void julee_cleancache_flush_fs(int pool_id)
{
#if defined(julee_FLUSH)
#if defined(julee_DEBUG)
	printk(KERN_INFO "julee: FLUSH FS\n");
#endif
#endif
}

static int julee_cleancache_init_fs(size_t pagesize)
{
	static atomic_t pool_id = ATOMIC_INIT(0);

	atomic_inc(&pool_id);
	printk(KERN_INFO "[ INFO ] julee: INIT FS (pool_id=%d)\n", atomic_read(&pool_id));

	return atomic_read(&pool_id);
}

static int julee_cleancache_init_shared_fs(uuid_t *uuid, size_t pagesize)
{
	printk(KERN_INFO "julee: FLUSH INIT SHARED\n");
	return -1;
}

static const struct cleancache_ops julee_cleancache_ops = {
	.put_page = julee_cleancache_put_page,
	.get_page = julee_cleancache_get_page,
	.invalidate_page = julee_cleancache_flush_page,
	.invalidate_inode = julee_cleancache_flush_inode,
	.invalidate_fs = julee_cleancache_flush_fs,
	.init_shared_fs = julee_cleancache_init_shared_fs,
	.init_fs = julee_cleancache_init_fs
};

static int julee_cleancache_register_ops(void)
{
	int ret;

	ret = cleancache_register_ops(&julee_cleancache_ops);

	return ret;
}

void julee_debugfs_exit(void)
{
	debugfs_remove_recursive(julee_dentry);
}

void julee_debugfs_init(void)
{
	julee_dentry = debugfs_create_dir("julee", NULL);
	debugfs_create_u64("total_gets", 0444, julee_dentry, &julee_total_gets);
	debugfs_create_u64("actual_gets", 0444, julee_dentry, &julee_actual_gets);
	debugfs_create_u64("miss_gets", 0444, julee_dentry, &julee_miss_gets);
	debugfs_create_u64("hit_gets", 0444, julee_dentry, &julee_hit_gets);
	debugfs_create_u64("drop_puts", 0444, julee_dentry, &julee_drop_puts);
}

static int __init julee_init(void)
{
	int ret;
	//unsigned int cpu;

#ifdef CONFIG_DEBUG_FS
	julee_debugfs_init();
#endif

	pr_info("[ INFO ] * CKCache BACKEND *\n");
	pr_info("\t  +-- Hostname\t: apache1\n");
	pr_info("\t  +-- Method  \t: %s\n", onesided? "onesided" : "twosided");
#ifdef julee_HASHTABLE
	pr_info("\t  +-- julee_HASHTABLE on\n");
#endif
#ifdef julee_GET
	pr_info("\t  +-- julee_GET on\n");
#endif
#ifdef julee_PUT  
	pr_info("\t  +-- julee_PUT on\n");
#endif

#ifdef julee_HASHTABLE
    hash_init(hash_head);
    pr_info("[  OK  ] hashtable initialized BITS: %d, NUM_PAGES: %lu\n", 
            BITS, NUM_PAGES);
#endif

	ret = julee_cleancache_register_ops();

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
static void julee_exit(void)
{
#if defined(julee_DEBUG_FS)
	julee_debugfs_exit();
#endif
}

module_param(onesided, int, 0);

module_init(julee_init);
module_exit(julee_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NAM JAEYOUN & Daegyu");
MODULE_DESCRIPTION("Cleancache backend driver");
