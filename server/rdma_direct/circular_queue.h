#ifndef QUEUE_H
#define QUEUE_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include "util/common.h"

//#define CAS(_p, _u, _v) (__sync_bool_compare_and_swap (_p, _u, _v))

/************************************************************

  Lock-free queue implementation based on the paper
  "Implementing Lock-Free Queues" (John D. Valois) in ICPADS.

 ***********************************************************/


struct node_t{
	void* value;
};

struct queue_t{
	struct node_t* buffer; //for circular queue
	volatile unsigned long front;
	volatile unsigned long rear;
	char *name;
};

/* queue.cpp */
struct queue_t* create_queue(const char *);
void destroy_queue(struct queue_t* q);
int count_queue(struct queue_t* q);
void enqueue(struct queue_t* q, void* value);
void* dequeue(struct queue_t* q);


//debug
void* getElement(struct queue_t*, int);
#endif
