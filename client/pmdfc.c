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
/* TODO: is it thread safe? */
struct bloom_filter *bf;

/* Currently handled oid */
struct tmem_oid coid = {.oid[0]=-1UL, .oid[1]=-1UL, .oid[2]=-1UL};

/* Global count */
atomic_t v = ATOMIC_INIT(0);
atomic_t r = ATOMIC_INIT(0);

extern int cond;

/*  Clean cache operations implementation */
static void pmdfc_cleancache_put_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index, struct page *page)
{
	struct tmem_oid oid = *(struct tmem_oid *)&key;
	void *pg_from;

	int status = 0;
	int ret = -1;

	/* hash input data */
	unsigned char *data = (unsigned char*)&key;
	unsigned char *idata = (unsigned char*)&index;

	data[4] = idata[0];
	data[5] = idata[1];
	data[6] = idata[2];
	data[7] = idata[3];

	if ( pool_id < 0 ) 
		return;

	atomic_inc(&r);
	if (atomic_read(&r) % 100 == 0 )
		pr_info("count =%d\n", atomic_read(&r));

	/* get page virtual address */
	pg_from = page_address(page);

#if defined(PMDFC_DEBUG)
	/* Send page to server */
	printk(KERN_INFO "pmdfc: PUT PAGE pool_id=%d key=%llx,%llx,%llx index=%lx page=%p\n", pool_id, 
		(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2], index, page);

	pr_info("CLIENT-->SERVER: PMNET_MSG_PUTPAGE\n");
#endif

	ret = pmnet_send_message(PMNET_MSG_PUTPAGE, (long)oid.oid[0], index, pg_from, PAGE_SIZE,
		   0, &status);

	if ( ret < 0 )
		pr_info("pmnet_send_message_fail(ret=%d)\n", ret);

	ret = bloom_filter_add(bf, data, 8);
	if ( ret < 0 )
		pr_info("bloom_filter add fail\n");

#if defined(PMDFC_DEBUG)
	printk(KERN_INFO "pmdfc: PUT PAGE success\n");
#endif
}

static int pmdfc_cleancache_get_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index, struct page *page)
{
	struct tmem_oid oid = *(struct tmem_oid *)&key;
	char *to_va;
	char response[PAGE_SIZE];
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


	/* page is in or not? */
	bloom_filter_check(bf, data, 8, &isIn);

	/* This page is not exist in PM */
	if ( !isIn )
		goto not_exists;

	goto not_exists;

	if ( atomic_read(&v) < 1000 ) {
		atomic_inc(&v);

#if defined(PMDFC_DEBUG)
		/* Send get request and receive page */
		printk(KERN_INFO "pmdfc: GET PAGE pool_id=%d key=%llx,%llx,%llx index=%lx page=%p\n", pool_id, 
			(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2], index, page);
#endif

		pmnet_send_message(PMNET_MSG_GETPAGE, (long)oid.oid[0], index, NULL, 0,
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

#if defined(PMDFC_DEBUG)
		printk(KERN_INFO "pmdfc: GET PAGE success\n");
#endif

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
}

static void pmdfc_cleancache_flush_inode(int pool_id,
		struct cleancache_filekey key)
{
#if defined(PMDFC_DEBUG)
	struct tmem_oid oid = *(struct tmem_oid *)&key;

	printk(KERN_INFO "pmdfc: FLUSH INODE pool_id=%d key=%llu,%llu,%llu \n", pool_id, 
			(long long)oid.oid[0], (long long)oid.oid[1], (long long)oid.oid[2]);
#endif
}

static void pmdfc_cleancache_flush_fs(int pool_id)
{
#if defined(PMDFC_DEBUG)
	printk(KERN_INFO "pmdfc: FLUSH FS\n");
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
	bf = bloom_filter_new(10000);
	bloom_filter_add_hash_alg(bf, "md5");
	bloom_filter_add_hash_alg(bf, "sha1");
	bloom_filter_add_hash_alg(bf, "sha224");
	bloom_filter_add_hash_alg(bf, "sha256");
	bloom_filter_add_hash_alg(bf, "sha384");
	bloom_filter_add_hash_alg(bf, "ccm");

	return 0;
}

static int __init pmdfc_init(void)
{
	int ret;


	/* initailize pmdfc's network feature */
	pmnet_init();
	pr_info(" *** mtp | network client init | network_client_init *** \n");

	/* initialize bloom filter */
	bloom_filter_init();
	pr_info(" *** bloom filter | init | bloom_filter_init *** \n");


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
}

module_init(pmdfc_init);
module_exit(pmdfc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NAM JAEYOUN");
