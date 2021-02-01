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

#define NUM_QUEUES 			(40)
#define NUM_ENTRY			(32) 			/* # of Metadata per queue */
#define METADATA_SIZE		(16) 	 		/* [ key, remote address ] */ 
#define BITMAP_SIZE	(64)

#define ENTRY_SIZE 						(METADATA_SIZE + PAGE_SIZE) 	/* [meta, page] */
#define LOCAL_META_REGION_SIZE			(NUM_QUEUES * NUM_ENTRY * ENTRY_SIZE)

#define GET_LOCAL_META_REGION(addr, qid, mid)		(addr + NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid)
#define GET_REMOTE_ADDRESS_BASE(addr, qid, mid) 	(addr + NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid + 8)
#define GET_LOCAL_PAGE_REGION(addr, qid, mid) 	(addr + NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid + METADATA_SIZE)
#define GET_OFFSET_FROM_BASE(qid, mid) 		(NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid)
#define GET_OFFSET_FROM_BASE_TO_ADDR(qid, mid) 		(NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid + 8)
#define GET_PAGE_OFFSET_FROM_BASE(qid, mid) 		(NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid + METADATA_SIZE)

enum qp_type {
	QP_READ_SYNC,
	QP_READ_ASYNC,
	QP_WRITE_SYNC,
	QP_TEST_SYNC
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
	u64 dma;
	struct page *page;
};

struct pmdfc_rdma_ctrl;

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
	struct idr 		queue_status_idr;
	spinlock_t		queue_lock;
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

struct rdma_queue *pmdfc_rdma_get_queue(unsigned int idx, enum qp_type type);
int pmdfc_rdma_get_queue_id(unsigned int idx, enum qp_type type);

int rdpma_get(struct page *page, uint64_t);
int rdpma_put(struct page *page, uint64_t);
int pmdfc_rdma_poll_load(int cpu);
void pmdfc_rdma_print_stat(void);
enum qp_type get_queue_type(unsigned int idx);

#endif // _RDPMA_H_
