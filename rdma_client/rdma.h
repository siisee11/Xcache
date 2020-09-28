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
#include <rdma/rdma_cm.h>

#define DEPTH 64

#define DEBUG

#ifdef DEBUG
#define dprintk(...) printk(KERN_DEBUG __VA_ARGS__)
#else
#define dprintk(...)
#endif

#define NUM_ENTRY	(4)
#define METADATA_SIZE	(16)
#define MAX_PROCESS	(64)
#define BITMAP_SIZE	(64)

#define LOCAL_META_REGION_SIZE		(MAX_PROCESS * NUM_ENTRY * METADATA_SIZE)

#define GET_LOCAL_META_REGION(addr, id)	 (addr + NUM_ENTRY * METADATA_SIZE * id)
#define GET_REMOTE_META_REGION(addr, id) (addr + NUM_ENTRY * METADATA_SIZE * id)

#define REQUEST_MAX_BATCH (4)

#define PMDFC_RDMA_CONNECT_TIMEOUT_MS 	3000

#define PMDFC_RDMA_MAX_SEGMENTS 		256

#define PMDFC_RDMA_MAX_INLINE_SEGMENTS 	4

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
	PROCESS_STATE_ABORT,
	PROCESS_STATE_DONE		/* returning read pages completed */
};

struct kmem_cache* request_cache;

char* test_mm;
uintptr_t test_ptr;

spinlock_t list_lock;

struct client_context{
	struct rdma_cm_id* cm_id;
	struct ib_context* context;
	struct ib_device 	*dev;
	struct ib_pd* pd;
	struct ib_comp_channel* channel;
	struct ib_cq* recv_cq;
	struct ib_cq* send_cq;
	struct ib_qp* qp;
	struct ib_mr* mr;
	struct ib_port_attr port_attr;

	struct kref 		kref;
	struct list_head 	req_list;
	spinlock_t   lock;

	int node_id;
	uint64_t local_mm;
	uintptr_t local_dma_addr;
	uint64_t remote_mm;
	uint32_t rkey;
	int size;
	int send_flags;
	int depth;
	atomic_t connected;

	//atomic_t* process_state;
	volatile int* process_state;

	//atomic64_t bitmap;
//	DECLARE_BITMAP(bitmap, 64);
	unsigned int bitmap_size;
	unsigned long *bitmap;
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
	struct list_head entry; // 16
	int type; // 4
	int pid;  // 4
	int64_t num; // 8
	/*
	   union{
	   uint64_t remote_mm; // 8
	   void* pages[NUM_ENTRY]; // 8*4 = 32
	   uint64_t keys[NUM_ENTRY]; // 8*4 = 32
	   };*/
};

struct pmdfc_rdma_queue;
struct pmdfc_rdma_request {
//	struct pmdfc_request	req;
	struct ib_mr		*mr;
//	struct pmdfc_rdma_qe	sqe;
//	union pmdfc_result	result;
	__le16			status;
	refcount_t		ref;
	struct ib_sge		sge[1 + PMDFC_RDMA_MAX_INLINE_SEGMENTS];
	u32			num_sge;
	int			nents;
	struct ib_reg_wr	reg_wr;
	struct ib_cqe		reg_cqe;
	struct pmdfc_rdma_queue  *queue;
	struct sg_table		sg_table;
	struct scatterlist	first_sgl[];
};

enum pmdfc_rdma_queue_flags {
	PMDFC_RDMA_Q_ALLOCATED		= 0,
	PMDFC_RDMA_Q_LIVE		= 1,
	PMDFC_RDMA_Q_TR_READY		= 2,
};

struct pmdfc_rdma_queue {
//	struct pmdfc_rdma_qe	*rsp_ring;
	int			queue_size;
	size_t			cmnd_capsule_len;
//	struct pmdfc_rdma_ctrl	*ctrl;
//	struct pmdfc_rdma_device	*device;
	struct ib_cq		*ib_cq;
	struct ib_qp		*qp;

	unsigned long		flags;
	struct rdma_cm_id	*cm_id;
	int			cm_error;
	struct completion	cm_done;
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

uint64_t ib_reg_mr_addr(void* addr, uint64_t length);
struct mr_info* ib_reg_mr(void* addr, uint64_t length, enum ib_access_flags flags);
int establish_conn(void);
int tcp_conn(void);
int tcp_recv(struct socket* sock, char* buf, int len);
int tcp_send(struct socket* sock, char* buf, int len);
int create_qp(void);
int create_cq(struct ib_cq* cq, struct ib_device* dev, int rx_depth);
//int modify_qp(int ib_port, int my_psn, enum ib_mtu mtu, int sl, struct server_info* server);
int modify_qp(int my_psn, int sl, struct node_info* server);
void cleanup_resource(void);

int generate_write_request(void** pages, uint64_t* keys, int num);
int generate_single_write_request(void*, uint64_t);
//int generate_write_request(struct page** pages, int size);
//int generate_read_request(uint64_t* keys, int size);
int generate_read_request(void** pages, uint64_t* keys, int num);
int generate_single_read_request(void* ,uint64_t);


int find_and_set_nextbit(void);
void unset_bit(int idx);

//uint64_t bit_mask(int node_id, int pid, int type, uint32_t size);
//int bit_unmask(uint64_t target, int* node_id, int* pid, int* type, uint32_t* size);
uint32_t bit_mask(int node_id, int pid, int type, int state, uint32_t num);
void bit_unmask(uint32_t target, int* node_id, int* pid, int* type, int* state, uint32_t* num);

int post_meta_request_batch(int pid, int type, int size, int tx_state, int len, void* addr, uint64_t offset, int batch_size);
//int post_read_request_batch(int pid, int type, uint32_t size, uintptr_t* addr, uint64_t offset, int batch_size);
//int post_read_request(int pid, int type, uint32_t size, uintptr_t addr, uint64_t offset);
int post_read_request_batch(uintptr_t* addr, uint64_t offset, int batch_size);
int post_write_request_batch(int pid, int type, int size, uintptr_t* addr, uint64_t offset, int batch_size);
int post_data_request(int node_id, int type, int size, uintptr_t addr, int imm_data, uint64_t offset);
int post_recv(void);

int query_qp(struct ib_qp* qp);

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
