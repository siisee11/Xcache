#ifndef QUEUE_H
#define QUEUE_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include "common.h"

//#define CAS(_p, _u, _v) (__sync_bool_compare_and_swap (_p, _u, _v))

/************************************************************

  Lock-free queue implementation based on the paper
  "Implementing Lock-Free Queues" (John D. Valois) in ICPADS.

 ***********************************************************/


struct node_t{
	void* value;
	struct node_t* next;
};

struct queue_t{
	struct node_t* head;
	struct node_t* tail;
};

/* queue.cpp */
struct queue_t* create_queue(void);
void destroy_queue(struct queue_t* q);
int count_queue(struct queue_t* q);
void enqueue(struct queue_t* q, void* value);
void* dequeue(struct queue_t* q);

#endif
