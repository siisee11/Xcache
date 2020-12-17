#include <thread>
#include <string.h> 
#include "circular_queue.h"
#include "atomic.h"
#define QUEUE_SIZE 10000000

static volatile unsigned long Eretry =0;
static volatile unsigned long Dretry =0;
static volatile unsigned long Notretry =0;
struct queue_t* create_queue(const char *name){
	struct queue_t *q = (struct queue_t*)malloc(sizeof(struct queue_t));
	if(!q){
		fprintf(stderr, "[%s] failed\n", __func__);
		exit(0);
	}

	q->buffer = (struct node_t*)calloc(QUEUE_SIZE, sizeof(struct node_t));
	q->front = 0;
	q->rear = 0;	
	q->name = strdup(name);
	return q;
}

void destroy_queue(struct queue_t* q){
	if(q){
		free(q->buffer);
		free(q);
	}
}

int count_queue(struct queue_t* q){
	printf("q->rear= %lu, q->front= %lu\n", q->rear, q->front);
	return q->rear - q->front;
}

#if 0
void enqueue(struct queue_t* q, void* value){
	unsigned long rear = 0;
	struct node_t *cur, *buf = q->buffer;

RETRY:
	while((q->rear >= q->front) && (q->rear - q->front > QUEUE_SIZE)){
	    asm("nop");
	}
	rear = FAA(&q->rear,1) % QUEUE_SIZE; //fetch and add
	cur = &buf[rear];
	cur->value = value;
}

//dequeue value version
void* dequeue(struct queue_t* q){
	unsigned long front = 0, next = 0;
	struct node_t *cur, *buf = q->buffer;

	do{
	    while(q->front > q->rear){
			asm("nop");
	    }
	    front = q->front;
	    if(front >= q->rear){
			continue;
	    }
	}while(!CAS1(&q->front, front, front+1));

	cur = &buf[front];
	return cur->value;
}
#endif

void enqueue(struct queue_t* q, void* value){
	unsigned long rear = 0;
	struct node_t *cur, *buf = q->buffer;
	bool succ = 0;

	rear = FAA(&q->rear,1); //fetch and add
	rear = rear%QUEUE_SIZE;
	cur = &buf[rear];
	while(!CAS1(&cur->value, NULL, value)){
		asm("nop");	
	}
}

//dequeue value version
void* dequeue(struct queue_t* q){
	void* value;
	unsigned long front = 0, next = 0;
	struct node_t *cur, *buf = q->buffer;
	
	front = FAA(&q->front, 1);
	front = front%QUEUE_SIZE;
RETRY:
	do{
		cur = &buf[front];
		value = cur->value;
		if(!value)
			goto RETRY;
	}while(!CAS1(&cur->value, value, NULL));

	return value;
}

void* getElement(struct queue_t* q, int i){
	return q->buffer[i].value;
}
