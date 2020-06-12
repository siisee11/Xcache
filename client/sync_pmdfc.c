#include <linux/cleancache.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/uuid.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <asm/delay.h>

#include "tmem.h"
#include "tcp.h"
#include "bloom_filter.h"

/* Allocation flags */
#define PMDFC_GFP_MASK  (GFP_ATOMIC | __GFP_NORETRY | __GFP_NOWARN)

/* Initial page pool: 32 MB (2^13 * 4KB) in pages */
#define PMDFC_ORDER 13

/* bloom filter */
struct bloom_filter *bf;

/* The pool holding the compressed pages */
//extern struct page* page_pool;

/* Currently handled oid */
struct tmem_oid coid = {.oid[0]=-1UL, .oid[1]=-1UL, .oid[2]=-1UL};

/* Global count */
atomic_t v = ATOMIC_INIT(0);
atomic_t r = ATOMIC_INIT(0);

/* wait queue for cleancache_get_page */
extern wait_queue_head_t get_page_wait_queue;
extern int cond;

/*  Clean cache operations implementation */
static void pmdfc_cleancache_put_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index, struct page *page)
{
	struct tmem_oid oid = *(struct tmem_oid *)&key;
	void *pg_from;
	void *pg_to;

	char reply[1024];
	char response[1024];
	int status = 0;
	int ret = -1;

	/* hash input data */
	unsigned char *data = (unsigned char*)&key;
	data[0] += index;

	if ( pool < 0 ) 
		return;


//	if (!tmem_oid_valid(&coid)) {
/*
	printk(KERN_INFO "pmdfc: PUT PAGE pool_id=%d key=%llu,%llu,%llu index=%ld page=%p\n", pool_id, 
			(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2], index, page);
*/
//		coid = oid;
//	tmem_oid_print(&coid);
	if ( atomic_read(&r) < 3 ) {
		atomic_inc(&r);
		/* get page virtual address */
		pg_from = page_address(page);

		/* get page from server */
		memset(&reply, 0, 1024);
		strcat(reply, "PUTPAGE"); 

		/* Send page to server */

		pr_info("CLIENT-->SERVER: PMNET_MSG_PUTPAGE\n");
		ret = pmnet_send_message(PMNET_MSG_PUTPAGE, (long)oid.oid[0], index, pg_from, PAGE_SIZE,
			   0, &status);

		ret = bloom_filter_add(bf, data, 8);
		if ( ret < 0 )
			pr_info("bloom_filter add fail\n");
		printk(KERN_INFO "pmdfc: PUT PAGE success\n");
	} /* if */
}

static int pmdfc_cleancache_get_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index, struct page *page)
{
//	u32 ind = (u32) index;
	struct tmem_oid oid = *(struct tmem_oid *)&key;
	char *to_va, *from_va;
	int ret;

	char response[PAGE_SIZE];
	char reply[1024];

	int status;
	bool isIn = false;

	/* hash input data */
	unsigned char *data = (unsigned char*)&key;
	data[0] += index;
	
	bloom_filter_check(bf, data, 8, &isIn);

	/* This page is not exist in PM */
	if ( !isIn )
		goto not_exists;

//	printk(KERN_INFO "pmdfc: GET PAGE pool_id=%d key=%llu,%llu,%llu index=%ld page=%p\n", pool_id, 
//			(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2], index, page);

//	if ( tmem_oid_compare(&coid, &oid) == 0 && atomic_read(&v) == 0 ) {
	if ( atomic_read(&v) < 3 ) {
		atomic_inc(&v);
		printk(KERN_INFO "pmdfc: GET PAGE start\n");

		/* get page from server */
		memset(&reply, 0, 1024);
		strcat(reply, "GETPAGE"); 

		pmnet_send_message(PMNET_MSG_GETPAGE, (long)oid.oid[0], index, &reply, sizeof(reply),
			   0, &status);

		ret = pmnet_recv_message(PMNET_MSG_SENDPAGE, 0, &response, PAGE_SIZE,
			0, &status);

		if (status != 0) {
			/* get page failed */
			goto not_exists;
		}
		/* copy page content from message */
		to_va = page_address(page);
		memcpy(to_va, response, PAGE_SIZE);

		printk(KERN_INFO "pmdfc: GET PAGE success\n");

		return 0;
	} /* if */

not_exists:
	return -1;
}

static void pmdfc_cleancache_flush_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index)
{
	struct tmem_oid oid = *(struct tmem_oid *)&key;
	char response[1024];
	char reply[1024];
	int status;

	bool isIn = false;

	/* hash input data */
	unsigned char *data = (unsigned char*)&key;
	data[0] += index;
	
	bloom_filter_check(bf, data, 8, &isIn);

//	printk(KERN_INFO "pmdfc: FLUSH PAGE pool_id=%d key=%llu,%llu,%llu index=%ld \n", pool_id, 
//			(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2], index);

	if (!isIn) {
		return;
	}

	pmnet_send_message(PMNET_MSG_INVALIDATE, (long)oid.oid[0], index, &reply, sizeof(reply),
		   0, &status);
}

static void pmdfc_cleancache_flush_inode(int pool_id,
		struct cleancache_filekey key)
{
	struct tmem_oid oid = *(struct tmem_oid *)&key;
//	printk(KERN_INFO "pmdfc: FLUSH INODE pool_id=%d key=%llu,%llu,%llu \n", pool_id, 
//			(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2]);
}

static void pmdfc_cleancache_flush_fs(int pool_id)
{
	printk(KERN_INFO "pmdfc: FLUSH FS\n");
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
	bf = bloom_filter_new(1000);
	bloom_filter_add_hash_alg(bf, "md5");
	bloom_filter_add_hash_alg(bf, "sha1");
	bloom_filter_add_hash_alg(bf, "sha256");

	return 0;
}

static int __init pmdfc_init(void)
{
	int ret;

	// -- initialize the WAIT QUEUE head
	init_waitqueue_head(&get_page_wait_queue);

	/* initailize pmdfc's network feature */
	pmnet_init();
	pr_info(" *** mtp | network client init | network_client_init *** \n");

	/* initialize bloom filter */
	bloom_filter_init();
	pr_info(" *** bloom filter | init | bloom_filter_init *** \n");


	/* TODO: alloc many pages */
	//	page_pool = alloc_pages(PMDFC_GFP_MASK, PMDFC_ORDER);
//	page_pool = alloc_page(PMDFC_GFP_MASK);

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

/* TODO: how to exit normally??? */
static void pmdfc_exit(void)
{
	bloom_filter_unref(bf);
	pmnet_exit();

//	__free_page(page_pool);
}

module_init(pmdfc_init);
module_exit(pmdfc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NAM JAEYOUN");
