/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _RDS_IB_H
#define _RDS_IB_H

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include "rds.h"
#include "rdma_transport.h"

#define PMDFC_IB_MAX_SGE			8
#define PMDFC_IB_RECV_SGE 		2

#define PMDFC_IB_DEFAULT_RECV_WR		1024
#define PMDFC_IB_DEFAULT_SEND_WR		256
#define PMDFC_IB_DEFAULT_FR_WR		512

#define PMDFC_IB_DEFAULT_RETRY_COUNT	1

#define PMDFC_IB_SUPPORTED_PROTOCOLS	0x00000001	/* minor versions supported */

#define PMDFC_IB_RECYCLE_BATCH_COUNT	32

#define PMDFC_IB_WC_MAX			32

extern struct rw_semaphore rds_ib_devices_lock;
extern struct list_head rds_ib_devices;

/*
 * IB posts RDS_FRAG_SIZE fragments of pages to the receive queues to
 * try and minimize the amount of memory tied up both the device and
 * socket receive queues.
 */
struct rds_page_frag {
	struct list_head	f_item;
	struct list_head	f_cache_entry;
	struct scatterlist	f_sg;
};

struct pmdfc_ib_incoming {
	struct list_head	ii_frags;
	struct list_head	ii_cache_entry;
	struct pmdfc_incoming	ii_inc;
};

struct pmdfc_ib_cache_head {
	struct list_head *first;
	unsigned long count;
};

struct pmdfc_ib_refill_cache {
	struct pmdfc_ib_cache_head __percpu *percpu;
	struct list_head	 *xfer;
	struct list_head	 *ready;
};

/* This is the common structure for the IB private data exchange in setting up
 * an pmdfc connection.  The exchange is different for IPv4 and IPv6 connections.
 * The reason is that the address size is different and the addresses
 * exchanged are in the beginning of the structure.  Hence it is not possible
 * for interoperability if same structure is used.
 */
struct pmdfc_ib_conn_priv_cmn {
	u8			ricpc_protocol_major;
	u8			ricpc_protocol_minor;
	__be16			ricpc_protocol_minor_mask;	/* bitmask */
	u8			ricpc_dp_toss;
	u8			ripc_reserved1;
	__be16			ripc_reserved2;
	__be64			ricpc_ack_seq;
	__be32			ricpc_credit;	/* non-zero enables flow ctl */
};

struct pmdfc_ib_connect_private {
	/* Add new fields at the end, and don't permute existing fields. */
	__be32				dp_saddr;
	__be32				dp_daddr;
	struct pmdfc_ib_conn_priv_cmn	dp_cmn;
};

struct pmdfc6_ib_connect_private {
	/* Add new fields at the end, and don't permute existing fields. */
	struct in6_addr			dp_saddr;
	struct in6_addr			dp_daddr;
	struct pmdfc_ib_conn_priv_cmn	dp_cmn;
};

#define dp_protocol_major	dp_cmn.ricpc_protocol_major
#define dp_protocol_minor	dp_cmn.ricpc_protocol_minor
#define dp_protocol_minor_mask	dp_cmn.ricpc_protocol_minor_mask
#define dp_ack_seq		dp_cmn.ricpc_ack_seq
#define dp_credit		dp_cmn.ricpc_credit

union pmdfc_ib_conn_priv {
	struct pmdfc_ib_connect_private	ricp_v4;
	struct pmdfc6_ib_connect_private	ricp_v6;
};

struct pmdfc_ib_send_work {
	void			*s_op;
	union {
		struct ib_send_wr	s_wr;
		struct ib_rdma_wr	s_rdma_wr;
		struct ib_atomic_wr	s_atomic_wr;
	};
	struct ib_sge		s_sge[pmdfc_IB_MAX_SGE];
	unsigned long		s_queued;
};

struct pmdfc_ib_recv_work {
	struct pmdfc_ib_incoming 	*r_ibinc;
	struct pmdfc_page_frag	*r_frag;
	struct ib_recv_wr	r_wr;
	struct ib_sge		r_sge[2];
};

struct pmdfc_ib_work_ring {
	u32		w_nr;
	u32		w_alloc_ptr;
	u32		w_alloc_ctr;
	u32		w_free_ptr;
	atomic_t	w_free_ctr;
};

/* Rings are posted with all the allocations they'll need to queue the
 * incoming message to the receiving socket so this can't fail.
 * All fragments start with a header, so we can make sure we're not receiving
 * garbage, and we can tell a small 8 byte fragment from an ACK frame.
 */
struct pmdfc_ib_ack_state {
	u64		ack_next;
	u64		ack_recv;
	unsigned int	ack_required:1;
	unsigned int	ack_next_valid:1;
	unsigned int	ack_recv_valid:1;
};


struct pmdfc_ib_device;

struct pmdfc_ib_connection {

	struct list_head	ib_node;
	struct pmdfc_ib_device	*pmdfc_ibdev;
	struct pmdfc_connection	*conn;

	/* alphabet soup, IBTA style */
	struct rdma_cm_id	*i_cm_id;
	struct ib_pd		*i_pd;
	struct ib_cq		*i_send_cq;
	struct ib_cq		*i_recv_cq;
	struct ib_wc		i_send_wc[pmdfc_IB_WC_MAX];
	struct ib_wc		i_recv_wc[pmdfc_IB_WC_MAX];

	/* To control the number of wrs from fastreg */
	atomic_t		i_fastreg_wrs;
	atomic_t		i_fastreg_inuse_count;

	/* interrupt handling */
	struct tasklet_struct	i_send_tasklet;
	struct tasklet_struct	i_recv_tasklet;

	/* tx */
	struct pmdfc_ib_work_ring	i_send_ring;
	struct rm_data_op	*i_data_op;
	struct pmdfc_header	*i_send_hdrs;
	dma_addr_t		i_send_hdrs_dma;
	struct pmdfc_ib_send_work *i_sends;
	atomic_t		i_signaled_sends;

	/* rx */
	struct mutex		i_recv_mutex;
	struct pmdfc_ib_work_ring	i_recv_ring;
	struct pmdfc_ib_incoming	*i_ibinc;
	u32			i_recv_data_rem;
	struct pmdfc_header	*i_recv_hdrs;
	dma_addr_t		i_recv_hdrs_dma;
	struct pmdfc_ib_recv_work *i_recvs;
	u64			i_ack_recv;	/* last ACK received */
	struct pmdfc_ib_refill_cache i_cache_incs;
	struct pmdfc_ib_refill_cache i_cache_frags;
	atomic_t		i_cache_allocs;

	/* sending acks */
	unsigned long		i_ack_flags;
#ifdef KERNEL_HAS_ATOMIC64
	atomic64_t		i_ack_next;	/* next ACK to send */
#else
	spinlock_t		i_ack_lock;	/* protect i_ack_next */
	u64			i_ack_next;	/* next ACK to send */
#endif
	struct pmdfc_header	*i_ack;
	struct ib_send_wr	i_ack_wr;
	struct ib_sge		i_ack_sge;
	dma_addr_t		i_ack_dma;
	unsigned long		i_ack_queued;

	/* Flow control related information
	 *
	 * Our algorithm uses a pair variables that we need to access
	 * atomically - one for the send credits, and one posted
	 * recv credits we need to transfer to remote.
	 * Rather than protect them using a slow spinlock, we put both into
	 * a single atomic_t and update it using cmpxchg
	 */
	atomic_t		i_credits;

	/* Protocol version specific information */
	unsigned int		i_flowctl:1;	/* enable/disable flow ctl */

	/* Batched completions */
	unsigned int		i_unsignaled_wrs;

	/* Endpoint role in connection */
	bool			i_active_side;
	atomic_t		i_cq_quiesce;

	/* Send/Recv vectors */
	int			i_scq_vector;
	int			i_rcq_vector;
	u8			i_sl;
};

/* This assumes that atomic_t is at least 32 bits */
#define IB_GET_SEND_CREDITS(v)	((v) & 0xffff)
#define IB_GET_POST_CREDITS(v)	((v) >> 16)
#define IB_SET_SEND_CREDITS(v)	((v) & 0xffff)
#define IB_SET_POST_CREDITS(v)	((v) << 16)

struct pmdfc_ib_ipaddr {
	struct list_head	list;
	__be32			ipaddr;
	struct rcu_head		rcu;
};

enum {
	PMDFC_IB_MR_8K_POOL,
	PMDFC_IB_MR_1M_POOL,
};

struct pmdfc_ib_device {
	struct list_head	list;
	struct list_head	ipaddr_list;
	struct list_head	conn_list;
	struct ib_device	*dev;
	struct ib_pd		*pd;
	bool                    use_fastreg;

	unsigned int		max_mrs;
	struct pmdfc_ib_mr_pool	*mr_1m_pool;
	struct pmdfc_ib_mr_pool   *mr_8k_pool;
	unsigned int		fmr_max_remaps;
	unsigned int		max_8k_mrs;
	unsigned int		max_1m_mrs;
	int			max_sge;
	unsigned int		max_wrs;
	unsigned int		max_initiator_depth;
	unsigned int		max_responder_resources;
	spinlock_t		spinlock;	/* protect the above */
	refcount_t		refcount;
	struct work_struct	free_work;
	int			*vector_load;
};

#define ibdev_to_node(ibdev) dev_to_node((ibdev)->dev.parent)
#define pmdfcibdev_to_node(pmdfcibdev) ibdev_to_node(pmdfcibdev->dev)

/* bits for i_ack_flags */
#define IB_ACK_IN_FLIGHT	0
#define IB_ACK_REQUESTED	1

/* Magic WR_ID for ACKs */
#define pmdfc_IB_ACK_WR_ID	(~(u64) 0)

struct pmdfc_ib_statistics {
	uint64_t	s_ib_connect_raced;
	uint64_t	s_ib_listen_closed_stale;
	uint64_t	s_ib_evt_handler_call;
	uint64_t	s_ib_tasklet_call;
	uint64_t	s_ib_tx_cq_event;
	uint64_t	s_ib_tx_ring_full;
	uint64_t	s_ib_tx_throttle;
	uint64_t	s_ib_tx_sg_mapping_failure;
	uint64_t	s_ib_tx_stalled;
	uint64_t	s_ib_tx_credit_updates;
	uint64_t	s_ib_rx_cq_event;
	uint64_t	s_ib_rx_ring_empty;
	uint64_t	s_ib_rx_refill_from_cq;
	uint64_t	s_ib_rx_refill_from_thread;
	uint64_t	s_ib_rx_alloc_limit;
	uint64_t	s_ib_rx_total_frags;
	uint64_t	s_ib_rx_total_incs;
	uint64_t	s_ib_rx_credit_updates;
	uint64_t	s_ib_ack_sent;
	uint64_t	s_ib_ack_send_failure;
	uint64_t	s_ib_ack_send_delayed;
	uint64_t	s_ib_ack_send_piggybacked;
	uint64_t	s_ib_ack_received;
	uint64_t	s_ib_rdma_mr_8k_alloc;
	uint64_t	s_ib_rdma_mr_8k_free;
	uint64_t	s_ib_rdma_mr_8k_used;
	uint64_t	s_ib_rdma_mr_8k_pool_flush;
	uint64_t	s_ib_rdma_mr_8k_pool_wait;
	uint64_t	s_ib_rdma_mr_8k_pool_depleted;
	uint64_t	s_ib_rdma_mr_1m_alloc;
	uint64_t	s_ib_rdma_mr_1m_free;
	uint64_t	s_ib_rdma_mr_1m_used;
	uint64_t	s_ib_rdma_mr_1m_pool_flush;
	uint64_t	s_ib_rdma_mr_1m_pool_wait;
	uint64_t	s_ib_rdma_mr_1m_pool_depleted;
	uint64_t	s_ib_rdma_mr_8k_reused;
	uint64_t	s_ib_rdma_mr_1m_reused;
	uint64_t	s_ib_atomic_cswp;
	uint64_t	s_ib_atomic_fadd;
	uint64_t	s_ib_recv_added_to_cache;
	uint64_t	s_ib_recv_removed_from_cache;
};

extern struct workqueue_struct *pmdfc_ib_wq;

/*
 * Fake ib_dma_sync_sg_for_{cpu,device} as long as ib_verbs.h
 * doesn't define it.
 */
static inline void pmdfc_ib_dma_sync_sg_for_cpu(struct ib_device *dev,
					      struct scatterlist *sglist,
					      unsigned int sg_dma_len,
					      int direction)
{
	struct scatterlist *sg;
	unsigned int i;

	for_each_sg(sglist, sg, sg_dma_len, i) {
		ib_dma_sync_single_for_cpu(dev, sg_dma_address(sg),
					   sg_dma_len(sg), direction);
	}
}
#define ib_dma_sync_sg_for_cpu	pmdfc_ib_dma_sync_sg_for_cpu

static inline void pmdfc_ib_dma_sync_sg_for_device(struct ib_device *dev,
						 struct scatterlist *sglist,
						 unsigned int sg_dma_len,
						 int direction)
{
	struct scatterlist *sg;
	unsigned int i;

	for_each_sg(sglist, sg, sg_dma_len, i) {
		ib_dma_sync_single_for_device(dev, sg_dma_address(sg),
					      sg_dma_len(sg), direction);
	}
}
#define ib_dma_sync_sg_for_device	pmdfc_ib_dma_sync_sg_for_device


/* ib.c */
extern struct pmdfc_transport pmdfc_ib_transport;
struct pmdfc_ib_device *pmdfc_ib_get_client_data(struct ib_device *device);
void pmdfc_ib_dev_put(struct pmdfc_ib_device *pmdfc_ibdev);
extern struct ib_client pmdfc_ib_client;

extern unsigned int pmdfc_ib_retry_count;

extern spinlock_t ib_nodev_conns_lock;
extern struct list_head ib_nodev_conns;

/* ib_cm.c */
int pmdfc_ib_conn_alloc(struct pmdfc_connection *conn, gfp_t gfp);
void pmdfc_ib_conn_free(void *arg);
int pmdfc_ib_conn_path_connect(struct pmdfc_conn_path *cp);
void pmdfc_ib_conn_path_shutdown(struct pmdfc_conn_path *cp);
void pmdfc_ib_state_change(struct sock *sk);
int pmdfc_ib_listen_init(void);
void pmdfc_ib_listen_stop(void);
__printf(2, 3)
void __pmdfc_ib_conn_error(struct pmdfc_connection *conn, const char *, ...);
int pmdfc_ib_cm_handle_connect(struct rdma_cm_id *cm_id,
			     struct rdma_cm_event *event, bool isv6);
int pmdfc_ib_cm_initiate_connect(struct rdma_cm_id *cm_id, bool isv6);
void pmdfc_ib_cm_connect_complete(struct pmdfc_connection *conn,
				struct rdma_cm_event *event);


#define pmdfc_ib_conn_error(conn, fmt...) \
	__pmdfc_ib_conn_error(conn, KERN_WARNING "pmdfc/IB: " fmt)

/* ib_rdma.c */
int pmdfc_ib_update_ipaddr(struct pmdfc_ib_device *pmdfc_ibdev,
			 struct in6_addr *ipaddr);
void pmdfc_ib_add_conn(struct pmdfc_ib_device *pmdfc_ibdev, struct pmdfc_connection *conn);
void pmdfc_ib_remove_conn(struct pmdfc_ib_device *pmdfc_ibdev, struct pmdfc_connection *conn);
void pmdfc_ib_destroy_nodev_conns(void);
void pmdfc_ib_mr_cqe_handler(struct pmdfc_ib_connection *ic, struct ib_wc *wc);

/* ib_recv.c */
int pmdfc_ib_recv_init(void);
void pmdfc_ib_recv_exit(void);
int pmdfc_ib_recv_path(struct pmdfc_conn_path *conn);
int pmdfc_ib_recv_alloc_caches(struct pmdfc_ib_connection *ic, gfp_t gfp);
void pmdfc_ib_recv_free_caches(struct pmdfc_ib_connection *ic);
void pmdfc_ib_recv_refill(struct pmdfc_connection *conn, int prefill, gfp_t gfp);
void pmdfc_ib_inc_free(struct pmdfc_incoming *inc);
int pmdfc_ib_inc_copy_to_user(struct pmdfc_incoming *inc, struct iov_iter *to);
void pmdfc_ib_recv_cqe_handler(struct pmdfc_ib_connection *ic, struct ib_wc *wc,
			     struct pmdfc_ib_ack_state *state);
void pmdfc_ib_recv_tasklet_fn(unsigned long data);
void pmdfc_ib_recv_init_ring(struct pmdfc_ib_connection *ic);
void pmdfc_ib_recv_clear_ring(struct pmdfc_ib_connection *ic);
void pmdfc_ib_recv_init_ack(struct pmdfc_ib_connection *ic);
void pmdfc_ib_attempt_ack(struct pmdfc_ib_connection *ic);
void pmdfc_ib_ack_send_complete(struct pmdfc_ib_connection *ic);
u64 pmdfc_ib_piggyb_ack(struct pmdfc_ib_connection *ic);
void pmdfc_ib_set_ack(struct pmdfc_ib_connection *ic, u64 seq, int ack_required);

/* ib_ring.c */
void pmdfc_ib_ring_init(struct pmdfc_ib_work_ring *ring, u32 nr);
void pmdfc_ib_ring_resize(struct pmdfc_ib_work_ring *ring, u32 nr);
u32 pmdfc_ib_ring_alloc(struct pmdfc_ib_work_ring *ring, u32 val, u32 *pos);
void pmdfc_ib_ring_free(struct pmdfc_ib_work_ring *ring, u32 val);
void pmdfc_ib_ring_unalloc(struct pmdfc_ib_work_ring *ring, u32 val);
int pmdfc_ib_ring_empty(struct pmdfc_ib_work_ring *ring);
int pmdfc_ib_ring_low(struct pmdfc_ib_work_ring *ring);
u32 pmdfc_ib_ring_oldest(struct pmdfc_ib_work_ring *ring);
u32 pmdfc_ib_ring_completed(struct pmdfc_ib_work_ring *ring, u32 wr_id, u32 oldest);
extern wait_queue_head_t pmdfc_ib_ring_empty_wait;

/* ib_send.c */
void pmdfc_ib_xmit_path_complete(struct pmdfc_conn_path *cp);
int pmdfc_ib_xmit(struct pmdfc_connection *conn, struct pmdfc_message *rm,
		unsigned int hdr_off, unsigned int sg, unsigned int off);
void pmdfc_ib_send_cqe_handler(struct pmdfc_ib_connection *ic, struct ib_wc *wc);
void pmdfc_ib_send_init_ring(struct pmdfc_ib_connection *ic);
void pmdfc_ib_send_clear_ring(struct pmdfc_ib_connection *ic);
int pmdfc_ib_xmit_rdma(struct pmdfc_connection *conn, struct rm_rdma_op *op);
void pmdfc_ib_send_add_credits(struct pmdfc_connection *conn, unsigned int credits);
void pmdfc_ib_advertise_credits(struct pmdfc_connection *conn, unsigned int posted);
int pmdfc_ib_send_grab_credits(struct pmdfc_ib_connection *ic, u32 wanted,
			     u32 *adv_credits, int need_posted, int max_posted);
int pmdfc_ib_xmit_atomic(struct pmdfc_connection *conn, struct rm_atomic_op *op);

/* ib_stats.c */
DECLARE_PER_CPU_SHARED_ALIGNED(struct pmdfc_ib_statistics, pmdfc_ib_stats);
#define pmdfc_ib_stats_inc(member) pmdfc_stats_inc_which(pmdfc_ib_stats, member)
#define pmdfc_ib_stats_add(member, count) \
		pmdfc_stats_add_which(pmdfc_ib_stats, member, count)
unsigned int pmdfc_ib_stats_info_copy(struct pmdfc_info_iterator *iter,
				    unsigned int avail);

/* ib_sysctl.c */
int pmdfc_ib_sysctl_init(void);
void pmdfc_ib_sysctl_exit(void);
extern unsigned long pmdfc_ib_sysctl_max_send_wr;
extern unsigned long pmdfc_ib_sysctl_max_recv_wr;
extern unsigned long pmdfc_ib_sysctl_max_unsig_wrs;
extern unsigned long pmdfc_ib_sysctl_max_unsig_bytes;
extern unsigned long pmdfc_ib_sysctl_max_recv_allocation;
extern unsigned int pmdfc_ib_sysctl_flow_control;

#endif
