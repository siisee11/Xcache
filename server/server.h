#ifndef PMNET_SERVER_H
#define PMNET_SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include <time.h>
#include <signal.h>
#include <stddef.h>
#include <libpmemobj.h>

#include "CCEH.h"
#include "queue.h"

#define LOG_SIZE (42949672960) // 40GB
#define INDEX_SIZE (10737418240) // 10GB

#define TIME_CHECK 1
//#define PRETEND_GET_FAIL 1
//#define NOENQUEUE 1

#define SAMPLE_RATE 100
#define NR_Q_TIME_CHECK SAMPLE_RATE
#define NR_PUT_TIME_CHECK SAMPLE_RATE
#define NR_GET_TIME_CHECK SAMPLE_RATE

#define DEBUG
#ifdef DEBUG
#define dprintf(...) do{ fprintf(stderr, __VA_ARGS__); fflush(stdout);} while(0)
#else
#define dprintf(...)
#endif

extern int tcp_port;
extern int ib_port;

/* lock free queues */
extern queue_t **lfqs;
extern unsigned int nr_cpus;

struct server_context{
    int node_id;
    void* local_ptr;
    int send_flags;
    int cur_node;
    int num_node;

    PMEMobjpool* log_pop;
    PMEMobjpool* index_pop;
    TOID(CCEH) hashtable;
};

/* server.cpp */

/* rdma.c */
void init_rdma_server(char *);
void sigint_callback_handler_rdma(int signum);

/* tcp.cpp */
int init_tcp_server(char *);
void sigint_callback_handler(int signum);
void sigsegv_callback_handler(int signum);

#endif /* PMNET_SERVER_H */
