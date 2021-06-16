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

static int sswap_store(unsigned type, pgoff_t pageid,
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
static int sswap_load(unsigned type, pgoff_t pageid, struct page *page)
{
	if (unlikely(rdpma_get(page, pageid, 1))) {
		pr_err("could not read page remotely\n");
		return -1;
	}

	return 0;
}

static void sswap_invalidate_page(unsigned type, pgoff_t offset)
{
	return;
}

static void sswap_invalidate_area(unsigned type)
{
	pr_err("sswap_invalidate_area\n");
}

static void sswap_init(unsigned type)
{
	pr_info("sswap_init end\n");
}

static struct frontswap_ops sswap_frontswap_ops = {
	.init = sswap_init,
	.store = sswap_store,
	.load = sswap_load,
	.invalidate_page = sswap_invalidate_page,
	.invalidate_area = sswap_invalidate_area,
};

static int __init sswap_init_debugfs(void)
{
	return 0;
}

static int __init init_sswap(void)
{
	frontswap_register_ops(&sswap_frontswap_ops);
	if (sswap_init_debugfs())
		pr_err("sswap debugfs failed\n");

	pr_info("sswap module loaded\n");
	return 0;
}

static void __exit exit_sswap(void)
{
	pr_info("unloading sswap\n");
}

module_init(init_sswap);
module_exit(exit_sswap);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("CuckooCache fastswap driver");
