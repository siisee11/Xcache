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

#define LOCAL_META_REGION_SIZE			(MAX_NODE * MAX_PROCESS * NUM_ENTRY * METADATA_SIZE)
#define PER_NODE_META_REGION_SIZE		(MAX_PROCESS * NUM_ENTRY * METADATA_SIZE)
#define PER_PROCESS_META_REGION_SIZE	(NUM_ENTRY * METADATA_SIZE)
#define GET_CLIENT_META_REGION(addr, nid, pid)	(addr + nid * PER_NODE_META_REGION_SIZE + PER_PROCESS_META_REGION_SIZE * pid)

#define TEST_NZ(x) do { if ( (x)) die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) die("error: " #x " failed (returned zero/null)."); } while (0)

const size_t BUFFER_SIZE = ((1UL << 30) * 16);
//const unsigned int NUM_PROCS = 8;
//const unsigned int NUM_QUEUES_PER_PROC = 3;
//const unsigned int NUM_QUEUES = NUM_PROCS * NUM_QUEUES_PER_PROC;
//const unsigned int NUM_QUEUES = 2;
const unsigned int NUM_QUEUES = 40; /* XXX */

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

	/* XXX */
	void *page; 		/* 4096 byte */
	uint64_t key; 	/* 64 byte */
	void *empty_page; 
};

struct memregion {
	uint64_t baseaddr;
	uint32_t key;
	uint64_t mr_size;
};

struct ctrl {
	struct queue *queues;
	struct ibv_mr *mr_buffer;
#ifdef APP_DIRECT
	PMEMobjpool* log_pop[NUM_NUMA];
	PMEMobjpool* pop[NUM_NUMA];
	TOID(void) p_mr;
	int fd;
#endif
	void *buffer;
	struct device *dev;

	/* XXX */
	struct memregion clientmr;

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
