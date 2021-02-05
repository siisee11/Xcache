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
#include "bloom_filter.h"
#include "pmdfc.h"

#include "rdma_op.h"
#include "rdma_conn.h"

#define PMDFC_PUT 1
#define PMDFC_GET 1
#define PMDFC_DEBUG 1

#ifdef PMDFC_DEBUG_FS
struct dentry *pmdfc_dentry; 
#endif

// Hashtable
//#define BITS 21 // 8GB=4KBx2x2^20
#define BITS 22 // 16GB=4KBx4x2^20
#define NUM_PAGES (1UL << BITS)
#define REMOTE_BUF_SIZE (PAGE_SIZE * NUM_PAGES)
DEFINE_HASHTABLE(hash_head, BITS);

// Remote buffer address mapping
atomic_long_t mr_free_start;
extern uint64_t mr_free_end;

static int rdma, rdma_direct;
#ifdef PMDFC_DEBUG
#define SAMPLE_RATE 10000 // Per-MB
static long put_cnt, get_cnt;
#endif

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

static uint64_t get_longkey(long key, long index)
{
    uint64_t longkey;
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
	void *pg_from;
	int i;

	int status = 0;
	int ret = -1;
    struct ht_data *tmp;

	/* Here is irq disabled context */
	BUG_ON(!irqs_disabled());
	
	if ( pool_id < 0 ) 
		return;

	/* Check whether oid.oid[1] use */
	BUG_ON(oid.oid[1] != 0);
	BUG_ON(oid.oid[2] != 0);

	/* get page virtual address */
	pg_from = page_address(page);
    
    tmp = (struct ht_data *)kmalloc(sizeof(struct ht_data), GFP_ATOMIC);
    tmp->longkey = get_longkey((long)oid.oid[0], index);
    tmp->roffset = atomic_long_fetch_add_unless(&mr_free_start, PAGE_SIZE, mr_free_end);
    if(tmp->roffset >= mr_free_end) {
        pr_warn("[ WARN ] Remote memory is full..\n");
    }
    hash_add(hash_head, &tmp->h_node, tmp->longkey);

    if (rdma && rdma_direct) { 
        ret = pmdfc_rdma_write(page, tmp->roffset);
    }

#ifdef PMDFC_DEBUG
    if (put_cnt % SAMPLE_RATE == 0) {
        pr_info("pmdfc: PUT PAGE: inode=%lx, index=%lx, longkey=%lld, roffset=%lld\n",
                (long)oid.oid[0], index, tmp->longkey, tmp->roffset);
    }
    put_cnt++;
#endif 

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

    uint64_t longkey = 0, roffset = 0;
    struct ht_data *cur;
    atomic_t found = ATOMIC_INIT(0);

	/* Check whether oid.oid[1] use */
	BUG_ON(oid.oid[1] != 0);
	BUG_ON(oid.oid[2] != 0);

	BUG_ON(page == NULL);

    longkey = get_longkey((long)oid.oid[0], index);
    hash_for_each_possible(hash_head, cur, h_node, longkey) {
        if (cur->longkey == longkey) {
            atomic_set(&found, 1);
            roffset = cur->roffset;
        }
    }

    if (!atomic_read(&found)) {
#ifdef PMDFC_DEBUG
        if (get_cnt % SAMPLE_RATE == 0) {
            //pr_info("pmdfc: GET PAGE FAILED: not allocated...\n");
        }
        get_cnt++;
#endif
        goto not_exists;
    }

    /* RDMA networking */
    if (rdma_direct) {
#ifdef PMDFC_DEBUG
        if (get_cnt % SAMPLE_RATE == 0) {
            pr_info("pmdfc: GET PAGE: inode=%lx, index=%lx, longkey=%ld, roffset=%ld\n",
                    (long)oid.oid[0], index, longkey, roffset);
        }
        get_cnt++;
#endif
        ret = pmdfc_rdma_read_sync(page, roffset);
        pmdfc_rdma_poll_load(smp_processor_id());
        atomic_set(&found, 0);
    } 

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

void pmdfc_debugfs_exit(void)
{
#ifdef PMDFC_DEBUG_FS
	debugfs_remove_recursive(pmdfc_dentry);
#endif
}

void pmdfc_debugfs_init(void)
{
#ifdef PMDFC_DEBUG_FS
	pmdfc_dentry = debugfs_create_dir("pmdfc", NULL);
	debugfs_create_u64("total_gets", 0444, pmdfc_dentry, &pmdfc_total_gets);
	debugfs_create_u64("actual_gets", 0444, pmdfc_dentry, &pmdfc_actual_gets);
	debugfs_create_u64("miss_gets", 0444, pmdfc_dentry, &pmdfc_miss_gets);
	debugfs_create_u64("hit_gets", 0444, pmdfc_dentry, &pmdfc_hit_gets);
	debugfs_create_u64("drop_puts", 0444, pmdfc_dentry, &pmdfc_drop_puts);
#endif
}

static int __init pmdfc_init(void)
{
	int ret;
	//unsigned int cpu;

#ifdef PMDFC_DEBUG_FS
	pmdfc_debugfs_init();
#endif

	//pr_info("Hostname: \tapache1\n");
	pr_info("Transport: \t%s\n", rdma ? "rdma" : "tcp");

    hash_init(hash_head);
    pr_info("[ OK ] hashtable initialized BITS: %d, NUM_PAGES: %lu\n", 
            BITS, NUM_PAGES);

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

module_param(rdma, int, 0);
module_param(rdma_direct, int, 0);

module_init(pmdfc_init);
module_exit(pmdfc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NAM JAEYOUN & Daegyu");
MODULE_DESCRIPTION("Cleancache backend driver");
