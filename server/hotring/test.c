#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include "test.h"

struct item *item_create(unsigned long index, unsigned int data)
{
	struct item *ret = malloc(sizeof(*ret));

	ret->index = index;
	ret->data = data;
	return ret;
}

int item_insert(struct hash *h, unsigned long index)
{
	struct item *item = item_create(index, 0);
	int err = hotring_insert(h, item->index, item);
	if (err)
		free(item);
	return err;
}

void item_free(struct item *item, unsigned long index)
{
	free(item);
}

/*
int item_delete(struct hash *h, unsigned long index)
{
	struct item *item = hash_delete(h, index);

	if (!item)
		return 0;

	item_free(item, index);
	return 1;
}
*/

void item_check_present(struct hash *h, unsigned long index)
{
	int error;
	return;
}

struct item *item_lookup(struct hash *h, unsigned long index)
{
//	return hash_lookup(h, index);
	return NULL;
}

void item_check_absent(struct hash *h, unsigned long index)
{
	struct item *item;

//	item = hash_lookup(h, index);
	assert(item == NULL);
}

/*
void item_kill_tree(struct hash *h)
{
	struct hash_iter iter;
	void **slot;
	void *entry;

	hash_for_each_slot(slot, h, &iter, 0) {
		if (!xa_is_value(slot)) {
			free(*slot);
		}
		hash_replace_slot(h, slot, NULL);
	}

	assert(hash_empty(h));
}
*/
