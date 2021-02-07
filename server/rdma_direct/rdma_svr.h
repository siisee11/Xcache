#ifndef _RDMA_DIRECT_SVR_H_
#define _RDMA_DIRECT_SVR_H_

#ifdef APP_DIRECT
#include <libpmemobj.h>

POBJ_LAYOUT_BEGIN(PM_MR);
POBJ_LAYOUT_TOID(PM_MR, void);
POBJ_LAYOUT_END(PM_MR);
#endif

#include "NuMA_KV.h"

#define PAGE_SIZE 	4096

#define NUM_CLIENT 		16
#define NUM_QUEUES 		2
#define NUM_ENTRY 		32
#define METADATA_SIZE 	16

#define ENTRY_SIZE 						(METADATA_SIZE + PAGE_SIZE)
#define CLIENT_META_REGION_SIZE			(NUM_QUEUES * NUM_ENTRY * ENTRY_SIZE)
#define LOCAL_META_REGION_SIZE			(NUM_CLIENT * CLIENT_META_REGION_SIZE)

#define GET_CLIENT_BASE(addr, nid) 		(addr + nid * CLIENT_META_REGION_SIZE)
#define GET_LOCAL_META_REGION(addr, qid, mid)		(addr + NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid)
#define GET_REMOTE_ADDRESS_BASE(addr, qid, mid) 	(addr + NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid + 8)
#define GET_LOCAL_PAGE_REGION(addr, qid, mid) 	(addr + NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid + METADATA_SIZE)
#define GET_OFFSET_FROM_BASE(qid, mid) 		(NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid)
#define GET_OFFSET_FROM_BASE_TO_ADDR(qid, mid) 		(NUM_ENTRY * ENTRY_SIZE * qid + ENTRY_SIZE * mid + 8)

#define TEST_NZ(x) do { if ( (x)) die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) die("error: " #x " failed (returned zero/null)."); } while (0)

const size_t BUFFER_SIZE = ((1UL << 30) * 16);

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

enum qp_type {
	QP_READ_SYNC,
	QP_WRITE_SYNC,
};

struct device {
	struct ibv_pd *pd;
	struct ibv_context *verbs;
};

struct queue {
	struct ibv_qp *qp;
	struct ibv_cq *cq;
	struct rdma_cm_id *cm_id;
	struct ctrl *ctrl;
	enum {
		INIT,
		CONNECTED
	} state;
};

struct memregion {
	uint64_t baseaddr;
	uint32_t key;
	uint64_t mr_size;
};

struct ctrl {
	struct queue *queues;
	struct ibv_mr *mr_buffer;
	void *buffer;
	struct device *dev;

	/* XXX */
	uint64_t local_mm;
	struct memregion clientmr;
	struct memregion servermr;

	NUMA_KV* kv;

	struct ibv_comp_channel *comp_channel;
};


static void die(const char *reason);

static int alloc_control();
static int on_connect_request(struct rdma_cm_id *id, struct rdma_conn_param *param);
static int on_connection(struct queue *q);
static int on_disconnect(struct queue *q);
static int on_event(struct rdma_cm_event *event);
static void destroy_device(struct ctrl *ctrl);

#endif // _RDMA_DIRECT_SVR_H_
