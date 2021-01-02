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
#include <linux/utsname.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <asm-generic/atomic-long.h>

#include "tmem.h"
#include "pmdfc.h"
#include "bloom_filter.h"

#define PMDFC_GET 1
#define PMDFC_PUT 1

//#define BITS 20 // 2^10: 1024, 2^20: 104857 max coverage: 4 * 2^30 
#define BITS 21 // 4KB x 2 x 2^20
#define NUM_PAGES (1UL << BITS) 
#define REMOTE_BUF_SIZE (PAGE_SIZE * NUM_PAGES) 

#define SAMPLE_RATE 5000
unsigned long put_cnt = 0;
unsigned long get_cnt = 0;

// Remote buffer address mapping
atomic_long_t mr_free_start;
long mr_free_end = REMOTE_BUF_SIZE;
void *dram_buf = NULL;
DEFINE_HASHTABLE(hash_head, BITS);

// Bloom filter
struct bloom_filter *bf;

/* longkey = [key, index] */
static long get_longkey(long key, long index)
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
    int ret = -1;
    struct tmem_oid oid = *(struct tmem_oid *)&key; 
    void *page_vaddr = page_address(page);
    unsigned char *data, *idata;
    struct ht_data *tmp;
    
    BUG_ON(!irqs_disabled());
   
    /* bloom filter hash input data */
    data = (unsigned char*)&key; 
    idata = (unsigned char*)&index;

    data[4] = idata[0];
    data[5] = idata[1];
    data[6] = idata[2];
    data[7] = idata[3];

    if(pool_id < 0)
        return;

    BUG_ON(oid.oid[1] != 0);
    BUG_ON(oid.oid[2] != 0);
    
    tmp = (struct ht_data *)kzalloc(sizeof(struct ht_data), GFP_ATOMIC);

    tmp->longkey = get_longkey((long)oid.oid[0], index);
    tmp->roffset = atomic_long_fetch_add_unless(&mr_free_start, PAGE_SIZE, mr_free_end);
    if(tmp->roffset >= mr_free_end) {
        pr_warn("[ WARN ] Remote memory is full...");
    }

    if (put_cnt % SAMPLE_RATE == 0) {
        pr_info("pmdfc: PUT PAGE: inode=%lx, index=%lx, longkey=%lld, roffset=%lld\n", 
                (long)oid.oid[0], index, tmp->longkey, tmp->roffset);
    } 
    put_cnt++;

    hash_add(hash_head, &tmp->h_node, tmp->longkey);
    memcpy((void *) (dram_buf + tmp->roffset), page_vaddr, PAGE_SIZE);
    
    ret = bloom_filter_add(bf, data, 24);
    if (ret < 0) {
        pr_warn("[ WARN ] bloom_filter add fail\n");
    }
#endif
}

static int pmdfc_cleancache_get_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index, struct page *page)
{
#if defined(PMDFC_GET)
    struct tmem_oid oid = *(struct tmem_oid *)&key; 
    void *page_vaddr;
    page_vaddr = page_address(page); 
    
    unsigned char *data, *idata;
    bool isIn = false;

    long long longkey = 0, roffset = 0;
    struct ht_data *cur;
    atomic_t found = ATOMIC_INIT(0); 

    /* bloom filter hash input data */
    data = (unsigned char*)&key; 
    idata = (unsigned char*)&index;

    data[4] = idata[0];
    data[5] = idata[1];
    data[6] = idata[2];
    data[7] = idata[3];

    BUG_ON(oid.oid[1] != 0);
    BUG_ON(oid.oid[2] != 0);

    BUG_ON(page == NULL); 
    
    bloom_filter_check(bf, data, 24, &isIn); 
    if (!isIn)
        goto not_exist;

    longkey = get_longkey((long)oid.oid[0], index);  
    hash_for_each_possible(hash_head, cur, h_node, longkey) {
        if (cur->longkey == longkey) {
            atomic_set(&found, 1);
            roffset = cur->roffset;
        }
    }

    if(!atomic_read(&found)) {
        if (get_cnt % SAMPLE_RATE == 0) {
            pr_info("pmdfc: GET PAGE FAILED: not allocated...\n");
        }   
        get_cnt++;
        goto not_exist;
    } else {
        if (get_cnt % SAMPLE_RATE == 0) {
            pr_info("pmdfc: GET PAGE: inode=%lx, index=%lx, longkey=%lld, roffset=%lld\n",
                     (long)oid.oid[0], index, longkey, roffset);

        }
        memcpy(page_vaddr, (void *) (dram_buf + roffset), PAGE_SIZE);
        atomic_set(&found, 0);
        get_cnt++;
    }
    return 0;

not_exist:  
#endif
    return -1;
}

static void pmdfc_cleancache_flush_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index)
{
}

static void pmdfc_cleancache_flush_inode(int pool_id,
		struct cleancache_filekey key)
{
}

static void pmdfc_cleancache_flush_fs(int pool_id)
{
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
	pr_info("pmdfc_cleancache_init_shared_fs");
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
	bf = bloom_filter_new(139740386); // #item: 10GB=4KBx2.5M
	//bf = bloom_filter_new(279480772); // #item: 10GB=4KBx5M
	//bf = bloom_filter_new(558961543); // #item: 40GB=4KBx10M
	//bf = bloom_filter_new(1117923086); // #item: 80GB, hash: 8

	bloom_filter_add_hash_alg(bf, "md5");
	bloom_filter_add_hash_alg(bf, "sha1");
	bloom_filter_add_hash_alg(bf, "sha224");
	bloom_filter_add_hash_alg(bf, "sha256");
	bloom_filter_add_hash_alg(bf, "sha384");
	bloom_filter_add_hash_alg(bf, "ccm");
	bloom_filter_add_hash_alg(bf, "rsa");
	bloom_filter_add_hash_alg(bf, "crc32c");
    return 0;
}

static int __init pmdfc_init(void)
{
	int ret;
	
    pr_info("[ INFO ] start: %s\n", __func__);
	pr_info("[ INFO ] will use new DRAM backend, plz run app using cgroup\n");

    hash_init(hash_head);
	
    dram_buf = vzalloc(REMOTE_BUF_SIZE);
    pr_info("[ OK ] vzalloc'ed %lu GB for dram backend\n", 
            REMOTE_BUF_SIZE/1024/1024/1024); 

    bloom_filter_init();
    pr_info("[ OK ] bloom filter initialized\n");

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
    int bkt;
    struct ht_data *cur;
    struct hlist_node *tmp_hnode;

    pr_info("[ INFO ] free data structure...");

    hash_for_each_safe(hash_head, bkt, tmp_hnode, cur, h_node) {
        hash_del(&cur->h_node);
        kfree(cur);
    }
    
    vfree(dram_buf);
    bloom_filter_unref(bf);
    pr_info("[  OK  ] pmdfc_exit\n");
}


module_init(pmdfc_init);
module_exit(pmdfc_exit);

MODULE_LICENSE("GPL");
