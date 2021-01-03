#ifndef _RDMA_OP_H_
#define _RDMA_OP_H_

#include "rdma_conn.h"
/*
static inline int begin_read(struct rdma_queue *q, struct page *page, u64 roffset);
static int drain_queue(struct rdma_queue *q);
static inline int get_req_for_page(struct rdma_req **req, struct ib_device *dev, 
        struct page *page, enum dma_data_direction dir);
*/

//struct rdma_queue *pmdfc_rdma_get_queue(unsigned int cpuid, enum qp_type type);
int pmdfc_rdma_poll_load(int cpu);
//static inline int pmdfc_rdma_post_rdma(struct rdma_queue *q, struct rdma_req *qe,
//        struct ib_sge *sge, u64 roffset, enum ib_wr_opcode op);

int pmdfc_rdma_read_async(struct page *page, u64 roffset);
//static void pmdfc_rdma_read_done(struct ib_cq *cq, struct ib_wc *wc);
int pmdfc_rdma_read_sync(struct page *page, u64 roffset);

int pmdfc_rdma_write(struct page *page, u64 roffset);
//static void pmdfc_rdma_write_done(struct ib_cq *cq, struct ib_wc *wc);

//static inline int poll_target(struct rdma_queue *q, int target);
//static inline int write_queue_add(struct rdma_queue *q, struct page *page, u64 roffset);

#endif // _RDMA_OP_H_
