#ifndef CUCKOO_HASH_H
#define CUCKOO_HASH_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include <pthread.h>

struct pair{
    uint64_t key;
    void* value;
};

struct cuckoo{
    size_t capacity;
    struct pair* table;
    struct pair pushed[2];
    struct pair temp;

    size_t old_cap;
    struct pair* old_table;

    int resizing_lock;
    pthread_mutex_t mutex;
    int nlocks;
    int locksize;
};


#endif
