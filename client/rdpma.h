#ifndef _RDPMA_H_ 
#define _RDPMA_H_

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <linux/inet.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/gfp.h>
#include <linux/pagemap.h>
#include <linux/spinlock.h>

#define NUM_LOCKS 			(10)
#define BATCH_SIZE 			(4)

#define NUM_QUEUES 			(8) 			/* 4 CPU * 2 */
#define MAX_BATCH 			(1) 			/* 16 get fault */
#define NUM_ENTRY 			(4) 			/* # of Metadata per queue */
#define METADATA_SIZE 		(8 * BATCH_SIZE) 	 		/* [ key * 4 ] */ 

#define ENTRY_SIZE 						(METADATA_SIZE + PAGE_SIZE * MAX_BATCH) 	/* [meta, page] */
#define LOCAL_META_REGION_SIZE  		(NUM_QUEUES * NUM_ENTRY * ENTRY_SIZE)

#define GET_LOCAL_META_REGION(addr, qid, mid)(addr + NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid)
#define GET_BATCH_SIZE(addr, qid, mid) 	(addr + NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid + 8)
#define GET_REMOTE_ADDRESS_BASE(addr, qid, mid) 	(addr + NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid + 16)
#define GET_LOCAL_PAGE_REGION(addr, qid, mid) 	(addr + NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid + METADATA_SIZE)
#define GET_OFFSET_FROM_BASE(qid, mid) 				(NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid)
#define GET_OFFSET_FROM_BASE_TO_ADDR(qid, mid) 		(NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid + 16)
#define GET_OFFSET_FROM_BASE_TO_PAGE(qid, mid) 		(NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid + METADATA_SIZE)


enum qp_type {
	QP_READ_SYNC,
	QP_READ_ASYNC,
	QP_WRITE_SYNC,
	QP_TEST_SYNC
};

struct rdpma_metadata {
	uint64_t key;
	uint64_t batch;
	uint64_t raddr;
};

struct pmdfc_rdma_dev {
	struct ib_device *dev;
	struct ib_pd *pd;

	/* XXX */
	struct ib_mr* mr;
	uint64_t mr_size;
	uint64_t local_mm;
	uintptr_t local_dma_addr;
};

struct rdma_req {
	struct completion done;
	struct list_head list;
	struct ib_cqe cqe;
	int mid;
	u64 dma;
	struct page *page;
};

struct pmdfc_rdma_ctrl;

struct combined_buffer {
	char buffer[PAGE_SIZE * 4];
	uint64_t keys[4];
};

struct rdma_queue {
	struct ib_qp *qp;
	struct ib_cq *send_cq;
	struct ib_cq *recv_cq;
	spinlock_t cq_lock;
	enum qp_type qp_type;

	struct pmdfc_rdma_ctrl *ctrl;

	struct rdma_cm_id *cm_id;
	int cm_error;
	struct completion cm_done;

	atomic_t pending;

	/* XXX*/
	int success;
	struct page *page;
	struct combined_buffer *cbuffer;
	char buffer[PAGE_SIZE * BATCH_SIZE];   /* batching 4 pages */
	uint64_t keys[BATCH_SIZE];   /* batching 4 keys*/
	atomic_t nr_buffered;
	struct idr 		queue_status_idr;
	spinlock_t		queue_lock[NUM_LOCKS];  /* fine grained lock */
	spinlock_t		global_lock;
};

struct pmdfc_rdma_memregion {
	u64 baseaddr;
	u32 key;
	u64 mr_size;
};

struct pmdfc_rdma_ctrl {
	struct pmdfc_rdma_dev *rdev; // TODO: move this to queue
	struct rdma_queue *queues;
	struct pmdfc_rdma_memregion servermr;
	/* XXX */
	struct pmdfc_rdma_memregion clientmr;

	union {
		struct sockaddr addr;
		struct sockaddr_in addr_in;
	};

	union {
		struct sockaddr srcaddr;
		struct sockaddr_in srcaddr_in;
	};
};

struct rdma_queue *rdpma_get_queue(unsigned int idx, enum qp_type type);
int rdpma_get_queue_id(unsigned int idx, enum qp_type type);

int rdpma_get(struct page *page, uint64_t, int);
int rdpma_get_onesided(struct page *page, uint64_t, int);
int rdpma_put(struct page *page, uint64_t, int);
int rdpma_put_onesided(struct page *page, uint64_t, int);
int pmdfc_rdma_poll_load(int cpu);
void pmdfc_rdma_print_stat(void);
enum qp_type get_queue_type(unsigned int idx);

#endif // _RDPMA_H_
