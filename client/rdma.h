#ifndef NET_H
#define NET_H

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mm.h>
#include <linux/net.h>
#include <linux/inet.h>
#include <linux/socket.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/memory.h>
#include <linux/semaphore.h>
#include <linux/spinlock_types.h>
#include <linux/completion.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/device.h>
#include <linux/atomic.h>
#include <linux/syscalls.h>

#include <net/sock.h>
#include <net/tcp.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_user_verbs.h>

#define DEPTH 64

//#define DEBUG
#ifdef DEBUG
#define dprintk(...) printk(KERN_DEBUG __VA_ARGS__)
#else
#define dprintk(...)
#endif

#define NUM_ENTRY	(4)
#define REQUEST_SIZE	(8)
#define MAX_PROCESS	(64)
#define BITMAP_SIZE	(64)

#define LOCAL_META_REGION_SIZE		(MAX_PROCESS * NUM_ENTRY * REQUEST_SIZE)

#define GET_LOCAL_META_REGION(addr, id)	(addr + NUM_ENTRY * REQUEST_SIZE * id)
#define GET_REMOTE_META_REGION(addr, id) (addr + NUM_ENTRY * REQUEST_SIZE * id)




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

enum{
    PROCESS_STATE_IDLE,
    PROCESS_STATE_ACTIVE,
    PROCESS_STATE_WAIT,		/* waiting for returning read pages */
    PROCESS_STATE_DONE		/* returning read pages completed */
};

struct kmem_cache* page_cache;
struct kmem_cache* request_cache;
struct kmem_cache* buffer_cache;

char* test_mm;
uintptr_t test_ptr;

spinlock_t list_lock;

struct client_context{
    struct rdma_cm_id* cm_id;
    struct ib_context* context;
    struct ib_comp_channel* channel;
    struct ib_pd* pd;
    struct ib_cq* recv_cq;
    struct ib_cq* send_cq;
    struct ib_qp* qp;
    struct ib_mr* mr;
    struct ib_port_attr port_attr;

    int node_id;
    uint64_t local_mm;
    uintptr_t local_dma_addr;
    uint64_t remote_mm;
    uint32_t rkey;
    int size;
    int send_flags;
    int depth;
    atomic_t connected;

    atomic_t* process_state;
    //atomic64_t bitmap;
    DECLARE_BITMAP(bitmap, 64);
    //uint64_t bitmap;
    uint64_t** temp_log;

};

struct mr_info{
    uint32_t node_id;
    uint64_t addr;
    uint32_t length;
    uint32_t lkey;
    uint32_t rkey;
};

struct hash_mr_info{
    uint32_t node_id;
    struct mr_info* value;
    struct hlist_node hlist;
};

struct reply_format{
    uint64_t key;
    uint64_t offset;
};

struct recv_struct{
    uint64_t addr;
    uint64_t size;
};

struct send_struct{
    uint64_t addr;
    uint64_t size;
};

struct request_struct{
    struct list_head list; // 16
    int type;	// 4
    int pid; 	// 4
	u32 key;	// 4
	u32 index;	// 4
    uint32_t num; // 4
    union{
        uint64_t remote_mm; // 8
	void* pages[NUM_ENTRY]; // 8*4 = 32
	uint64_t keys[NUM_ENTRY]; // 8*4 = 32
    };
};

/*
struct request_struct{
    int node_id;
    uint64_t mm;
    uint32_t length;
    int type;
    uint64_t addr;
    char* msg;
    struct list_head list;
};*/

struct request_struct request_list;

struct node_info{
    int node_id;
    uint32_t lid;
    int qpn;
    int psn;
    uint64_t mm;
    uint32_t rkey;
    union ib_gid gid;
};

static void poll_cq(struct ib_cq* cq, void* cq_ctx);
static void add_one(struct ib_device* dev);

uintptr_t ib_reg_mr_addr(void* addr, uint64_t length);
struct mr_info* ib_reg_mr(void* addr, uint64_t length, enum ib_access_flags flags);
static struct client_context* client_init_ctx(void);
static int client_init_interface(void);
int establish_conn(void);
int tcp_conn(void);
int tcp_recv(struct socket* sock, char* buf, int len);
int tcp_send(struct socket* sock, char* buf, int len);
int create_qp(void);
int create_cq(struct ib_cq* cq, struct ib_device* dev, int rx_depth);
//int modify_qp(int ib_port, int my_psn, enum ib_mtu mtu, int sl, struct server_info* server);
int modify_qp(int my_psn, int sl, struct node_info* server);
void init_header(int node_id, uint64_t addr, uint32_t length, int type, struct ib_header* header);
void cleanup_resource(void);

//int generate_write_request(void** pages, int size);
//int generate_write_request(struct page** pages, int size);
int generate_write_request(void** pages, u32 key, u32 index, int num);
//int generate_read_request(uint64_t* keys, int size);
//int generate_read_request(void** pages, uint64_t* keys, int size);
int generate_read_request(void** pages, u32 key, u32 index, int num);


int find_and_set_nextbit(void);
void unset_bit(int idx);

//uint64_t bit_mask(int node_id, int pid, int type, uint32_t size);
//int bit_unmask(uint64_t target, int* node_id, int* pid, int* type, uint32_t* size);
uint32_t bit_mask(int node_id, int pid, int type, int state, uint32_t num);
void bit_unmask(uint32_t target, int* node_id, int* pid, int* type, int* state, uint32_t* num);

int post_meta_request_batch(int pid, int type, int size, int tx_state, int len, void* addr, uint64_t offset, int batch_size);
int post_meta_request(int pid, int type, int size, int tx_state, int len, void* addr, uint64_t offset);
//int post_read_request_batch(int pid, int type, uint32_t size, uintptr_t* addr, uint64_t offset, int batch_size);
//int post_read_request(int pid, int type, uint32_t size, uintptr_t addr, uint64_t offset);
int post_read_request_batch(uintptr_t* addr, uint64_t offset, int batch_size);
int post_read_request(uintptr_t addr, uint64_t offset);
int post_write_request_batch(int pid, int type, int size, uintptr_t* addr, uint64_t offset, int batch_size);
int post_write_request(int pid, int type, int size, uintptr_t addr, uint64_t offset);
int post_data_request(int node_id, int type, int size, uintptr_t addr, int imm_data, uint64_t offset);
int post_recv(void);


int send_message(int node_id, int type, void* addr, int size, uint64_t inbox_addr);
int recv_message(int node_id);
int test_func(void);
int test_func2(void);
int poll_cq_test(struct ib_cq* cq, void* cq_ctx);

static inline unsigned int inet_addr(char* addr){
    int a, b, c, d;
    char inet[4];

    sscanf(addr, "%d.%d.%d.%d", &a, &b, &c, &d);
    inet[0] = a;
    inet[1] = b;
    inet[2] = c;
    inet[3] = d;
    return *(unsigned int*)inet;
}

#endif
