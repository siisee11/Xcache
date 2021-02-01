#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <thread>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <numa.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <getopt.h>


#include "rdma_svr.h"
#include "variables.h"


/* option values */
int tcp_port = -1;
int ib_port = 1;
char *path;
char *data_path;
char *pm_path;
size_t initialTableSize = 32*1024;
size_t numData = 0;
size_t numKVThreads = 0;
size_t numNetworkThreads = 0;
size_t numPollThreads = 0;
bool numa_on = false;
bool verbose_flag = false;
bool human = false;
struct bitmask *netcpubuf;
struct bitmask *kvcpubuf;
struct bitmask *pollcpubuf;

/*  Global values */
static struct ctrl *gctrl = NULL;
static unsigned int queue_ctr = 0;
unsigned int nr_cpus;
std::atomic<bool> done(false);

/* counting values */
int putcnt = 0;
int getcnt = 0;
int found_cnt = 0;
int notfound_cnt = 0;


/* performance timer */
uint64_t rdpma_handle_write_elapsed=0;
uint64_t rdpma_handle_write_poll_elapsed=0;
uint64_t rdpma_handle_read_elapsed=0;
uint64_t rdpma_handle_read_poll_elapsed=0;
uint64_t rdpma_handle_read_poll_notfound_elapsed=0;
uint64_t rdpma_handle_read_poll_found_elapsed=0;

#ifdef APP_DIRECT
static char pm_path[32] = "/mnt/pmem0/pmdfc/pm_mr";
#endif

static void dprintf( const char* format, ... ) {
	if (verbose_flag) {
		va_list args;
		va_start( args, format );
		vprintf( format, args );
		va_end( args );
	}
}

static uint32_t bit_mask(int num, int msg_num, int type, int state, int qid){
	uint32_t target = (((uint32_t)num << 28) | ((uint32_t)msg_num << 16) | ((uint32_t)type << 12) | ((uint32_t)state << 8) | ((uint32_t)qid & 0x000000ff));
	return target;
}

static void bit_unmask(uint32_t target, int* num, int* msg_num, int* type, int* state, int* qid){
	*qid= (uint32_t)(target & 0x000000ff);
	*state = (int)((target >> 8) & 0x0000000f);
	*type = (int)((target >> 12) & 0x0000000f);
	*msg_num = (int)((target >> 16) & 0x00000fff);
	*num= (int)((target >> 28) & 0x0000000f);
}


inline enum qp_type get_queue_type(unsigned int idx)
{
	if (idx < NUM_QUEUES / 2)
		return QP_READ_SYNC;
	else if (idx >= NUM_QUEUES /2)
		return QP_WRITE_SYNC;

	return QP_READ_SYNC;
}

static void rdpma_print_stats() {
	printf("\n--------------------REPORT---------------------\n");
//	printf("SAMPLE RATE [1/%d]\n", SAMPLE_RATE);
	printf("# of puts : %d , # of gets : %d ( %d / %d )\n",
			putcnt, getcnt, found_cnt, notfound_cnt);

	if (putcnt == 0) putcnt++;
	if (getcnt == 0) getcnt++;
	if (found_cnt == 0) found_cnt++;
	if (notfound_cnt == 0) notfound_cnt++;

	printf("\n--------------------SUMMARY--------------------\n");
	printf("Average (divided by number of ops)\n");
	printf("Write: %.3f (us), Read: %.3f (us)\n",
			rdpma_handle_write_elapsed/putcnt/1000.0,
			rdpma_handle_read_elapsed/getcnt/1000.0);
	printf("(Poll) Write: %.3f (us), Read: %.3f [%.3f / %.3f](us)\n",
			rdpma_handle_write_poll_elapsed/putcnt/1000.0,
			(rdpma_handle_read_poll_found_elapsed + rdpma_handle_read_poll_notfound_elapsed)/getcnt/1000.0,
			rdpma_handle_read_poll_found_elapsed/found_cnt/1000.0,
			rdpma_handle_read_poll_notfound_elapsed/notfound_cnt/1000.0);

	gctrl->kv->PrintStats();

	printf("--------------------FIN------------------------\n");
}

/**
 * indicator - Show stats periodically
 */
void rdpma_indicator() {
	while (!done) {
		sleep(10);
		rdpma_print_stats();
	}
}


/* FROM RDMA_SERVER.CPP */
int post_recv(int queue_id){
	struct ibv_recv_wr wr;
	struct ibv_recv_wr* bad_wr;
	struct ibv_sge sge;

	memset(&wr, 0, sizeof(struct ibv_recv_wr));
	memset(&sge, 0, sizeof(struct ibv_sge));

	sge.addr = 0;
	sge.length = 0;
	sge.lkey = gctrl->mr_buffer->lkey;

	wr.wr_id = 0;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.next = NULL;

	if(ibv_post_recv(gctrl->queues[queue_id].qp, &wr, &bad_wr)){
		fprintf(stderr, "[%s] ibv_post_recv failed\n", __func__);
		return 1;
	}
	return 0;
}

/* post recv [ page, key ] */
int pmdfc_rdma_post_recv(int queue_id){
	struct ibv_recv_wr wr;
	struct ibv_recv_wr* bad_wr;
	struct ibv_sge sge[2];

	memset(&wr, 0, sizeof(struct ibv_recv_wr));
	memset(&sge, 0, sizeof(struct ibv_sge) * 2);

	sge[0].addr = (uint64_t)gctrl->queues[queue_id].page;
	sge[0].length = PAGE_SIZE;
	sge[0].lkey = gctrl->mr_buffer->lkey;

	sge[1].addr = (uint64_t)&gctrl->queues[queue_id].key;
	sge[1].length = sizeof(uint64_t);
	sge[1].lkey = gctrl->mr_buffer->lkey;

	wr.wr_id = 0;
	wr.sg_list = &sge[0];
	wr.num_sge = 2;
	wr.next = NULL;

	if(ibv_post_recv(gctrl->queues[queue_id].qp, &wr, &bad_wr)){
		fprintf(stderr, "[%s] ibv_post_recv to node %d failed\n", __func__, queue_id);
		return 1;
	}
	return 0;
}

static void server_recv_poll_cq(struct queue *q, int queue_id) {
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
//	int cpu = sched_getcpu();
//	int thisNode = numa_node_of_cpu(cpu);
	struct ibv_wc wc;
	int ne;
	struct ibv_wc wc2;
#if defined(TIME_CHECK)
	struct timespec start, end;
#endif

	while(1) {
		ne = 0;
		do{
			ne += ibv_poll_cq(q->qp->recv_cq, 1, &wc);
			if(ne < 0){
				fprintf(stderr, "ibv_poll_cq failed %d\n", ne);
				die("ibv_poll_cq failed");
			}
		}while(ne < 1);

		if(wc.status != IBV_WC_SUCCESS){
			fprintf(stderr, "Failed status %s (%d)\n", ibv_wc_status_str(wc.status), wc.status);
			die("Failed status");
		}

		int ret;
		if((int)wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM){
			int qid, msg_id, type, tx_state, num;
			struct ibv_send_wr wr = {};
			struct ibv_send_wr *bad_wr = NULL;
			struct ibv_sge sge = {};

			bit_unmask(ntohl(wc.imm_data), &num, &msg_id, &type, &tx_state, &qid);
			dprintf("[ INFO ] On Q[%d]: num(%d), msg_id(%d), type(%d), tx_state(%d), qid(%d)\n", queue_id, qid, msg_id, type, tx_state, num);

			post_recv(queue_id);

			if(type == MSG_WRITE){
				putcnt++;

				uint64_t* key = (uint64_t*)GET_LOCAL_META_REGION(gctrl->local_mm, qid, msg_id);
				uint64_t page = (uint64_t)GET_LOCAL_PAGE_REGION(gctrl->local_mm, qid, msg_id);
				void *save_page = malloc(PAGE_SIZE);
				memcpy((char *)save_page, (char *)page, PAGE_SIZE);

#if defined(TIME_CHECK)
				clock_gettime(CLOCK_MONOTONIC, &start);
#endif
				gctrl->kv->Insert(*key, (Value_t)save_page, 0, 0);
				dprintf("[ INFO ] MSG_WRITE page %lx, key %lx Inserted\n", (uint64_t)page, *key);
				dprintf("[ INFO ] page %s\n", (char *)save_page);

#if defined(TIME_CHECK)
				clock_gettime(CLOCK_MONOTONIC, &end);
				rdpma_handle_write_elapsed+= end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);
#endif

#if 1 /* if this block is commented, Client polling get slow down */
				sge.addr = 0;
				sge.length = 0;
				sge.lkey = gctrl->mr_buffer->lkey;

				wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM; /* IBV_WR_SEND_WITH_IMM same */
				wr.sg_list = &sge;
				wr.num_sge = 0;
				wr.send_flags = IBV_SEND_SIGNALED;
				wr.imm_data = htonl(bit_mask(0, msg_id, MSG_READ_REPLY, TX_WRITE_COMMITTED, qid));

				ret = ibv_post_send(q->qp, &wr, &bad_wr);
				if(ret){
					fprintf(stderr, "[%s] ibv_post_send to node failed with %d\n", __func__, ret);
				}

				struct ibv_wc wc2;
				int ne;
				do{
					ne = ibv_poll_cq(q->qp->send_cq, 1, &wc2);
					if(ne < 0){
						fprintf(stderr, "[%s] ibv_poll_cq failed\n", __func__);
						return;
					}
				}while(ne < 1);

				if(wc2.status != IBV_WC_SUCCESS){
					fprintf(stderr, "[%s] sending rdma_write failed status %s (%d)\n", __func__, ibv_wc_status_str(wc2.status), wc2.status);
					return;
				}
#if defined(TIME_CHECK)
				clock_gettime(CLOCK_MONOTONIC, &start);
				rdpma_handle_write_poll_elapsed += start.tv_nsec - end.tv_nsec + 1000000000 * (start.tv_sec - end.tv_sec);
#endif
#endif 
			} else if(type == MSG_READ) {
				getcnt++;
#if defined(TIME_CHECK)
				clock_gettime(CLOCK_MONOTONIC, &start);
#endif
//				printf("[ INFO ] received MSG_READ\n");

				uint64_t* key = (uint64_t*)GET_LOCAL_META_REGION(gctrl->local_mm, qid, msg_id);
				uint64_t* remote_addr = (uint64_t*)GET_REMOTE_ADDRESS_BASE(gctrl->local_mm, qid, msg_id);
				uint64_t target_addr = (uint64_t)GET_REMOTE_ADDRESS_BASE(gctrl->local_mm, qid, msg_id);
				dprintf("[ INFO ] key= %lx, remote address= %lx\n", *key, *remote_addr);

				/* 1. Get page address -> value */
				void* value;
				bool abort = false;

				value = (void *)gctrl->kv->Get((Key_t&)*key, 0); 
				if(!value){
					dprintf("Value for key[%lx] not found\n", key);
					abort = true;
				}

#if defined(TIME_CHECK)
				clock_gettime(CLOCK_MONOTONIC, &end);
				rdpma_handle_read_elapsed += end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);
#endif

				if( !abort ) {
					found_cnt++;
					dprintf("[ INFO ] page %lx, key %lx Searched\n", (uint64_t)value, *key);
					dprintf("[ INFO ] page %s\n", (char *)value);

					/* 2. Send page retrieved to client so that client can initiate RDMA READ */	
					memcpy((char *)target_addr, (char *)value, PAGE_SIZE);
					dprintf("[ INFO ] page %s\n", (char *)target_addr);

					wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
					wr.sg_list = &sge;
					wr.num_sge = 0;
					wr.send_flags = IBV_SEND_SIGNALED;
					wr.imm_data = htonl(bit_mask(0, msg_id, MSG_READ_REPLY, TX_READ_COMMITTED, qid));

					TEST_NZ(ibv_post_send(q->qp, &wr, &bad_wr));

				} else {
					printf("[ INFO ] aborted\n");
					notfound_cnt++;

					wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
					wr.sg_list = &sge;
					wr.num_sge = 0;
					wr.send_flags = IBV_SEND_SIGNALED;
					wr.imm_data = htonl(bit_mask(0, msg_id, MSG_READ_REPLY, TX_READ_ABORTED, qid));

					TEST_NZ(ibv_post_send(q->qp, &wr, &bad_wr));
				}

				do{
					ne = ibv_poll_cq(q->qp->send_cq, 1, &wc2);
					if(ne < 0){
						fprintf(stderr, "[%s] ibv_poll_cq failed\n", __func__);
						return;
					}
				}while(ne < 1);

				if(wc2.status != IBV_WC_SUCCESS){
					fprintf(stderr, "[%s] sending rdma_write failed status %s (%d)\n", __func__, ibv_wc_status_str(wc2.status), wc2.status);
					return;
				}

#if defined(TIME_CHECK)
				clock_gettime(CLOCK_MONOTONIC, &start);
				if (abort) {
					rdpma_handle_read_poll_notfound_elapsed += start.tv_nsec - end.tv_nsec + 1000000000 * (start.tv_sec - end.tv_sec);
				} else {
					rdpma_handle_read_poll_found_elapsed += start.tv_nsec - end.tv_nsec + 1000000000 * (start.tv_sec - end.tv_sec);
				}
#endif
			}
		}
		else if((int)wc.opcode == IBV_WC_RDMA_READ){
			dprintf("[%s]: received WC_RDMA_READ\n", __func__);
			/* the client is reading data from read region*/
		}
		else if ( (int)wc.opcode == IBV_WC_RECV ){
			/* only for first connection */
			dprintf("[ INFO ] connected. receiving memory region info.\n");
			printf("[ INFO ] *** Client MR key=%u base vaddr=%p size=%lu ***\n", gctrl->clientmr.key, (void *)gctrl->clientmr.baseaddr
					, gctrl->clientmr.mr_size/1024);
		}else{
			fprintf(stderr, "Received a weired opcode (%d)\n", (int)wc.opcode);
		}
	}
}


static device *get_device(struct queue *q)
{
	struct device *dev = NULL;

	if (!q->ctrl->dev) {
		dev = (struct device *) malloc(sizeof(*dev));
		TEST_Z(dev);
		dev->verbs = q->cm_id->verbs;
		TEST_Z(dev->verbs);
		dev->pd = ibv_alloc_pd(dev->verbs);
		TEST_Z(dev->pd);

		struct ctrl *ctrl = q->ctrl;    
#if APP_DIRECT
		/*
		   if (access(pm_path, 0)) {
		   ctrl->pop = pmemobj_create(pm_path, POBJ_LAYOUT_NAME(PM_MR), BUFFER_SIZE, 0666);
		   if(!ctrl->pop){
		   perror("pmemobj_create");
		   exit(0);
		   }
		   } else {
		   ctrl->pop = pmemobj_open(pm_path, POBJ_LAYOUT_NAME(PM_MR));
		   if(!ctrl->pop){
		   perror("pmemobj_open");
		   exit(0);
		   }
		   }
		   printf("[  OK  ] PM initialized\n");
		   POBJ_ALLOC(ctrl->pop, &ctrl->p_mr, void, BUFFER_SIZE, NULL, NULL);
		   ctrl->buffer = (void *)&ctrl->p_mr;
		   */
		struct stat sb;
		int flag = PROT_WRITE | PROT_READ | PROT_EXEC;

		if ((ctrl->fd = open(pm_path, O_RDWR|O_CREAT, 645)) < 0) {
			perror("File Open Error");
			exit(1);
		}

		if (fstat(ctrl->fd, &sb) < 0) {
			perror("fstat error");
			exit(1);
		}
		ctrl->buffer = mmap(0, BUFFER_SIZE, flag, MAP_SHARED, ctrl->fd, 0); //XXX
		//memset(ctrl->buffer, 0x00, BUFFER_SIZE);

		if (ctrl->buffer == MAP_FAILED) {
			perror("mmap error");
			exit(1);
		}

		TEST_Z(ctrl->mr_buffer = ibv_reg_mr( dev->pd, ctrl->buffer, BUFFER_SIZE,
					IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ)); // XXX
		printf("[  OK  ] PM MR initialized\n");
#elif DAX_KMEM
		int dax_kmem_node = 3;
		ctrl->buffer = numa_alloc_onnode(BUFFER_SIZE, dax_kmem_node); 
		TEST_Z(ctrl->buffer);
		printf("[  OK  ] DAX KMEM MR initialized\n");
#else
		/* 
		 * To create an implicit ODP MR, IBV_ACCESS_ON_DEMAND should be set, 
		 * addr should be 0 and length should be SIZE_MAX.
		 */
		TEST_Z(ctrl->mr_buffer = ibv_reg_mr( dev->pd, NULL, (uint64_t)-1, \
					IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_ON_DEMAND));

		ctrl->local_mm = (uint64_t)malloc(LOCAL_META_REGION_SIZE);

		dprintf("[ INFO ] registered memory region of %zu KB\n", LOCAL_META_REGION_SIZE/1024);
		dprintf("[ INFO ] registered memory region key=%u base vaddr=%lx\n", ctrl->mr_buffer->rkey, ctrl->local_mm);
		printf("[  OK  ] DRAM MR initialized\n");
		q->ctrl->dev = dev;
#endif
	}

	return q->ctrl->dev;
}

static void destroy_device(struct ctrl *ctrl)
{
	TEST_Z(ctrl->dev);

	ibv_dereg_mr(ctrl->mr_buffer);
#ifdef APP_DIRECT
	munmap(ctrl->buffer, BUFFER_SIZE);
	close(ctrl->fd);
#endif
	free((void*)ctrl->local_mm);
	ibv_dealloc_pd(ctrl->dev->pd);
	free(ctrl->dev);
	ctrl->dev = NULL;
}

static void create_qp(struct queue *q)
{
	struct ibv_qp_init_attr qp_attr = {};
	
    struct ibv_cq* recv_cq = ibv_create_cq(q->cm_id->verbs, 256, NULL, NULL, 0);
	if(!recv_cq){
		fprintf(stderr, "ibv_create_cq for recv_cq failed\n");
	}

	struct ibv_cq* send_cq = ibv_create_cq(q->cm_id->verbs, 256, NULL, NULL, 0);
	if(!send_cq){
		fprintf(stderr, "ibv_create_cq for send_cq failed\n");
	}

	/* XXX */
	qp_attr.send_cq = recv_cq;
	qp_attr.recv_cq = send_cq;
	qp_attr.qp_type = IBV_QPT_RC; /* XXX */ 
	qp_attr.cap.max_send_wr = 10;
	qp_attr.cap.max_recv_wr = 10;
	qp_attr.cap.max_send_sge = 2; /* XXX */
	qp_attr.cap.max_recv_sge = 2; /* XXX */

	TEST_NZ(rdma_create_qp(q->cm_id, q->ctrl->dev->pd, &qp_attr));
	q->qp = q->cm_id->qp;
}

int on_connect_request(struct rdma_cm_id *id, struct rdma_conn_param *param)
{

	struct rdma_conn_param cm_params = {};
	struct ibv_device_attr attrs = {};
	int queue_number = queue_ctr++;
	struct queue *q = &gctrl->queues[queue_number];

	TEST_Z(q->state == queue::INIT);
//	printf("[ INFO ] %s\n", __FUNCTION__);

	id->context = q;
	q->cm_id = id;

	struct device *dev = get_device(q);
	create_qp(q);

	/* XXX : Poller start here */
	/* Create polling thread associated with q */
	std::thread p = std::thread( server_recv_poll_cq, q, queue_number);

	// Create a cpu_set_t object representing a set of CPUs. Clear it and mark
	// only CPU i as set.
	// threads[i] would be assigned to CPU i
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(queue_number, &cpuset);
	int rc = pthread_setaffinity_np(p.native_handle(),
			sizeof(cpu_set_t), &cpuset);
	if (rc != 0) {
		fprintf(stderr, "Error calling pthread_setaffinity_np\n");
	}

	p.detach();


	TEST_NZ(ibv_query_device(dev->verbs, &attrs));

#if 0
	if (queue_number == 0 ) {
		dprintf("[ INFO ] attrs: max_qp=%d, max_qp_wr=%d, max_cq=%d max_cqe=%d max_qp_rd_atom=%d, max_qp_init_rd_atom=%d\n", 
				attrs.max_qp, 
				attrs.max_qp_wr, attrs.max_cq, attrs.max_cqe, 
				attrs.max_qp_rd_atom, attrs.max_qp_init_rd_atom);

		dprintf("[ INFO ] ctrl attrs: initiator_depth=%d responder_resources=%d\n", 
				param->initiator_depth, param->responder_resources);
	}
#endif

	// the following should hold for initiator_depth:
	// initiator_depth <= max_qp_init_rd_atom, and
	// initiator_depth <= param->initiator_depth
	cm_params.initiator_depth = param->initiator_depth;
	// the following should hold for responder_resources:
	// responder_resources <= max_qp_rd_atom, and
	// responder_resources >= param->responder_resources
	cm_params.responder_resources = param->responder_resources;
	cm_params.rnr_retry_count = param->rnr_retry_count;
	cm_params.flow_control = param->flow_control;

	TEST_NZ(rdma_accept(q->cm_id, &cm_params));

	return 0;
}

int on_connection(struct queue *q)
{
//	printf("%s\n", __FUNCTION__);
	struct ctrl *ctrl = q->ctrl;

	TEST_Z(q->state == queue::INIT);

	if (q == &ctrl->queues[0]) {
		struct ibv_send_wr wr = {};
		struct ibv_recv_wr rwr = {};
		struct ibv_send_wr *bad_wr = NULL;
		struct ibv_recv_wr *bad_rwr = NULL;
		struct ibv_sge sge = {};
		struct memregion servermr = {};

		printf("[ INFO ] connected. sending memory region info.\n");
		printf("[ INFO ] *** Server MR key=%u base vaddr=%lx size=%d ***\n", ctrl->mr_buffer->rkey, ctrl->local_mm, LOCAL_META_REGION_SIZE/1024);
		/* XXX: base vaddr: (nil) why??  -> because of ODP */

		servermr.baseaddr = (uint64_t) ctrl->local_mm;
		servermr.key  = ctrl->mr_buffer->rkey;
		servermr.mr_size  = LOCAL_META_REGION_SIZE;

		wr.opcode = IBV_WR_SEND;
		wr.sg_list = &sge;
		wr.num_sge = 1;
		wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;

		sge.addr = (uint64_t) &servermr;
		sge.length = sizeof(servermr);

		TEST_NZ(ibv_post_send(q->qp, &wr, &bad_wr));

		// TODO: poll here XXX Where?????

		/* XXX: GET CLIENT MR REGION */
		sge.addr = (uint64_t) &ctrl->clientmr;
		sge.length = sizeof(struct memregion);
		sge.lkey = ctrl->mr_buffer->lkey;

		rwr.sg_list = &sge;
		rwr.num_sge = 1;

		TEST_NZ(ibv_post_recv(q->qp, &rwr, &bad_rwr));
	}


	q->state = queue::CONNECTED;
	return 0;
}

int on_disconnect(struct queue *q)
{
//	printf("[ INFO ] %s\n", __FUNCTION__);

	if (q->state == queue::CONNECTED) {
		q->state = queue::INIT;
		rdma_destroy_qp(q->cm_id);
		rdma_destroy_id(q->cm_id);
	}

	return 0;
}

int on_event(struct rdma_cm_event *event)
{
//	printf("[ INFO ] %s\n", __FUNCTION__);
	struct queue *q = (struct queue *) event->id->context;

	switch (event->event) {
		case RDMA_CM_EVENT_CONNECT_REQUEST:
			return on_connect_request(event->id, &event->param.conn);
		case RDMA_CM_EVENT_ESTABLISHED:
			return on_connection(q);
		case RDMA_CM_EVENT_DISCONNECTED:
			on_disconnect(q);
			return 1;
		default:
			printf("[ FAIL ] unknown event: %s\n", rdma_event_str(event->event));
			return 1;
	}
}


void die(const char *reason)
{
	fprintf(stderr, "%s - errno: %d\n", reason, errno);
	exit(EXIT_FAILURE);
}

int alloc_control()
{
	gctrl = (struct ctrl *) malloc(sizeof(struct ctrl));
	TEST_Z(gctrl);
	memset(gctrl, 0, sizeof(struct ctrl));

	gctrl->queues = (struct queue *) malloc(sizeof(struct queue) * NUM_QUEUES);
	TEST_Z(gctrl->queues);
	memset(gctrl->queues, 0, sizeof(struct queue) * NUM_QUEUES);
	for (unsigned int i = 0; i < NUM_QUEUES; ++i) {
		gctrl->queues[i].ctrl = gctrl;
		gctrl->queues[i].state = queue::INIT;
		/* XXX */
		gctrl->queues[i].page = malloc(PAGE_SIZE);
	}

	gctrl->kv = new NUMA_KV(initialTableSize/Segment::kNumSlot, 0, 0);
	dprintf("[  OK  ] KVStore Initialized\n");

	return 0;
}


/* MAIN FUNCTION HERE */
int main(int argc, char **argv)
{
	struct sockaddr_in addr = {};
	struct rdma_cm_event *event = NULL;
	struct rdma_event_channel *ec = NULL;
	struct rdma_cm_id *listener = NULL;
	uint16_t port = 0;

	const char *short_options = "vs:t:i:n:d:z:hK:P:W:";
	static struct option long_options[] =
	{
		{"verbose", 0, NULL, 'v'},
		{"tcp_port", 1, NULL, 't'},
		{"ib_port", 1, NULL, 'i'},
		{"tablesize", 1, NULL, 's'},
		{"dataset", 1, NULL, 'd'},
		{"pm_path", 1, NULL, 'z'},
		{"nr_data", 1, NULL, 'n'},
		{"netcpubind", 1, NULL, 'W'},
		{"kvcpubind", 1, NULL, 'K'},
		{"pollcpubind", 1, NULL, 'P'},
		{0, 0, 0, 0} 
	};


	while(1){
		int c = getopt_long(argc, argv, short_options, long_options, NULL);
		if(c == -1) break;
		switch(c){
			case 'i':
				ib_port = strtol(optarg, NULL, 0);
				if(ib_port <= 0){
					printf ("<%s> is invalid\n", optarg);
					return 0;
				}
				break;
			case 't':
				tcp_port = strtol(optarg, NULL, 0);
				if(tcp_port <= 0){
					printf ("<%s> is invalid\n", optarg);
					return 0;
				}
				break;
			case 'n':
				numData = strtol(optarg, NULL, 0);
				if(numData <= 0){
					printf ("<%s> is invalid\n", optarg);
					return 0;
				}
				break;
			case 's':
				initialTableSize = strtol(optarg, NULL, 0);
				if(initialTableSize <= 0){
					printf ("<%s> is invalid\n", optarg);
					return 0;
				}
				break;
			case 'd':
				data_path = strdup(optarg);
				break;
			case 'z':
				pm_path= strdup(optarg);
				break;
			case 'h':
				human = true;
				break;
			case 'v':
				verbose_flag = true;
				break;
			default:
				printf ("%c, <%s> is invalid\n", (char)c,optarg);
				return 0;
		}
	}

	nr_cpus = std::thread::hardware_concurrency();

	addr.sin_family = AF_INET;
	addr.sin_port = htons(tcp_port);

	TEST_NZ(alloc_control());

	TEST_Z(ec = rdma_create_event_channel());
	TEST_NZ(rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP));
	TEST_NZ(rdma_bind_addr(listener, (struct sockaddr *)&addr));
	TEST_NZ(rdma_listen(listener, NUM_QUEUES + 1));
	port = ntohs(rdma_get_src_port(listener));
	printf("[ INFO ] listening on port %d\n", port);

	for (unsigned int i = 0; i < NUM_QUEUES; ++i) {
//		printf("[ INFO ] waiting for queue connection: %d\n", i);
		struct queue *q = &gctrl->queues[i];

		// handle connection requests
		while (rdma_get_cm_event(ec, &event) == 0) {
			struct rdma_cm_event event_copy;

			memcpy(&event_copy, event, sizeof(*event));
			rdma_ack_cm_event(event);

			if (on_event(&event_copy) || q->state == queue::CONNECTED)
				break;
		}

		/* posting recv request */
		if (get_queue_type(i) == QP_READ_SYNC) {
			post_recv(i);
		}
		else
			pmdfc_rdma_post_recv(i);
	}

	/* XXX: After all connection done. It is needed? */
	/* servermr post send cq need polling */
	struct ibv_wc wc;
	int ne;
	do{
		ne = ibv_poll_cq(gctrl->queues[0].qp->send_cq, 1, &wc);
		if(ne < 0){
			fprintf(stderr, "[%s] ibv_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);

	if(wc.status != IBV_WC_SUCCESS){
		fprintf(stderr, "[%s] sending rdma_write failed status %s (%d)\n", __func__, ibv_wc_status_str(wc.status), wc.status);
		return 1;
	}
	
	std::thread i = std::thread( rdpma_indicator );

	// handle disconnects, etc.
	while (rdma_get_cm_event(ec, &event) == 0) {
		struct rdma_cm_event event_copy;

		memcpy(&event_copy, event, sizeof(*event));
		rdma_ack_cm_event(event);

		if (on_event(&event_copy))
			break;
	}

	i.join();

	rdma_destroy_event_channel(ec);
	rdma_destroy_id(listener);
	destroy_device(gctrl);

	return 0;
}

