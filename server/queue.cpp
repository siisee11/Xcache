#include "queue.h"

struct queue_t* create_queue(void){
	struct queue_t *q = (struct queue_t*)malloc(sizeof(struct queue_t));
	if(!q){
		fprintf(stderr, "[%s] failed\n", __func__);
		exit(0);
	}

	q->head = (struct node_t*)malloc(sizeof(struct node_t));
	q->head->value = q->head->next = NULL;
	q->tail = q->head;
	return q;
}

void destroy_queue(struct queue_t* q){
	struct node_t *cur, *prev;
	if(q){
		cur = q->head;
		do{
			prev = cur;
			cur = cur->next;
			free(prev);
		}while(cur);
		free(q);
	}
}

int count_queue(struct queue_t* q){
	struct node_t* cur = q->head;
	int cnt = 0;
	while(!cur){
		cur = cur->next;
		cnt++;
	}
	return cnt;
}

void enqueue(struct queue_t* q, void* value){
	struct node_t* cur;
	struct node_t* new_node = (struct node_t*)malloc(sizeof(struct node_t));
	bool succ = 0;

	new_node->value = value;
	new_node->next = NULL;
	do{
		cur = q->tail;
		succ = CAS1(&cur->next, NULL, new_node);
		if(!succ)
			CAS1(&q->tail, cur, cur->next);
	}while(!succ);

	CAS1(&q->tail, cur, new_node);
}

void* dequeue(struct queue_t* q){
	void* value;
	struct node_t* cur;
RETRY:
	do{
		cur = q->head;
		if(!cur->next)
			goto RETRY; /* empty */
	}while(!CAS1(&q->head, cur, cur->next));

	value = cur->next->value;

	free(cur);
	return value;
}



