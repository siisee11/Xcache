#include "../queue.h"

#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>

struct queue_t* q;

int numData;
int numProducers;
int numConsumers;
int countEnqueue = 0;
int countDequeue = 0;

void clear_cache(void){
    int* dummy = (int*)malloc(sizeof(int)*1024*1024*256);
    for(int i=0; i<1024*1024*256; i++){
	dummy[i] = i;
    }

    for(int i=100; i<1024*1024*256; i++){
	dummy[i] = dummy[i-rand()%100] + dummy[i+rand()%100];
    }
    free(dummy);
}

void* producer_func(void* arg){
    struct queue_t* queue = q;
    int chunk = numData / numProducers;
    uint64_t tid = (uint64_t)arg;
    for(int i=0; i<chunk; i++){
	uint64_t value = rand();
	enqueue(queue, (void*)value);
	printf("thread[%llu] enqueued %llu\n", tid, value);
    }
    __sync_fetch_and_add(&countEnqueue, chunk);
    return NULL;
}

void* consumer_func(void* arg){
    struct queue_t* queue = q;
    int chunk = numData / numConsumers;
    uint64_t tid = (uint64_t)arg;
    for(int i=0; i<chunk; i++){
	void* value = dequeue(queue);
	printf("\tthread[%llu] dequeued %llu\n", tid, value);
        if(value)
	    __sync_fetch_and_add(&countDequeue, 1);
    }
    return NULL;
}


int main(int argc, char* argv[]){
    if(argc < 4){
	fprintf(stderr, "%s ./usage numData numProducers numConsumers\n", argv[0]);
	return 1;
    }

    numData = atoi(argv[1]);
    numProducers = atoi(argv[2]);
    numConsumers = atoi(argv[3]);
    if(numData < 0){
	fprintf(stderr, "numData should be larger than 0\n");
	return 1;
    }
/*    if(numProducers < 1 || numConsumers < 1){
	fprintf(stderr, "number of Threads should be larger than 1\n");
	return 1;
    }*/
    printf("numData(%d) numProducers(%d) numConsumers(%d)\n", numData, numProducers, numConsumers);

//    create_queue(&q);
    q = create_queue();

    srand((time(NULL)));
  
    struct timespec start, end;
    uint64_t elapsed;
    pthread_t producers[numProducers];
    pthread_t consumers[numConsumers];
    pthread_attr_t attr;
    pthread_attr_init(&attr);

//    clear_cache();
    clock_gettime(CLOCK_MONOTONIC, &start);
    for(int i=0; i<numProducers; i++){
	int ret = pthread_create(&producers[i], &attr, &producer_func, (void*)i);
	if(ret){
	    fprintf(stderr, "pthread_create failed\n");
	    return 1;
	}
    }

    for(int i=0; i<numConsumers; i++){
	int ret = pthread_create(&consumers[i], &attr, &consumer_func, (void*)i);
	if(ret){
	    fprintf(stderr, "pthread_create failed\n");
	    return 1;
	}
    }
	
    for(int i=0; i<numProducers; i++)
	pthread_join(producers[i], NULL);
    for(int i=0; i<numConsumers; i++)
	pthread_join(consumers[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &end);
    elapsed = (end.tv_nsec - start.tv_nsec) + (end.tv_sec - start.tv_sec)*1000000000;
    printf("Elapsed time(usec): %llu\n", elapsed/1000);
    printf("Throughput: %llu Ops/sec\n", (uint64_t)(1000000*(numData/(elapsed/1000.0))));

    printf("CountEnqueue: %d, CountDequeue: %d\n", countEnqueue, countDequeue);
    int empty = count_queue(q);
    printf("isempty(%d)\n", empty);
    destroy_queue(q);
/*
    pthread_t threads2[numData];
    for(int i=0; i<numData; i++){
	int ret = pthread_create(&threads2[i], &attr, &dequeue_func, NULL);
	if(ret){
	    fprintf(stderr, "pthread_create failed\n");
	    return 1;
	}
    }


    for(int i=0; i<numData; i++){
	int ret = pthread_join(threads2[i], NULL);
    }

    printf("total cnt: %d\n", count);*/
    return 0;
}

