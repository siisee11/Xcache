/*
 * Copyright (C) 2013 Daniel Mack <daniel@zonque.org>
 *
 * Bloom filter implementation.
 * See https://en.wikipedia.org/wiki/Bloom_filter
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/hash.h>
#include <crypto/algapi.h>

#include "bloom_filter.h"
#include "hash.h"


struct bloom_filter *bloom_filter_new(unsigned int num_hash, unsigned int bit_size)
{
	struct bloom_filter *filter;
	unsigned long bitmap_size = BITS_TO_LONGS(bit_size)
				  * sizeof(unsigned long);

	filter = kzalloc(sizeof(*filter) + bitmap_size, GFP_KERNEL);
	if (!filter)
		return ERR_PTR(-ENOMEM);

	kref_init(&filter->kref);
	mutex_init(&filter->lock);
	filter->bitmap_size = bit_size;
	filter->nr_hash = num_hash;

	return filter;
}

struct bloom_filter *bloom_filter_ref(struct bloom_filter *filter)
{
	kref_get(&filter->kref);
	return filter;
}

static void __bloom_filter_free(struct kref *kref)
{
	struct bloom_crypto_alg *alg, *tmp;
	struct bloom_filter *filter =
		container_of(kref, struct bloom_filter, kref);

	kref_put(&filter->kref, __bloom_filter_free);
	kfree(filter);
	pr_info("bloom_filter freed...\n");
}

int bloom_filter_add(struct bloom_filter *filter,
		     const u8 *data, unsigned int datalen)
{
	int ret = 0;
	int i;

	for (i = 0 ; i < filter->nr_hash; i++ ) {

	}

	list_for_each_entry(alg, &filter->alg_list, entry) {
		unsigned int bit;

		ret = __bit_for_crypto_alg(alg, data, datalen, filter->bitmap_size, &bit);
		if (ret < 0)
			goto exit_unlock;
		set_bit(bit, filter->bitmap);

//		pr_info("set_bit --> %u\n", bit);
	}

	return ret;
}

int bloom_filter_check(struct bloom_filter *filter,
		       const u8 *data, unsigned int datalen,
		       bool *result)
{
	struct bloom_crypto_alg *alg;
	int ret = 0;

//	mutex_lock(&filter->lock);
	if (unlikely(list_empty(&filter->alg_list))) {
		ret = -EINVAL;
		goto exit_unlock;
	}

	*result = true;

	list_for_each_entry(alg, &filter->alg_list, entry) {
		unsigned int bit;

		ret = __bit_for_crypto_alg(alg, data, datalen, filter->bitmap_size, &bit);
		if (ret < 0)
			goto exit_unlock;

//		pr_info("Bloom filter: test_bit-->%d\n", bit);

		if (!test_bit(bit, filter->bitmap)) {
			*result = false;
			break;
		}
	}

exit_unlock:
//	mutex_unlock(&filter->lock);

	return ret;
}

void bloom_filter_set(struct bloom_filter *filter,
		      const u8 *bit_data)
{
	memcpy(filter->bitmap, bit_data,
		BITS_TO_LONGS(filter->bitmap_size) * sizeof(unsigned long));
}

void bloom_filter_reset(struct bloom_filter *filter)
{
//	mutex_lock(&filter->lock);
	bitmap_zero(filter->bitmap, filter->bitmap_size);
//	mutex_unlock(&filter->lock);
}
