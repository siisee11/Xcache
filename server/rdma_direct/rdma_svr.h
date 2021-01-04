#ifndef _RDMA_DIRECT_SVR_H_
#define _RDMA_DIRECT_SVR_H_

#define TEST_NZ(x) do { if ( (x)) die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) die("error: " #x " failed (returned zero/null)."); } while (0)

const size_t BUFFER_SIZE = ((1UL << 30) * 16);
//const unsigned int NUM_PROCS = 8;
//const unsigned int NUM_QUEUES_PER_PROC = 3;
//const unsigned int NUM_QUEUES = NUM_PROCS * NUM_QUEUES_PER_PROC;
const unsigned int NUM_QUEUES = 2;

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

struct ctrl {
  struct queue *queues;
  struct ibv_mr *mr_buffer;
  void *buffer;
  struct device *dev;

  struct ibv_comp_channel *comp_channel;
};

struct memregion {
  uint64_t baseaddr;
  uint32_t key;
  uint64_t mr_size;
};

static void die(const char *reason);

static int alloc_control();
static int on_connect_request(struct rdma_cm_id *id, struct rdma_conn_param *param);
static int on_connection(struct queue *q);
static int on_disconnect(struct queue *q);
static int on_event(struct rdma_cm_event *event);
static void destroy_device(struct ctrl *ctrl);

#endif // _RDMA_DIRECT_SVR_H_
