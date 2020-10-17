#ifndef NET_H
#define NET_H

#include <linux/socket.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/mman.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include <infiniband/verbs.h>

#include "queue.h"
#include "CCEH.h"
#include "log.h"

#define LOG_SIZE (42949672960) // 40GB
#define INDEX_SIZE (10737418240) // 10GB

void die(const char* str){
    fprintf(stderr, "%s\n", str);
    exit(EXIT_FAILURE);
}

#define TEST_NZ do{ if( (x)) die("Error: " #x " failed (returned non-zero).");}while(0)
#define TEST_Z  do{ if(!(x)) die("Error: " #x " failed (returned zero).");}while(0)

//#define MAX_NODE 				(64)
#define MAX_NODE 				(1)
#define SERVER_NODE_ID 				(MAX_NODE)
#define NUM_ENTRY					(4)
//#define REGION_PER_NODE				(4)
#define METADATA_SIZE				(16)
//#define METADATA_SIZE				(4112)
#define PAGE_SIZE 				(4096)
#define MAX_PROCESS				(64)

#define LOCAL_META_REGION_SIZE			(MAX_NODE * MAX_PROCESS * NUM_ENTRY * METADATA_SIZE)
#define PER_NODE_META_REGION_SIZE		(MAX_PROCESS * NUM_ENTRY * METADATA_SIZE)
#define PER_PROCESS_META_REGION_SIZE	(NUM_ENTRY * METADATA_SIZE)
#define GET_CLIENT_META_REGION(addr, nid, pid)	(addr + nid * PER_NODE_META_REGION_SIZE + PER_PROCESS_META_REGION_SIZE * pid)
//#define GET_CLIENT_DATA_REGION(addr, nid, pid) 	(addr + PER_NODE_META_REGION_SIZE * nid + PER_PROCESS_META_REGION_SIZE * pid + 16)

int QP_DEPTH;

enum{
    MSG_WRITE_REQUEST,
    MSG_WRITE_REQUEST_REPLY,
    MSG_WRITE,
    MSG_WRITE_REPLY,
    MSG_READ_REQUEST,
    MSG_READ_REQUEST_REPLY,
    MSG_READ,
    MSG_READ_REPLY
};

enum{				/* server TX messages */
    TX_WRITE_BEGIN,
    TX_WRITE_READY,
    TX_WRITE_COMMITTED,
    TX_WRITE_ABORTED,
    TX_READ_BEGIN,
    TX_READ_READY,
    TX_READ_COMMITTED,
    TX_READ_ABORTED,
};

struct server_context{
    struct ibv_context* context;
    struct ibv_comp_channel* channel;
    struct ibv_pd* pd;
    struct ibv_cq* recv_cq;
    struct ibv_cq* send_cq;
    struct ibv_qp** qp;
    struct ibv_port_attr port_attr;
    struct ibv_mr* mr;

    int node_id;
    uint64_t local_mm;
    void* local_ptr;
    uint64_t remote_mm[MAX_NODE];
    uint64_t rkey[MAX_NODE];
    int size;
    int rx_depth;
    int send_flags;
    int cur_node;
    int num_node;

    struct queue_t* request_queue;

    uint64_t** temp_log;
    PMEMobjpool* log_pop;
    PMEMobjpool* index_pop;
    TOID(CCEH) hashtable;
};


struct mr_info{
    uint32_t node_id;
    uint64_t addr;
    uint32_t length;
    uint32_t lkey;
    uint32_t rkey;
};

struct request_struct{
    int type;
    int node_id;
    int pid;
    uint32_t num;
};

struct node_info{
    int node_id;
    uint32_t lid;
    int qpn;
    int psn;
    uint64_t mm;
    uint32_t rkey;
    union ibv_gid gid;
};

static unsigned int my_inet_addr(const char* addr){
    int a, b, c, d;
    char inet[4];
    sscanf(addr, "%d.%d.%d.%d", &a, &b, &c, &d);
    inet[0] = a;
    inet[1] = b;
    inet[2] = c;
    inet[3] = d;

    return *(unsigned int*)inet;
}
/*
static int modify_qp(struct ibv_qp*, int, int, enum ibv_mtu, int, struct node_info*);
static struct server_context* server_init_ctx(struct ibv_device*, int, int);
int server_init_interface(void);
int establish_conn(void);
*/
//uint64_t bit_mask(int node_id, int pid, int type, uint32_t size);
uint32_t bit_mask(int node_id, int pid, int type, int state, uint32_t num);
void init_server(void);
#endif

