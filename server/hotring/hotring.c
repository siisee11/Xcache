#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>

#include "hotring.h"
#include "error.h"

__thread int nr_request = 0;

struct hash *cur_hash;

struct head *
head_alloc(struct hash *h)
{
	struct head *ret = NULL;

	ret = (struct head *)calloc(1, sizeof(struct head));

	printv(3, "%s()::head pointer alloced\n",__func__);

	return ret;
}

struct hash_node *
hash_node_alloc(struct hash *h)
{
	struct hash_node *ret = NULL;

	ret = (struct hash_node *)calloc(1, sizeof(struct hash_node));

	if (ret) {
		ret->tag = 0;
		ret->key = 0;
		ret->value = NULL;
	}

	printv(3, "%s()::hash_node alloced\n",__func__);

	return ret;
}

struct hash*
hotring_alloc(unsigned long nbits, unsigned long kbits)
{
	struct hash *ret = NULL;

	ret = (struct hash *)calloc(1, sizeof(struct hash)); 

	if (ret) {
		ret->n = nbits;
		ret->k = kbits;
		ret->size = (1 << kbits);
		ret->slots = (void *)calloc(ret->size, sizeof(void *));
		for (int i = 0 ; i < ret->size ; i++) {
			ret->slots[i] = NULL;
		}
	}

	cur_hash = ret;

	return ret;
}

struct hash* hash_extend(struct hash *h)
{
	return NULL;
}


static struct hash_node* get_head_node_by_head_pointer(struct head* head_pointer)
{
	volatile uintptr_t iptr;
	unsigned long *ptr;

	printv(4, "%s()::head %p\n", __func__, head_pointer);


	if (head_pointer)
	{
		iptr = head_pointer->addr;
		ptr = (unsigned long *)iptr;
		return (struct hash_node *)rcu_dereference_raw(ptr);	
	}
	else
		return NULL;
}

/*
 * Get head node(first node in ring) of given index
 */
static int __hotring_get_first(struct hash *h, unsigned long index, struct hash_node **nodep) 
{
	struct head *head_pointer;
	struct hash_node *head_node;

	if (h->slots[index] != NULL) {
		head_pointer = rcu_dereference_raw(h->slots[index]);
		head_node = get_head_node_by_head_pointer(head_pointer);
		*nodep = head_node;
		return 1;
	}
	return 0;
}

/*
 * Get largest node (largest key node)
 */
static int __hotring_get_largest(struct hash *h, unsigned long hashIndex, struct hash_node **nodep) 
{
	struct head *head_pointer;
	struct hash_node *prev, *next, *head_node;
	struct list_head *p;

	unsigned long hashTag;

	if (h->slots[hashIndex] != NULL) {
		head_pointer = rcu_dereference_raw(h->slots[hashIndex]);
		head_node = get_head_node_by_head_pointer(head_pointer);

		prev = head_node;
		list_for_each(p, &head_node->list) {
			next = list_entry(p, struct hash_node, list);

			/* 
			 * if next node is smaller than prev node,
			 * it indicates the end of the ring.
			 */
			if (prev->tag > next->tag) {
				break;
			}
			prev = next;

		}
		*nodep = prev;
		
		return 1;
	}
	return 0;
}


/* possible outcome of hotspot_shift() */
typedef enum {
	GET_NOT_FOUND,
	GET_SUCCESS,
	GET_ACCESS_HOT,
	GET_ACCESS_COLD,
} hotring_get_t;

/*
 * @h : hash
 * @key : target key
 * @nodep : node having target key
 * @prevp : previous node of target node (NULL if accessed node is head node)
 * @return : 0 on fail, 1 on success
 */
static hotring_get_t __hotring_get(struct hash *h, unsigned long key, struct hash_node **nodep, struct hash_node **prevp) 
{
	struct head *head_pointer;
	struct hash_node *tmp, *head_node, *prev;
	struct list_head *p;

	volatile uintptr_t iptr;
	unsigned long *ptr;
	unsigned long hashTag;
	unsigned long hashIndex = hashCode(h, key, &hashTag);  

	if (h->slots[hashIndex] != NULL) {
		head_pointer = rcu_dereference_raw(h->slots[hashIndex]);
		head_pointer->counter++;

		head_node = get_head_node_by_head_pointer(head_pointer);
		*prevp = NULL;
		if (head_node->key == key) {
			head_node->list.counter++;
			*nodep = head_node;
			return GET_ACCESS_HOT;
		} else {
			prev = head_node;
			list_for_each(p, &head_node->list) {
				tmp = list_entry(p, struct hash_node, list);
				if (tmp->key == key) {
					tmp->list.counter++;
					*nodep = tmp;
					*prevp = prev;
					return GET_ACCESS_COLD;
				}
				if (prev->key < key && tmp->key > key)
					goto not_found;
				if (key < tmp->key && tmp->key < prev->key)
					goto not_found;
				if (tmp->key < prev->key && prev->key < key)
					goto not_found;
				prev = tmp;
			}
		}
	}

not_found:
	*prevp = NULL;
	return GET_NOT_FOUND;
}

int hotring_get(struct hash **hashp, unsigned long key, struct hash_node **nodep, struct hash_node **prevp) 
{
	struct hash *h = *hashp;
	nr_request++;
	hotring_get_t ret = __hotring_get(h, key, nodep, prevp);

	unsigned long hashTag;
	unsigned long hashIndex = hashCode(h, key, &hashTag);  

	switch (ret) {
		case GET_SUCCESS:
			return 1;
		case GET_ACCESS_HOT:
			return 1;
		case GET_ACCESS_COLD:
			if ( nr_request >= 5 ) {
				hotspot_shift(hashp, hashIndex);
				nr_request = 0;
			}
			return 1;
		case GET_NOT_FOUND:
			return 0;
	}
}

/* index자리에 노드가 있으면 반환, 없으면 생성 */
static int __hash_create(struct hash *h,
		unsigned long index, unsigned long tag,
		struct hash_node __rcu **nodep)
{
	struct head *head_pointer = NULL; 
	struct hash_node *head_node, *prev, *next;
	struct hash_node *node = NULL;
	struct list_head *p;

	bool added = false;

	if ( h->slots[index] ) 
		head_pointer = rcu_dereference_raw(h->slots[index]);

	printv(3, "%s()::head%p\n", __func__, head_pointer);


	if (head_pointer)
	{
		head_node = get_head_node_by_head_pointer(head_pointer);	
		node = hash_node_alloc(h);
		/* tag and key comparison */
		prev = head_node;
		/* add to end of the ring*/	
		list_for_each(p, &head_node->list) {
			next = list_entry(p, struct hash_node, list);

			/*
			 * if next node is smaller than prev node,
			 * it indicates the end of the ring.
			 */
			if (prev->tag > next->tag) {
				if (tag <= next->tag || prev->tag < tag) {
					list_add( &(node->list), &(prev->list) );
					added = true;
					break;
				}
			}

			/* check condition (prev->tag < tag <= next->tag) */
			if (prev->tag < tag && tag <= next->tag){
				list_add( &(node->list), &(prev->list) );
				added = true;
				break;
			}

			prev = next;
		}
		/* if can't succeed, add to last */
		if (!added)
			list_add( &(node->list), &(prev->list) );
	}
	else
	{
		/* Empty bucket */
		head_pointer = head_alloc(h);
		rcu_assign_pointer(h->slots[index], head_pointer);
		node = hash_node_alloc(h);
		INIT_LIST_HEAD(&node->list);
		rcu_assign_pointer(head_pointer->node, node);
	}

	if (nodep) {
		node->list.counter = 1;
		*nodep = node;
	}

	return 0;
}

static inline int insert_entries(struct hash_node __rcu *node,
		unsigned long key, unsigned long tag, void *value, bool replace)
{
	if (node->value)
		return -EEXIST;
	node->key = key;
	node->tag = tag;
	rcu_assign_pointer(node->value, value);
	return 1;
}


int hotring_insert(struct hash *h, unsigned long key, void *value) 
{
	struct hash_node __rcu *node;
	int error;
	unsigned long hashTag;

	unsigned long hashIndex = hashCode(h, key, &hashTag);
	printv(3, "%s()::key= %lx, hashIndex= %lx, hashTag= %lx, value= %p\n", __func__, key, hashIndex, hashTag, value);

	error = __hash_create(h, hashIndex, hashTag, &node);
	if (error)
		return error;

	error = insert_entries(node, key, hashTag, value, false);
	if (error < 0)
		return error;

	return 0;
}

void hotring_node_rcu_free(struct rcu_head *head)
{
	struct hash_node *node =
			container_of(head, struct hash_node, rcu_head);

	memset(&node->tag, 0, sizeof(node->tag));
	memset(&node->key, 0, sizeof(node->tag));
	memset(node->value, 0, sizeof(node->value));

	INIT_LIST_HEAD(&node->list);
	free(node);
}

static inline void
hotring_head_free(struct hash *h, unsigned long key)
{
	unsigned long tag = 0;
	unsigned long hashIndex = hashCode(h, key, &tag);  

	struct head *head_pointer = 
		rcu_dereference_raw(h->slots[hashIndex]);

	h->slots[hashIndex] = NULL;

	free(head_pointer);
}


/*
 * free head_pointer for index bucket
 * and put NULL instead
 */
static inline void
hotring_head_free_by_index(struct hash *h, unsigned long index)
{
	struct head *head_pointer = 
		rcu_dereference_raw(h->slots[index]);

	h->slots[index] = NULL;

	free(head_pointer);
}


static inline void
hotring_node_free(struct hash_node *node)
{
	free(node);
//	call_rcu(&node->rcu_head, hotring_node_rcu_free);
	printv(3, "%s()::key= %lx\n",__func__, node->key);
}

bool hotring_delete_head(struct hash *h, unsigned long index)
{
	bool deleted = false;

	struct head *head_pointer = rcu_dereference_raw(h->slots[index]);

	struct hash_node *prev, *head_node, *next_head_node;
	struct list_head *p;

	head_node = get_head_node_by_head_pointer(head_pointer);

	/* 
	 * if the dummy node is then only node
	 * delete it and free head_pointer too
	 */
	if (&(head_node->list) == head_node->list.next) {
		hotring_node_free(head_node);	
		hotring_head_free_by_index(h, index);
		return true;
	}

	/* assign new head node */
	next_head_node = __iptr_to_ptr(head_node->list.addr);
	rcu_assign_pointer(head_pointer->node, next_head_node);

	/* get previous node of head_node */
	list_for_each(p, &head_node->list) {
		prev = list_entry(p, struct hash_node, list);
	}

	/* prev->list --> head_node->list.next */
	list_del( &(prev->list), &(head_node->list) );
	hotring_node_free(head_node);	

	deleted = true;

	return deleted;
}



bool hotring_delete(struct hash *h, unsigned long key)
{
	bool deleted = false;

	struct hash_node *target, *prev;
	hotring_get_t ret = __hotring_get(h, key, &target, &prev);

	if (ret != GET_NOT_FOUND) {
		if (prev != NULL)
			list_del( &(prev->list), &(target->list) );
		
		hotring_node_free(target);	

		/* target was head */
		if (prev == NULL)
		{
			hotring_head_free(h, key);
		}
		deleted = true;
	} 

	return deleted;
}

static struct hash *hotring_rehash_init(struct hash *h)
{
	struct hash *new;
	struct hash_node *node;
	struct head *head_pointer;
	int i, error;

	int hash_tag_bits = h->n - h->k;	
	unsigned long maxTag = ( (unsigned long) 1 << hash_tag_bits) - 1;
	unsigned long halfTag =  (unsigned long) 1 << (hash_tag_bits - 1);

	new = hotring_alloc(NBITS, h->k + 1);

	printv(3, "%s()::maxTag= %lx\n", __func__, maxTag);

	/*
	 * Insert two milestone
	 * The order of insertion should not be modified.
	 */
	for ( i = 0 ; i < h->size ; i++){
		if ( h->slots[i] ) {
			/* insert tag T/2 node */
			error = __hash_create(h, i, halfTag, &node);
			error = insert_entries(node, (i << hash_tag_bits) + halfTag, halfTag, NULL, false);
			head_pointer = head_alloc(new);
			rcu_assign_pointer(new->slots[i * 2 + 1], head_pointer);
			rcu_assign_pointer(head_pointer->node, node);

			/* insert tag 0 node */
			error = __hash_create(h, i, 0, &node);
			error = insert_entries(node, i << hash_tag_bits, 0, NULL, false);
			head_pointer = head_alloc(new);
			rcu_assign_pointer(new->slots[i * 2], head_pointer);
			rcu_assign_pointer(head_pointer->node, node);
		}
	}

	return new;
}


struct hash *hotring_rehash(struct hash *h)
{
	struct hash *new;
	struct hash_node *ring1_head, *ring1_tail, *ring2_head, *ring2_tail;

	printv(2, "hotring_rehash\n");

	/* Initialization */
	new = hotring_rehash_init(h);

	/* Split */
	int hash_tag_bits = h->n - h->k;
	unsigned long halfTag =  (unsigned long) 1 << (hash_tag_bits - 1);

	int i;
	for (i = 0 ; i < h->size; i++){
		if ( h->slots[i] ){
			__hotring_get(h, (i << hash_tag_bits), &ring1_head, &ring2_tail);
			__hotring_get(h, (i << hash_tag_bits) + halfTag , &ring2_head, &ring1_tail);
			ring1_tail->list.next = &ring1_head->list;
			ring2_tail->list.next = &ring2_head->list;
		}
	}

	/* Deletion */
	for (i = 0 ; i < new->size; i++){
		if ( new->slots[i] ) {
			hotring_delete_head(new, i);
		}
	}

	display(new);

	return new;
}

void display(struct hash *h) {
	int i = 0;
	struct hash_node *node = NULL;
	struct hash_node *tmp;
	struct list_head *p;

	for(i = 0; i< h->size; i++) {
		printv(2, "[%d] ", i);
		if (__hotring_get_first(h, i, &node)) {
			printv(2, "(%lx, %lx, %p) \t--> ", node->key, node->tag, node->value);
			list_for_each(p, &node->list) {
				tmp = list_entry(p, struct hash_node, list);
				printv(2, "(%lx, %lx, %p) \t--> ", tmp->key, tmp->tag, tmp->value);
			}
			printv(2, "\n");
		}
		else
			printv(2, " ~~ \n");
	}
	printv(2, "\n");
}
	

/* possible outcome of hotspot_shift() */
typedef enum {
	SHIFT_REHASH,
	SHIFT_SUCCESS,
} hotspot_shift_t;

/*
 * hotspot_shift is called by hotring_get()
 * if current access is cold access
 */
static hotspot_shift_t do_hotspot_shift(struct head *head, struct hash_node *head_node) {
	/* Set Active bit */
	head->active = 1;

	double total_counter = head->counter;
	double counters[100000] = {0,};

	double min_income = 1000000000;
	double income = 0;
	int nr_hottest = 0;
	struct hash_node *hottest = NULL;
	struct hash_node *tmp = NULL;
	struct list_head *p;

	int nr_node = 0;
	counters[nr_node] = head_node->list.counter;	
	head_node->list.counter = 0;
	list_for_each(p, &head_node->list) {
		/* get counters */
		nr_node++;
		tmp = list_entry(p, struct hash_node, list);
		counters[nr_node] = tmp->list.counter;	

		/* reset counters */
		tmp->list.counter = 0;
	}
	nr_node++;


	double head_income = 0;
	/* Find hottest item index*/
	int t, i;
	for (t = 0; t < nr_node ; t++){
		income = 0;
		for (i = 0; i < nr_node ; i++) {
			income += (counters[i] / total_counter) * abs((i - t) % nr_node);
		}
		if (t == 0) {
			head_income = income;
		}
		if (income < min_income) {
			min_income = income;
			nr_hottest = t;
		}
	}

	if (min_income == head_income)
		goto stay_hot;

	/* Find hottest hash_node */
	i = 1;
	list_for_each(p, &head_node->list) {
		if (i == nr_hottest) {
			hottest = list_entry(p, struct hash_node, list);
			break;
		}
		i++;
	}

	/* assign hottest node to head pointer */
	rcu_assign_pointer(head->node, hottest);

	printv(3, "%s()::head= %p, node= %p, min_income= %lf\n", __func__, head, head_node, min_income);
	if (min_income > INCOME_THRESHOLD)
		return SHIFT_REHASH;

stay_hot:
	/* reset all counters and flags */
	head->counter = 0;
	head->active = 0;

	return SHIFT_SUCCESS;
}

void hotspot_shift(struct hash **hashp, unsigned long index) {
	struct hash *new;
	struct hash *h = *hashp;
	struct head *head_pointer = rcu_dereference_raw(h->slots[index]);
	struct hash_node *head_node = get_head_node_by_head_pointer(head_pointer);
	hotspot_shift_t ret;
	ret = do_hotspot_shift(head_pointer, head_node);
	switch (ret) {
		case SHIFT_SUCCESS:
			return;
		case SHIFT_REHASH:
			new = hotring_rehash(h);
			*hashp = new;
	}

	return;
}
