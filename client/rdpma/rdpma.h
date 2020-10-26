#ifndef RDPMA_H
#define RDPMA_H

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
#define RDPMA_DEBUG

#ifdef DEBUG
#define dprintk(...) printk(KERN_DEBUG __VA_ARGS__)
#else
#define dprintk(...)
#endif

#ifdef RDPMA_DEBUG
#define rdpmadebug(fmt, args...) pr_debug("%s(): " fmt, __func__ , ##args)
#else
/* sigh, pr_debug() causes unused variable warnings */
static inline __printf(1, 2)
void rdpmadebug(char *fmt, ...)
{
}
#endif

extern struct client_context* ctx;

#define NUM_ENTRY		(1)
#define METADATA_SIZE	(16)  		/* [ key, remote address ] */ 
#define MAX_PROCESS	(4096) 			/* same as PMDFC_STORAGE_SIZE */
#define BITMAP_SIZE	(64)

#define LOCAL_META_REGION_SIZE		(MAX_PROCESS * NUM_ENTRY * METADATA_SIZE)

#define GET_LOCAL_META_REGION(addr, id)	 (addr + NUM_ENTRY * METADATA_SIZE * id)
#define GET_REMOTE_ADDRESS_BASE(addr, id) (addr + NUM_ENTRY * METADATA_SIZE * id + 8)

#define REQUEST_MAX_BATCH (4)

#define PMDFC_RDMA_CONNECT_TIMEOUT_MS 	3000

#define PMDFC_RDMA_MAX_SEGMENTS 		256

#define PMDFC_RDMA_MAX_INLINE_SEGMENTS 	4

#define RDPMA_MAX_NODES 1

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

/* server TX messages */
enum{				
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

#define TYPE_STR(type)                           					\
	(MSG_WRITE_REQUEST      == type? "MSG_WRITE_REQUEST"    :       \
	 (MSG_WRITE 			== type? "MSG_WRITE"   :                \
	  (MSG_READ_REQUEST 	== type? "MSG_READ_REQUEST"  :          \
	   (MSG_READ 			== type? "MSG_READ" : "unknown"))))   

/* from rdpma.c */
extern struct kmem_cache* request_cache;

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
	int msg_num;  // 8
	int64_t num; // 4
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

struct node_info{
	int node_id;
	uint32_t lid;
	int qpn;
	int psn;
	uint64_t mm;
	uint32_t rkey;
	union ib_gid gid;
};

/* Migrate belwo to rdma_internal.h */


struct rdpma_node {
	/* this is never called from int/bh */
	spinlock_t			nn_lock;

	/* set the moment an sc is allocated and a connect is started */
	struct client_context *nn_ctx;
	/* _valid is only set after the handshake passes and tx can happen */
	unsigned			nn_ctx_valid:1;
	/* if this is set tx just returns it */
	int				nn_persistent_error;
	/* It is only set to 1 after the idle time out. */
	atomic_t			nn_timeout;

	/* threads waiting for an sc to arrive wait on the wq for generation
	 * to increase.  it is increased when a connecting socket succeeds
	 * or fails or when an accepted socket is attached. */
	wait_queue_head_t		nn_ctx_wq;

	struct idr			nn_status_idr;
	struct list_head		nn_status_list;

	/* connects are attempted from when heartbeat comes up until either hb
	 * goes down, the node is unconfigured, or a connect succeeds.
	 * connect_work is queued from set_nn_state both from hb up and from
	 * itself if a connect attempt fails and so can be self-arming.
	 * shutdown is careful to first mark the nn such that no connects will
	 * be attempted before canceling delayed connect work and flushing the
	 * queue. 
	 */
	struct delayed_work		nn_connect_work;
	unsigned long			nn_last_connect_attempt;

	/* this is queued as nodes come up and is canceled when a connection is
	 * established.  this expiring gives up on the node and errors out
	 * transmits */
	struct delayed_work		nn_connect_expired;

	/* after we give up on a socket we wait a while before deciding
	 * that it is still heartbeating and that we should do some
	 * quorum work */
	struct delayed_work		nn_still_up;
};

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

	/* ib_connection */
	struct rdpma_ib_connection *ic;

	struct kref 		kref;
	struct list_head 	req_list;
	wait_queue_head_t	req_wq;
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

	unsigned int bitmap_size;
	unsigned long *bitmap;
	uint64_t** temp_log;

};

enum rdpma_system_error {
	RDPMA_ERR_NONE = 0,
	RDPMA_ERR_NO_HNDLR,
	RDPMA_ERR_NOT_FOUND,
	RDPMA_ERR_OVERFLOW,
	RDPMA_ERR_DIED,
	RDPMA_ERR_MAX
};

struct rdpma_status_wait {
	enum rdpma_system_error ns_sys_status;
	s32			ns_status;
	int			ns_id;
	void* 		ns_page;
	wait_queue_head_t	ns_wq;
	struct list_head	ns_node_item;
};

struct rdpma_message {
	refcount_t		m_refcount;
	struct list_head	m_sock_item;
	struct list_head	m_conn_item;
	u64			m_ack_seq;
	struct in6_addr		m_daddr;
	unsigned long		m_flags;

	/* Never access m_rs without holding m_rs_lock.
	 * Lock nesting is
	 *  rm->m_rs_lock
	 *   -> rs->rs_lock
	 */
	spinlock_t		m_rs_lock;
	wait_queue_head_t	m_flush_wait;

	unsigned int		m_used_sgs;
	unsigned int		m_total_sgs;

	void			*m_final_op;

	struct {
		struct rm_atomic_op {
			int			op_type;
			union {
				struct {
					uint64_t	compare;
					uint64_t	swap;
					uint64_t	compare_mask;
					uint64_t	swap_mask;
				} op_m_cswp;
				struct {
					uint64_t	add;
					uint64_t	nocarry_mask;
				} op_m_fadd;
			};

			u32			op_rkey;
			u64			op_remote_addr;
			unsigned int		op_notify:1;
			unsigned int		op_recverr:1;
			unsigned int		op_mapped:1;
			unsigned int		op_silent:1;
			unsigned int		op_active:1;
			struct scatterlist	*op_sg;
			struct rdpma_notifier	*op_notifier;

			struct rdpma_mr		*op_rdma_mr;
		} atomic;
		struct rm_rdma_op {
			u32			op_rkey;
			u64			op_remote_addr;
			unsigned int		op_write:1;
			unsigned int		op_fence:1;
			unsigned int		op_notify:1;
			unsigned int		op_recverr:1;
			unsigned int		op_mapped:1;
			unsigned int		op_silent:1;
			unsigned int		op_active:1;
			unsigned int		op_bytes;
			unsigned int		op_nents;
			unsigned int		op_count;
			struct scatterlist	*op_sg;
			struct rdpma_notifier	*op_notifier;

			struct rdpma_mr		*op_rdma_mr;
		} rdma;
	};
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

/* rdpma.c */
int rdpma_write_message(u32 msg_type, u32 key, u32 index, u32 bit, void *page, 
			u32 len, u8 target_node, int *status);
int rdpma_read_message(u32 msg_type, u32 key, u32 index, void *data, 
			u32 len, u8 target_node, int *status);
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
int post_read_request_batch(uintptr_t* addr, uint64_t offset, int batch_size);
//int post_write_request_batch(int pid, int type, int size, uintptr_t* addr, uintptr_t, int batch_size);
int post_write_request_batch(int pid, int type, int size, uintptr_t* addr, uint64_t offset, int batch_size);
int post_data_request(int node_id, int type, int size, uintptr_t addr, int imm_data, uint64_t offset);
int post_recv(void);
int rdpma_post_recv(void);
int query_qp(struct ib_qp* qp);
int send_message(int node_id, int type, void* addr, int size, uint64_t inbox_addr);
int recv_message(int node_id);

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

/* from ib.c */
int rdpma_ib_init(void);
void rdpma_ib_exit(void);

/* from rdpma.c */
void rdpma_complete_nsw(struct rdpma_node *nn,
			       struct rdpma_status_wait *nsw,
			       u64 id, enum rdpma_system_error sys_status,
			       s32 status);
struct rdpma_node * rdpma_nn_from_num(u8 node_num);

#endif
