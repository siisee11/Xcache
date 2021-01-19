#ifndef _RDMA_CONN_H_
#define _RDMA_CONN_H_

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <linux/inet.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/gfp.h>
#include <linux/pagemap.h>
#include <linux/spinlock.h>

#define NUM_ENTRY			(1)
#define METADATA_SIZE		(16) 	 		/* [ key, remote address ] */ 
#define MAX_PROCESS			(4096) 			/* same as PMDFC_STORAGE_SIZE */
#define BITMAP_SIZE	(64)

#define LOCAL_META_REGION_SIZE		(MAX_PROCESS * NUM_ENTRY * METADATA_SIZE)

#define GET_LOCAL_META_REGION(addr, id)	 (addr + NUM_ENTRY * METADATA_SIZE * id)
#define GET_REMOTE_ADDRESS_BASE(addr, id) (addr + NUM_ENTRY * METADATA_SIZE * id + 8)


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
	struct ib_cq *cq;
	spinlock_t cq_lock;
	enum qp_type qp_type;

	struct pmdfc_rdma_ctrl *ctrl;

	struct rdma_cm_id *cm_id;
	int cm_error;
	struct completion cm_done;

	atomic_t pending;
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
enum qp_type get_queue_type(unsigned int idx);

#endif // _RDMA_CONN_H_
