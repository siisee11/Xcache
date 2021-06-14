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


struct bloom_filter *bloom_filter_new(uint64_t bits_addr, unsigned int num_hash, unsigned int bit_size)
{
	struct bloom_filter *filter;
	unsigned long bitmap_size = BITS_TO_LONGS(bit_size)
		* sizeof(unsigned long);

	filter = kzalloc(sizeof(*filter), GFP_KERNEL);

	if (!filter)
		return ERR_PTR(-ENOMEM);

	kref_init(&filter->kref);
	mutex_init(&filter->lock);
	filter->bitmap_size = bit_size;
	filter->bitmap_size_in_byte = bitmap_size;
	filter->nr_hash = num_hash;
	filter->bitmap = (unsigned long *)bits_addr;

	return filter;
}

struct bloom_filter *bloom_filter_ref(struct bloom_filter *filter)
{
	kref_get(&filter->kref);
	return filter;
}

static void __bloom_filter_free(struct kref *kref)
{
	struct bloom_filter *filter =
		container_of(kref, struct bloom_filter, kref);

	kref_put(&filter->kref, __bloom_filter_free);
	kfree(filter);
	pr_info("bloom_filter freed...\n");
}

int bloom_filter_add(struct bloom_filter *filter,
		     const void *data, unsigned int datalen)
{
	int ret = 0;
	int i;
	unsigned int index;

	for (i = 0 ; i < filter->nr_hash; i++ ) {
		index = hash_funcs[1](data, datalen, i) % filter->bitmap_size;

		unsigned int j = index / 64;
		uint8_t bitShift = 64 - 1 - (index % 64);   // i == 65 -> 62
		uint64_t checkBit = (uint64_t) 1 << bitShift;
		filter->bitmap[j] |= checkBit;

//		set_bit(index, filter->bitmap);
	}

	return ret;
}

int bloom_filter_check(struct bloom_filter *filter,
		       const void *data, unsigned int datalen,
		       bool *result)
{
	int ret = 0;
	unsigned int index;
	int i;

	*result = true;

	for (i = 0 ; i < filter->nr_hash; i++ ) {
		index = hash_funcs[1](data, datalen, i) % filter->bitmap_size;
		if (*(uint64_t *)data == 0) {
			pr_info("data=%lx index = %lu, ", *(uint64_t *)data, index);
		}

		unsigned int j = index / 64;
		uint8_t bitShift = 64 - 1 - (index % 64);   // i == 65 -> 62
		uint64_t checkBit = (uint64_t) 1 << bitShift;
		if ((filter->bitmap[j] & checkBit) == 0) {
			*result = false;
			break;
		}

		/*
		if (!test_bit(index, filter->bitmap)) {
			*result = false;
			break;
		}
		*/
	}

	return ret;
}

void bloom_filter_set(struct bloom_filter *filter,
		      const void *bit_data)
{
	memcpy(filter->bitmap, bit_data,
		BITS_TO_LONGS(filter->bitmap_size) * sizeof(unsigned long));
}

int bloom_filter_bitsize(struct bloom_filter *filter)
{
	return BITS_TO_LONGS(filter->bitmap_size) * sizeof(unsigned long);
}

