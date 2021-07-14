#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/frontswap.h>
#include <linux/debugfs.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/page-flags.h>
#include <linux/memcontrol.h>
#include <linux/smp.h>

#include "rdpma.h"

static int ckswap_store(unsigned type, pgoff_t pageid,
		struct page *page)
{
	if (rdpma_put(page, pageid, 1)) {
		pr_err("could not store page remotely\n");
		return -1;
	}

	return 0;
}

/*
 * return 0 if page is returned
 * return -1 otherwise
 */
static int ckswap_load(unsigned type, pgoff_t pageid, struct page *page)
{
	if (unlikely(rdpma_get(page, pageid, 1))) {
		pr_err("could not read page remotely\n");
		return -1;
	}

	return 0;
}

static void ckswap_invalidate_page(unsigned type, pgoff_t offset)
{
	return;
}

static void ckswap_invalidate_area(unsigned type)
{
	pr_err("ckswap_invalidate_area\n");
}

static void ckswap_init(unsigned type)
{
	pr_info("ckswap_init end\n");
}

static struct frontswap_ops ckswap_frontswap_ops = {
	.init = ckswap_init,
	.store = ckswap_store,
	.load = ckswap_load,
	.invalidate_page = ckswap_invalidate_page,
	.invalidate_area = ckswap_invalidate_area,
};

static int __init ckswap_init_debugfs(void)
{
	return 0;
}

static int __init init_ckswap(void)
{
	frontswap_register_ops(&ckswap_frontswap_ops);
	if (ckswap_init_debugfs())
		pr_err("ckswap debugfs failed\n");

	pr_info("ckswap module loaded\n");
	return 0;
}

static void __exit exit_ckswap(void)
{
	pr_info("unloading ckswap\n");
}

module_init(init_ckswap);
module_exit(exit_ckswap);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("CuckooCache fastswap driver");
