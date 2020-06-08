#ifndef TEST_H
#define TEST_H

#include "hotring.h"

struct item *item_create(unsigned long, unsigned int);
int item_insert(struct hash *, unsigned long);
void item_free(struct item *, unsigned long);
//int item_delete(struct hash *h, unsigned long index);
struct item *item_lookup(struct hash *h, unsigned long index);

void item_check_present(struct hash *h, unsigned long index);
void item_check_absent(struct hash *h, unsigned long index);

//void item_kill_tree(struct hash *h);

void benchmark(void);

#endif
