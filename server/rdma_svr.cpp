#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <thread>
#include <set>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <numa.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <getopt.h>
#include <chrono>

#include "circular_queue.h"
#include "rdma_svr.h"
#include "variables.h"

#define CBLOOMFILTER 1
#define TIME_CHECK 1

/* option values */
int tcp_port = -1;
char *path;
size_t initialTableSize = 0;
size_t numKVThreads = 0;
size_t numNetworkThreads = 0;
size_t numPollThreads = 0;
bool numa_on = false;
bool verbose_flag = false;
bool bf_flag = false;
bool human = false;
struct bitmask *netcpubuf;
size_t BUFFER_SIZE = ((1UL << 30) * 10); // 10GB

/*  Global values */
static struct ctrl **gctrl = NULL;
static unsigned int queue_ctr = 0;
static unsigned int client_ctr = 0;
unsigned int nr_cpus;
std::atomic<bool> done(false);
uint64_t global_mr = 0;
CountingBloomFilter<Key_t>* global_bf = NULL;
struct ibv_mr *global_mr_buffer = NULL;

std::atomic<uint64_t> page_offset(0);

#ifdef SRQ
struct ibv_srq *srq[16]; /* 1 SRQ per Client */
#endif

/* counting values */
int putcnt = 0;
int getcnt = 0;
int found_cnt = 0;
int notfound_cnt = 0;
int bfsendcnt= 0;

/* performance timer */
uint64_t rdpma_handle_write_elapsed=0;
uint64_t rdpma_handle_write_malloc_elapsed=0;
uint64_t rdpma_handle_recv_poll_elapsed=0;
uint64_t rdpma_handle_write_memcpy_elapsed=0;
uint64_t rdpma_handle_write_poll_elapsed=0;
uint64_t rdpma_handle_read_elapsed=0;
uint64_t rdpma_handle_read_poll_elapsed=0;
uint64_t rdpma_handle_read_poll_notfound_elapsed=0;
uint64_t rdpma_handle_read_poll_found_elapsed=0;
uint64_t rdpma_handle_read_poll_found_memcpy_elapsed=0;

uint64_t rdpma_bf_send_elapsed=0;

#ifdef APP_DIRECT
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

static uint64_t longkeyToKey(uint64_t longkey) {
	return longkey >> 32;
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
	if (bfsendcnt == 0) bfsendcnt++;

	printf("\n--------------------SUMMARY--------------------\n");
	printf("Average (divided by number of ops)\n");
	printf("Write[insert: %.3f ,malloc: %.3f, memcpy: %.3f] (us), Read: %.3f (us)\n",
			rdpma_handle_write_elapsed/putcnt/1000.0,
			rdpma_handle_write_malloc_elapsed/putcnt/1000.0,
			rdpma_handle_write_memcpy_elapsed/putcnt/1000.0,
			rdpma_handle_read_elapsed/getcnt/1000.0);
	printf("(Poll) Write: %.3f (us), Read: %.3f [%.3f (memcpy: %.3f) / %.3f](us), Req: %.3f (us)\n",
			rdpma_handle_write_poll_elapsed/putcnt/1000.0,
			(rdpma_handle_read_poll_found_elapsed + rdpma_handle_read_poll_notfound_elapsed)/getcnt/1000.0,
			rdpma_handle_read_poll_found_elapsed/found_cnt/1000.0,
			rdpma_handle_read_poll_found_memcpy_elapsed/found_cnt/1000.0,
			rdpma_handle_read_poll_notfound_elapsed/notfound_cnt/1000.0,
			rdpma_handle_recv_poll_elapsed/getcnt/1000.0);

	printf("BF send: %.3f (us)\n",
			rdpma_bf_send_elapsed/bfsendcnt/1000.0);

	gctrl[0]->kv->PrintStats();

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

/*
 * send_bf - Send bloomfilter of server to client.
 * It uses onesided RDMA and overwrite client side bloomfilter,
 * so it don't involve client side CPUs.
 */
void send_bf(struct queue *q, int cid) {
	struct ibv_sge sge;
	struct ibv_sge *sges;
	struct ibv_send_wr wr = {}; 
	struct ibv_send_wr *bad_wr = NULL; 
	struct ibv_wc wc;
	int ne;
	int ret;
	int i = 0;
#if defined(TIME_CHECK)
	struct timespec start, end;
#endif

	// TODO:
	// Only updated part of bloomfilter
	// ibv_post_send returns errno 12 (ENOMEM)
	// Maybe related to max_send_wr
	if (false)
	{
		std::set updatedBlocks = global_bf->GetUpdatedBlocks();
		size_t sge_size = updatedBlocks.size();
		cout << "BF send for " << sge_size << " blocks\n";

		if (sge_size == 0) return;

		sges = (struct ibv_sge *)malloc(sge_size * sizeof(struct ibv_sge));

		for(auto it = updatedBlocks.begin(); it != updatedBlocks.end(); it++){
			uint64_t bitAddr = global_bf->GetBoolBitArray() + SENDINGBLOCKSIZE * *it;

			sges[i].addr = bitAddr;
			sges[i].length = SENDINGBLOCKSIZE;
			sges[i].lkey = gctrl[cid]->bf_mr_bits_buffer->lkey;

			i++;
		}
		global_bf->ResetUpdatedBlocks();
	}
	else
	{
		uint64_t bitAddr = global_bf->GetBoolBitArray();
		size_t size = global_bf->GetNumLongs() * sizeof(long);

//		printf("First bits of bf = %lu\n", global_bf->GetLong(0)); :: PASS
//		printf("Existence of Key 0 = %s \n", global_bf->QueryBitBloom(0)?"true":"false");
//		printf("Existence of Key 300 = %s \n", global_bf->Query(0)?"true":"false");

		sge.addr = bitAddr;
		sge.length = size;
		sge.lkey = gctrl[cid]->bf_mr_bits_buffer->lkey;
	}

	wr.opcode = IBV_WR_RDMA_WRITE; /* IBV_WR_SEND_WITH_IMM same */
	wr.sg_list = &sge;
	wr.num_sge = 1;
//	wr.sg_list = sges;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.wr.rdma.remote_addr = gctrl[cid]->bfmr.baseaddr;
	wr.wr.rdma.rkey        = gctrl[cid]->bfmr.key;

	q->m.lock(); // TODO: should we need a lock?
	ret = ibv_post_send(q->qp, &wr, &bad_wr);
	if(ret){
		fprintf(stderr, "[%s] ibv_post_send to node failed with %d\n", __func__, ret);
	}

#if defined(TIME_CHECK)
		clock_gettime(CLOCK_MONOTONIC, &start);
#endif
	do{
		ne = ibv_poll_cq(q->qp->send_cq, 1, &wc);
		if(ne < 0){
			fprintf(stderr, "[%s] ibv_poll_cq failed\n", __func__);
			return;
		}
	}while(ne < 1);

	if(wc.status != IBV_WC_SUCCESS){
		fprintf(stderr, "[%s] sending rdma_write failed status %s (%d)\n", __func__, ibv_wc_status_str(wc.status), wc.status);
		return;
	}

#if defined(TIME_CHECK)
		clock_gettime(CLOCK_MONOTONIC, &end);
		rdpma_bf_send_elapsed += end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);
#endif
	q->m.unlock();

//	free(sges);

	printf("[ INFO ] Send bloomfilter to client\n");
	bfsendcnt++;

	return;
}

/**
 * bf_sender - Send bf to client periodically
 */
void rdpma_bf_sender(int c) {
	while (!done) {
		sleep(10);
		if (global_bf) {
			global_bf->ToOrdinaryBloomFilter();  // Zip counting bloomfilter to normal bloomfilter.
			send_bf(&gctrl[c]->queues[0], c);
		}
	}
}

int post_recv(int client_id, int queue_id){
	struct ibv_recv_wr wr;
	struct ibv_recv_wr* bad_wr;
	struct ibv_sge sge;

	memset(&wr, 0, sizeof(struct ibv_recv_wr));
	memset(&sge, 0, sizeof(struct ibv_sge));

	wr.wr_id = 0;
	wr.sg_list = &sge;
	wr.num_sge = 0;
	wr.next = NULL;

#ifdef SRQ
	TEST_NZ(ibv_post_srq_recv(gctrl[client_id]->queues[queue_id].qp->srq, &wr, &bad_wr));
#else
	TEST_NZ(ibv_post_recv(gctrl[client_id]->queues[queue_id].qp, &wr, &bad_wr));
#endif
	return 0;
}

int post_recv_with_addr(uint64_t addr, int client_id, int queue_id){
	struct ibv_recv_wr wr;
	struct ibv_recv_wr* bad_wr;
	struct ibv_sge sge[2];

	memset(&wr, 0, sizeof(struct ibv_recv_wr));
	memset(&sge, 0, sizeof(struct ibv_sge));

	sge[0].addr = addr;
	sge[0].length = (PAGE_SIZE) * BATCH_SIZE;
	sge[0].lkey = gctrl[client_id]->mr_buffer->lkey;

#if 0
	sge[1].addr = (uint64_t)GET_LOCAL_META_REGION(gctrl[client_id]->local_mm, queue_id, 0);
	sge[1].length = sizeof(uint64_t) * BATCH_SIZE;
//	sge[1].lkey = gctrl[client_id]->mr_buffer->lkey;
	sge[1].lkey = global_mr_buffer->lkey;
#endif

	wr.wr_id = addr;
	wr.sg_list = &sge[0];
	wr.num_sge = 1;
	wr.next = NULL;

#ifdef SRQ
	TEST_NZ(ibv_post_srq_recv(gctrl[client_id]->queues[queue_id].qp->srq, &wr, &bad_wr));
#else
	TEST_NZ(ibv_post_recv(gctrl[client_id]->queues[queue_id].qp, &wr, &bad_wr));
#endif
	return 0;
}

static void process_write_twosided(struct queue *q, uint64_t target, int cid, int qid, int mid) {
	struct ibv_send_wr wr = {};
	struct ibv_send_wr *bad_wr = NULL;
	struct ibv_sge sge = {};
	int ret;
	uint64_t local_keys[BATCH_SIZE];
#if defined(TIME_CHECK)
	struct timespec start, end;
#endif

	uint64_t* keys = (uint64_t*)GET_LOCAL_META_REGION(gctrl[cid]->local_mm, qid, 0);
	for ( unsigned int i = 0 ; i < BATCH_SIZE ; i++ ) 
		local_keys[i] = keys[i];

	wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM; /* IBV_WR_SEND_WITH_IMM same */
	wr.sg_list = &sge;
	wr.num_sge = 0;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.imm_data = htonl(bit_mask(0, mid, MSG_READ_REPLY, TX_WRITE_COMMITTED, qid));

	ret = ibv_post_send(q->qp, &wr, &bad_wr);
	if(ret){
		fprintf(stderr, "[%s] ibv_post_send to node failed with %d\n", __func__, ret);
	}

	for ( unsigned int i = 0 ; i < BATCH_SIZE ; i++ ) {
		uint64_t cur_page = target + PAGE_SIZE * i;
#if defined(TIME_CHECK)
		clock_gettime(CLOCK_MONOTONIC, &start);
#endif
		gctrl[cid]->kv->Insert(local_keys[i], (Value_t)cur_page);
#if defined(TIME_CHECK)
		clock_gettime(CLOCK_MONOTONIC, &end);
		rdpma_handle_write_elapsed+= end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);
#endif
		dprintf("[ INFO ] MSG_WRITE page %lx, key %ld (decimal) Inserted\n", cur_page, longkeyToKey(local_keys[i]));
		dprintf("[ INFO ] page %s\n", (char *)cur_page);
#if 0
		if ( (uint64_t)atoi((char *)cur_page) != longkeyToKey(local_keys[i]) ) {
			printf("[ FAIL ] key %ld (decimal) inserted, but page %s [buf_id:%d, qid:%d, mid:%d]\n", longkeyToKey(local_keys[i]), (char *)cur_page, i, qid, mid);
		}
#endif
	}

#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &end);
#endif
	{
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
	}

	uint64_t local_page_offset = page_offset.fetch_add(BATCH_SIZE, std::memory_order_relaxed);
	void *save_page = (void *)(GET_FREE_PAGE_REGION(global_mr) + (PAGE_SIZE) * local_page_offset);
	post_recv_with_addr((uint64_t)save_page, cid, qid);

#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &start);
	rdpma_handle_write_poll_elapsed += start.tv_nsec - end.tv_nsec + 1000000000 * (start.tv_sec - end.tv_sec);
#endif
}


static void process_write(struct queue *q, int cid, int qid, int mid){
	struct ibv_send_wr wr = {};
	struct ibv_send_wr *bad_wr = NULL;
	struct ibv_sge sge = {};
	int ret;
#if defined(TIME_CHECK)
	struct timespec start, end;
#endif

	uint64_t* key = (uint64_t*)GET_LOCAL_META_REGION(gctrl[cid]->local_mm, qid, mid);
	uint64_t local_key = *key;
	uint64_t page = (uint64_t)GET_LOCAL_PAGE_REGION(gctrl[cid]->local_mm, qid, mid);
#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &start);
#endif
	void *save_page = (void *)(GET_FREE_PAGE_REGION(global_mr) + PAGE_SIZE * page_offset++);
#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &end);
	rdpma_handle_write_malloc_elapsed += end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);
	clock_gettime(CLOCK_MONOTONIC, &end);
#endif
	memcpy((char *)save_page, (char *)page, PAGE_SIZE);
#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &start);
	rdpma_handle_write_memcpy_elapsed += start.tv_nsec - end.tv_nsec + 1000000000 * (start.tv_sec - end.tv_sec);
#endif
	gctrl[cid]->kv->Insert(local_key, (Value_t)save_page);
#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &end);
	rdpma_handle_write_elapsed+= end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);
#endif

	dprintf("[ INFO ] MSG_WRITE page %lx, key %ld Inserted\n", (uint64_t)page, longkeyToKey(local_key));
	dprintf("[ INFO ] page %s\n", (char *)save_page);

#if 1 /* XXX: if this block is commented, some client message ignored */
	sge.addr = 0;
	sge.length = 0;
	sge.lkey = gctrl[cid]->mr_buffer->lkey;

	wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM; /* IBV_WR_SEND_WITH_IMM same */
	wr.sg_list = &sge;
	wr.num_sge = 0;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.imm_data = htonl(bit_mask(0, mid, MSG_READ_REPLY, TX_WRITE_COMMITTED, qid));

	ret = ibv_post_send(q->qp, &wr, &bad_wr);
	if(ret){
		fprintf(stderr, "[%s] ibv_post_send to node failed with %d\n", __func__, ret);
	}

#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &end);
#endif
	{
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
	}
#endif

#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &start);
	rdpma_handle_write_poll_elapsed += start.tv_nsec - end.tv_nsec + 1000000000 * (start.tv_sec - end.tv_sec);
#endif
}

static void process_read(struct queue *q, int cid, int qid, int mid){
	struct ibv_send_wr wr = {};
	struct ibv_send_wr *bad_wr = NULL;
	struct ibv_sge sge = {};
#if defined(TIME_CHECK)
	struct timespec start, end;
	struct timespec memcpy_start, memcpy_end;
#endif

#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &start);
#endif
	uint64_t* key = (uint64_t*)GET_LOCAL_META_REGION(gctrl[cid]->local_mm, qid, mid);
	uint64_t local_key = *key;
	uint64_t* remote_addr = (uint64_t*)GET_REMOTE_ADDRESS_BASE(gctrl[cid]->local_mm, qid, mid);
	uint64_t target_addr = (uint64_t)GET_REMOTE_ADDRESS_BASE(gctrl[cid]->local_mm, qid, mid);
	dprintf("[ INFO ] key= %ld (decimal), remote address= %lx\n", longkeyToKey(local_key), *remote_addr);

	/* 1. Get page address -> value */
	void* value;
	bool abort = false;

	value = (void *)gctrl[cid]->kv->Get((Key_t&)local_key); 

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
		dprintf("[ INFO ] page %lx, key %lx Searched\n", (uint64_t)value, local_key);
		dprintf("[ INFO ] page %s\n", (char *)value);

		/* 2. Send page retrieved to client so that client can initiate RDMA READ */	
#if defined(TIME_CHECK)
		clock_gettime(CLOCK_MONOTONIC, &memcpy_start);
#endif
		memcpy((char *)target_addr, (char *)value, PAGE_SIZE);
#if defined(TIME_CHECK)
		clock_gettime(CLOCK_MONOTONIC, &memcpy_end);
		rdpma_handle_read_poll_found_memcpy_elapsed+= memcpy_end.tv_nsec - memcpy_start.tv_nsec + 1000000000 * (memcpy_end.tv_sec - memcpy_start.tv_sec);
#endif
		dprintf("[ INFO ] page %s\n", (char *)target_addr);

		wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
		wr.sg_list = &sge;
		wr.num_sge = 0;
		wr.send_flags = IBV_SEND_SIGNALED;
		wr.imm_data = htonl(bit_mask(0, mid, MSG_READ_REPLY, TX_READ_COMMITTED, qid));

		TEST_NZ(ibv_post_send(q->qp, &wr, &bad_wr));

	} else {
		notfound_cnt++;

		wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
		wr.sg_list = &sge;
		wr.num_sge = 0;
		wr.send_flags = IBV_SEND_SIGNALED;
		wr.imm_data = htonl(bit_mask(0, mid, MSG_READ_REPLY, TX_READ_ABORTED, qid));

		TEST_NZ(ibv_post_send(q->qp, &wr, &bad_wr));
	}

	{
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

static void process_write_odp(struct queue *q, int cid, int qid, int mid){
	struct ibv_wc wc2;
	int ne, ret;
	struct ibv_send_wr wr = {};
	struct ibv_send_wr *bad_wr = NULL;
	struct ibv_sge sge = {};
#if defined(TIME_CHECK)
	struct timespec start, end;
#endif
	uint64_t* key = (uint64_t*)GET_LOCAL_META_REGION(gctrl[cid]->local_mm, qid, mid);
	uint64_t local_key = *key;
	uint64_t* target_addr = (uint64_t*)GET_REMOTE_ADDRESS_BASE(gctrl[cid]->local_mm, qid, mid);

#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &start);
#endif
	void *save_page = (void *)(GET_FREE_PAGE_REGION(global_mr) + PAGE_SIZE * page_offset++);
#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &end);
	rdpma_handle_write_malloc_elapsed += end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);
#endif
	*target_addr = (uint64_t)save_page;

#ifdef ODP
	/* Prefatch MR */
	{
		struct ibv_sge sg_list;

		sg_list.lkey = gctrl[cid]->mr_buffer->lkey;
		sg_list.addr = (uintptr_t)save_page;
		sg_list.length = PAGE_SIZE;

		ret = ibv_advise_mr(gctrl[cid]->dev->pd, IBV_ADVISE_MR_ADVICE_PREFETCH_WRITE,
				IB_UVERBS_ADVISE_MR_FLAG_FLUSH,
				&sg_list, 1);

		if (ret)
			fprintf(stderr, "Couldn't prefetch MR(%d). Continue anyway\n", ret);
	}
#endif

	sge.addr = (uint64_t)target_addr;
	sge.length = sizeof(uint64_t);
	sge.lkey = gctrl[cid]->mr_buffer->lkey;

	wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM; /* IBV_WR_SEND_WITH_IMM same */
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.wr.rdma.remote_addr = gctrl[cid]->clientmr.baseaddr + GET_OFFSET_FROM_BASE_TO_ADDR(qid, mid);
	wr.wr.rdma.rkey        = gctrl[cid]->clientmr.key;
	wr.imm_data = htonl(bit_mask(0, mid, MSG_READ_REPLY, TX_WRITE_COMMITTED, qid));

	ret = ibv_post_send(q->qp, &wr, &bad_wr);
	if(ret){
		fprintf(stderr, "[%s] ibv_post_send to node failed with %d\n", __func__, ret);
	}

#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &start);
#endif
	gctrl[cid]->kv->Insert(local_key, (Value_t)save_page);
#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &end);
	rdpma_handle_write_elapsed+= end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);
#endif
	dprintf("[ INFO ] MSG_WRITE page %lx, local_key %ld (decimal) Inserted\n", (uint64_t)save_page, longkeyToKey(local_key));
	dprintf("[ INFO ] page content %s\n", (char *)save_page);

#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &end);
#endif
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
}

static void process_read_odp(struct queue *q, int cid, int qid, int mid){
	struct ibv_wc wc2;
	int ne;
	struct ibv_send_wr wr = {};
	struct ibv_send_wr *bad_wr = NULL;
	struct ibv_sge sge = {};
#if defined(TIME_CHECK)
	struct timespec start, end;
#endif

#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &start);
#endif

	uint64_t* key = (uint64_t*)GET_LOCAL_META_REGION(gctrl[cid]->local_mm, qid, mid);
	uint64_t local_key = *key;
	uint64_t* remote_addr = (uint64_t*)GET_REMOTE_ADDRESS_BASE(gctrl[cid]->local_mm, qid, mid);
	uint64_t local_remote_addr = *remote_addr;
	dprintf("[ INFO ] key= %lx, remote address= %lx\n", local_key, local_remote_addr);

	/* 1. Get page address -> value */
	void* value;
	bool abort = false;

	value = (void *)gctrl[cid]->kv->Get((Key_t&)local_key); 

	if(!value){
		dprintf("Value for key[%ld] not found\n", longkeyToKey(local_key));
		abort = true;
	}

#if defined(TIME_CHECK)
	clock_gettime(CLOCK_MONOTONIC, &end);
	rdpma_handle_read_elapsed += end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);
#endif

	if( !abort ) {
		found_cnt++;
		dprintf("[ INFO ] page %lx, key %ld Searched\n", (uint64_t)value, longkeyToKey(local_key));
		dprintf("[ INFO ] page %s\n", (char *)value);

#if 0
		if ( atoi((char *)value) != longkeyToKey(local_key) ) {
			printf("[ FAIL ] key %ld (decimal) searched, but page %s\n", longkeyToKey(local_key), (char *)value);
		}
#endif

		/* 2. Send page retrieved to client memory directly */	
		sge.addr = (uint64_t)value;
		sge.length = PAGE_SIZE;
		sge.lkey = gctrl[cid]->mr_buffer->lkey;

		wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM; /* IBV_WR_SEND_WITH_IMM same */
		wr.sg_list = &sge;
		wr.num_sge = 1;
		wr.send_flags = IBV_SEND_SIGNALED;
		wr.wr.rdma.remote_addr = local_remote_addr;
		wr.wr.rdma.rkey        = gctrl[cid]->clientmr.key;
		wr.imm_data = htonl(bit_mask(0, mid, MSG_READ_REPLY, TX_READ_COMMITTED, qid));

		TEST_NZ(ibv_post_send(q->qp, &wr, &bad_wr));
	} else {
		notfound_cnt++;

		wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
		wr.sg_list = &sge;
		wr.num_sge = 0;
		wr.send_flags = IBV_SEND_SIGNALED;
		wr.imm_data = htonl(bit_mask(0, mid, MSG_READ_REPLY, TX_READ_ABORTED, qid));

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
	if ( abort ) {
		rdpma_handle_read_poll_notfound_elapsed += start.tv_nsec - end.tv_nsec + 1000000000 * (start.tv_sec - end.tv_sec);
	} else {
		rdpma_handle_read_poll_found_elapsed += start.tv_nsec - end.tv_nsec + 1000000000 * (start.tv_sec - end.tv_sec);
	}
#endif
}

static void server_recv_poll_cq(struct queue *q, int client_id, int queue_id) {
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	struct ibv_wc wc;
	int ne;

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
			fprintf(stderr, "%s: Failed status %s (%d)\n", __func__, ibv_wc_status_str(wc.status), wc.status);
			die("Failed status");
		}

		if((int)wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM){
			int qid, mid, type, tx_state, num;

			bit_unmask(ntohl(wc.imm_data), &num, &mid, &type, &tx_state, &qid);
			dprintf("[ INFO ] On Q[%d]: qid(%d), mid(%d), type(%d), tx_state(%d), num(%d)\n", queue_id, qid, mid, type, tx_state, num);

			post_recv(client_id, queue_id);
			if (queue_id != qid)
				printf("[ ERRR ] Queue ID mismatch!!\n");

			if(type == MSG_WRITE){
				putcnt++;
#ifdef NORMALPUT
				process_write(q, client_id, qid, mid);
#elif BIGMRPUT 
				process_write_odp(q, client_id, qid, mid);
#endif
			} else if(type == MSG_READ) {
				getcnt++;
#ifdef NORMALGET
				process_read(q, client_id, qid, mid);
#elif BIGMRGET
				process_read_odp(q, client_id, qid, mid);
#elif TWOSIDED
				process_read_odp(q, client_id, qid, mid);
#endif
			}
		}
		else if((int)wc.opcode == IBV_WC_RDMA_READ){
			dprintf("[%s]: received WC_RDMA_READ\n", __func__);
			/* the client is reading data from read region*/
		}
		else if ( (int)wc.opcode == IBV_WC_RECV ){
			if ( wc.wr_id != 0 ) {
				putcnt = putcnt + BATCH_SIZE;
				int qid, mid, type, tx_state, num;

				bit_unmask(ntohl(wc.imm_data), &num, &mid, &type, &tx_state, &qid);
				dprintf("[ INFO ] IBV_WC_RECV On Q[%d]: qid(%d), mid(%d), type(%d), tx_state(%d), num(%d)\n", queue_id, qid, mid, type, tx_state, num);

				if (queue_id != qid)
					printf("[ ERRR ] Queue ID mismatch!!\n");

				process_write_twosided(q, wc.wr_id, client_id, qid, mid);
			}
			else {
				/* only for first connection */
				dprintf("[ INFO ] connected. receiving memory region info.\n");
				printf("[ INFO ] *** Client MR key=%u base vaddr=%p size=%lu (KB) ***\n", gctrl[client_id]->clientmr.key, (void *)gctrl[client_id]->clientmr.baseaddr
						, gctrl[client_id]->clientmr.mr_size/1024);
#ifdef CBLOOMFILTER
				if (gctrl[client_id]->bfmr.key != 0)
					printf("[ INFO ] *** Client BF MR key=%u base vaddr=%p size=%lu (KB) ***\n", gctrl[client_id]->bfmr.key, (void *)gctrl[client_id]->bfmr.baseaddr
							, gctrl[client_id]->bfmr.mr_size/1024);
#endif
			}
		}else{
			fprintf(stderr, "Received a weired opcode (%d)\n", (int)wc.opcode);
		}
	}
}


static device *get_device(struct queue *q)
{
	struct device *dev = NULL;

	// ctrl에 dev가 등록되어 있지 않다면
	if (!q->ctrl->dev) {
		dev = (struct device *) malloc(sizeof(*dev));
		TEST_Z(dev);
		dev->verbs = q->cm_id->verbs;
		TEST_Z(dev->verbs);
		dev->pd = ibv_alloc_pd(dev->verbs);
		TEST_Z(dev->pd);

		struct ctrl *ctrl = q->ctrl;    


#ifdef ODP
		/* 
		 * To create an implicit ODP MR, IBV_ACCESS_ON_DEMAND should be set, 
		 * addr should be 0 and length should be SIZE_MAX.
		 */
		TEST_Z(ctrl->mr_buffer = ibv_reg_mr( dev->pd, NULL, (uint64_t)-1, \
					IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_ON_DEMAND));
		ctrl->local_mm = (uint64_t)malloc(LOCAL_META_REGION_SIZE + BUFFER_SIZE);
#elif ONESIDED
		ctrl->local_mm = (uint64_t)malloc(BUFFER_SIZE);
		TEST_Z(ctrl->local_mm);

		TEST_Z(ctrl->mr_buffer = ibv_reg_mr(
					dev->pd,
					(void *)ctrl->local_mm,
					BUFFER_SIZE,
					IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ));
#else
		// global_mr 은 free page들을 관리하는 공간 (client 끼리 공유됨)
		if (global_mr == 0) {
			// Shared MR region for every client.
			global_mr = (uint64_t)malloc(BUFFER_SIZE +  NUM_CLIENT * LOCAL_META_REGION_SIZE);
			TEST_Z(global_mr);
		}

		ctrl->local_mm = global_mr + BUFFER_SIZE + LOCAL_META_REGION_SIZE * ctrl->cid;

		TEST_Z(ctrl->mr_buffer = ibv_reg_mr(
					dev->pd,
					(void *)global_mr,
					BUFFER_SIZE + NUM_CLIENT * LOCAL_META_REGION_SIZE,
					IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ));
		printf("[ INFO ] registered perclient memory region key=%u base vaddr=%lx\n", ctrl->mr_buffer->rkey, ctrl->local_mm);

		if (bf_flag && global_bf) {
			ctrl->bf = global_bf;
			TEST_Z(ctrl->bf_mr_buffer = ibv_reg_mr(
						dev->pd,
						(void *)ctrl->bf->GetBaseAddr(),
						ctrl->bf->GetNumBits(),
						IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ));
			printf("[ INFO ] registered perclient BF memory region key=%u base vaddr=%lx\n", ctrl->bf_mr_buffer->rkey, ctrl->bf->GetBaseAddr());

			TEST_Z(ctrl->bf_mr_bits_buffer = ibv_reg_mr(
						dev->pd,
						(void *)ctrl->bf->GetBoolBitArray(),
						ctrl->bf->GetNumLongs() * sizeof(long),
						IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ));
			printf("[ INFO ] registered perclient BF bitfield MR key=%u base vaddr=%lx\n", ctrl->bf_mr_bits_buffer->rkey, ctrl->bf->GetBoolBitArray());
		}

#endif
		printf("[  OK  ] MEMORY MODE DRAM MR initialized\n");
		q->ctrl->dev = dev;
	}

	return q->ctrl->dev;
}

static void destroy_device(struct ctrl *ctrl)
{
	TEST_Z(ctrl->dev);

	ibv_dereg_mr(ctrl->mr_buffer);
	ibv_dereg_mr(global_mr_buffer);
	free((void*)global_mr);
	ibv_dealloc_pd(ctrl->dev->pd);
	free(ctrl->dev);
	ctrl->dev = NULL;
}

static void create_qp(struct queue *q, int client_number)
{
	struct ibv_qp_init_attr qp_attr = {};

	struct ibv_cq* send_cq = ibv_create_cq(q->cm_id->verbs, 256, NULL, NULL, 0);
	if(!send_cq){
		fprintf(stderr, "ibv_create_cq for send_cq failed\n");
	}

	struct ibv_cq* recv_cq = ibv_create_cq(q->cm_id->verbs, 256, NULL, NULL, 0);
	if(!send_cq){
		fprintf(stderr, "ibv_create_cq for send_cq failed\n");
	}

#ifdef SRQ
	qp_attr.srq = srq[client_number];
#endif
	qp_attr.send_cq = send_cq;
	qp_attr.recv_cq = recv_cq;
	qp_attr.qp_type = IBV_QPT_RC; /* XXX */ 
	qp_attr.cap.max_send_wr = 4096;
	qp_attr.cap.max_recv_wr = 4096;
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
	int client_number = client_ctr;
	struct queue *q = &gctrl[client_number]->queues[queue_number];

	/* next client */
	if (queue_number == NUM_QUEUES - 1) {
		client_ctr++;
		queue_ctr = 0;
	}

	TEST_Z(q->state == queue::INIT);
//	printf("[ INFO ] %s\n", __FUNCTION__);

	id->context = q;
	q->cm_id = id;

	struct device *dev = get_device(q);

#ifdef SRQ
	/* if it is first queue pair of this client, create SRQ(Shared Recv Queue) */
	if (queue_number == 0) {
		struct ibv_srq_init_attr srq_init_attr;
		 
		memset(&srq_init_attr, 0, sizeof(srq_init_attr));
		 
		srq_init_attr.attr.max_wr  = 4096;
		srq_init_attr.attr.max_sge = 2;

		srq[client_number] = ibv_create_srq(q->ctrl->dev->pd, &srq_init_attr);
		if (!srq[client_number]) {
			fprintf(stderr, "Error, ibv_create_srq() failed\n");
		}
	}
#endif

	create_qp(q, client_number);

#ifndef ONESIDED
	/* XXX : Poller start here */
	/* Create polling thread associated with q */
	std::thread p = std::thread( server_recv_poll_cq, q, client_number, queue_number);

	/*
	 * Create a cpu_set_t object representing a set of CPUs. Clear it and mark
	 * only CPU i as set.
	 * threads[i] would be assigned to CPU i
	 */
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(client_number * NUM_QUEUES + queue_number, &cpuset);
	int rc = pthread_setaffinity_np(p.native_handle(),
			sizeof(cpu_set_t), &cpuset);
	if (rc != 0) {
		fprintf(stderr, "Error calling pthread_setaffinity_np\n");
	}

	p.detach();
#endif

	TEST_NZ(ibv_query_device(dev->verbs, &attrs));

	cm_params.initiator_depth = param->initiator_depth;
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
		struct ibv_send_wr wr[2] = {};
		struct ibv_recv_wr rwr[2] = {};
		struct ibv_send_wr *bad_wr = NULL;
		struct ibv_recv_wr *bad_rwr = NULL;
		struct ibv_sge sge[2] = {};
		struct memregion servermr = {};
		struct memregion bfmr = {};

		printf("[ INFO ] connected. sending memory region info.\n");
//		printf("[ INFO ] *** Server per client MR key=%u base vaddr=%lx size=%lu (KB)***\n", ctrl->mr_buffer->rkey, ctrl->local_mm, (LOCAL_META_REGION_SIZE)/1024);

		servermr.baseaddr = (uint64_t) ctrl->local_mm;
		servermr.key  = ctrl->mr_buffer->rkey;
		servermr.mr_size  = BUFFER_SIZE + NUM_CLIENT * LOCAL_META_REGION_SIZE;

		if (bf_flag) {
			bfmr.baseaddr = ctrl->bf->GetBaseAddr();
			bfmr.key  = ctrl->bf_mr_buffer->rkey;
			bfmr.mr_size  = ctrl->bf->GetNumBits();
		} else {
			bfmr.baseaddr = 0;
			bfmr.key  = 0;
			bfmr.mr_size  = 0;
		}

		wr[0].next = &wr[1];
		wr[0].opcode = IBV_WR_SEND;
		wr[0].sg_list = &sge[0];
		wr[0].num_sge = 1;
		wr[0].send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;

		wr[1].next = NULL;
		wr[1].opcode = IBV_WR_SEND;
		wr[1].sg_list = &sge[1];
		wr[1].num_sge = 1;
		wr[1].send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;

		sge[0].addr = (uint64_t) &servermr;
		sge[0].length = sizeof(servermr);

		sge[1].addr = (uint64_t) &bfmr;
		sge[1].length = sizeof(bfmr);

		TEST_NZ(ibv_post_send(q->qp, &wr[0], &bad_wr));

#ifndef ONESIDED
		/* GET CLIENT MR REGION */
		sge[0].addr = (uint64_t) &ctrl->clientmr;
		sge[0].length = sizeof(struct memregion);
		sge[0].lkey = ctrl->mr_buffer->lkey;

		rwr[0].next = NULL;
		rwr[0].sg_list = &sge[0];
		rwr[0].num_sge = 1;

#ifdef CBLOOMFILTER
		rwr[0].next = &rwr[1];
		/* GET CLIENT BFMR REGION */
		sge[1].addr = (uint64_t) &ctrl->bfmr;
		sge[1].length = sizeof(struct memregion);
		sge[1].lkey = ctrl->mr_buffer->lkey;

		rwr[1].sg_list = &sge[1];
		rwr[1].num_sge = 1;
#endif


#ifdef SRQ
		TEST_NZ(ibv_post_srq_recv(q->qp->srq, &rwr[0], &bad_rwr));
#else
		TEST_NZ(ibv_post_recv(q->qp, &rwr[0], &bad_rwr));
#endif
#endif /* ndef ONESIDED */
	}


	q->state = queue::CONNECTED;
	return 0;
}

int on_disconnect(struct queue *q)
{
	if (q->state == queue::CONNECTED) {
		q->state = queue::INIT;
		rdma_destroy_qp(q->cm_id);
		rdma_destroy_id(q->cm_id);
	}

	return 0;
}

int on_event(struct rdma_cm_event *event)
{
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
	dprintf("[ INFO ] Bloom filter %s\n", bf_flag ? "enabled": "disabled");
	if (bf_flag) {
		global_bf = new CountingBloomFilter<Key_t>(NUM_HASHES, BF_SIZE);
		dprintf("[  OK  ] Bloom filter(%d, %d) Initialized\n", global_bf->GetNumHashes(), global_bf->GetNumBits());
	}
	KVStore *kv = new KV( BUFFER_SIZE / 4096,  global_bf);
	gctrl = (struct ctrl **)malloc(sizeof(struct ctrl *) * NUM_CLIENT);
	for ( unsigned int c = 0 ; c < NUM_CLIENT ; ++c) {
		gctrl[c] = (struct ctrl *) malloc(sizeof(struct ctrl));
		TEST_Z(gctrl[c]);
		memset(gctrl[c], 0, sizeof(struct ctrl));
		gctrl[c]->cid = c;

		gctrl[c]->queues = (struct queue *) malloc(sizeof(struct queue) * NUM_QUEUES);
		TEST_Z(gctrl[c]->queues);
		memset(gctrl[c]->queues, 0, sizeof(struct queue) * NUM_QUEUES);
		for (unsigned int i = 0; i < NUM_QUEUES; ++i) {
			gctrl[c]->queues[i].ctrl = gctrl[c];
			gctrl[c]->queues[i].state = queue::INIT;
		}
		gctrl[c]->kv = kv;
		dprintf("[  OK  ] Global controler & KVStore Initialized for client %d\n", c);
	}

	return 0;
}

void printUsage() {
  std::cerr
    << "usage: rdma_svr -<option> <args>\n\n"
    << "The options supported by rdma_svr are:\n\n"
    << "  verbose(v)                print debug messages\n"
    << "  help(h)                   print this messages\n"
    << "  human(H)                  print human readable output\n"
    << "  bloomfilter(b)            turn bloomfilter on\n"
    << "  tcp_port(t) <port>        listen clients on <port>\n"
    << "  tablesize(s) <size>       set table bucket size to <size>\n"
    << "  buffersize(S) <size>      set memory buffer size to <size>MByte\n"
    << "  netcpubind(W) <set>       set worker threads as <set>\n"
    << std::endl;
} 

/* MAIN FUNCTION HERE */
int main(int argc, char **argv)
{
	struct sockaddr_in addr = {};
	struct rdma_cm_event *event = NULL;
	struct rdma_event_channel *ec = NULL;
	struct rdma_cm_id *listener = NULL;
	uint16_t port = 0;

	const char *short_options = "vhbs:S:t:i:n:d:z:HK:P:W:";
	static struct option long_options[] =
	{
		{"verbose", 0, NULL, 'v'},
		{"help", 0, NULL, 'h'},
		{"bloomfilter", 0, NULL, 'b'},
		{"human", 0, NULL, 'H'},
		{"tcp_port", 1, NULL, 't'},
		{"tablesize", 1, NULL, 's'},
		{"buffersize", 1, NULL, 'S'},
		{"netcpubind", 1, NULL, 'W'},
		{0, 0, 0, 0} 
	};

	while(1){
		int c = getopt_long(argc, argv, short_options, long_options, NULL);
		if(c == -1) break;
		switch(c){
			case 't':
				tcp_port = strtol(optarg, NULL, 0);
				if(tcp_port <= 0){
					printf ("<%s> is invalid\n", optarg);
					printUsage();
					return 0;
				}
				break;
			case 's':
				initialTableSize = strtol(optarg, NULL, 0);
				if(initialTableSize <= 0){
					printf ("<%s> is invalid\n", optarg);
					printUsage();
					return 0;
				}
				break;
			case 'S':
				BUFFER_SIZE = ((1UL << 20) * strtol(optarg, NULL, 0));
				if(BUFFER_SIZE <= 0){
					printf ("<%s> is invalid\n", optarg);
					printUsage();
					return 0;
				}
				break;
			case 'h':
				printUsage();
				return 0;
				break;
			case 'H':
				human = true;
				break;
			case 'v':
				verbose_flag = true;
				break;
			case 'b':
				bf_flag = true;
				break;
			default:
				printf ("%c, <%s> is invalid\n", (char)c,optarg);
				printUsage();
				return 0;
		}
	}

	nr_cpus = std::thread::hardware_concurrency();

	// if initialTableSize is not set, use default value
	if (initialTableSize == 0) initialTableSize = BUFFER_SIZE / 4096;

	// Server configuration display (when human flag is true)
	if (human) {
		printf("[ INFO ] Configurations \n");
		printf("\t  +-- BUFFER_SIZE \t: %lu = %lu MB \n", BUFFER_SIZE, BUFFER_SIZE/1024/1024);
		printf("\t  +-- HT SIZE     \t: %lu buckets\n", initialTableSize);
		printf("\t  +-- Bloomfilter \t: %s \n", bf_flag ? "on" : "off");
		if (bf_flag) printf("\t        +-- BF_SIZE     \t: %d \n", BF_SIZE);
		if (bf_flag) printf("\t        +-- NUM_HASHES  \t: %d \n", NUM_HASHES);
#ifdef CBLOOMFILTER 
		printf("\t        +-- CBLOOMFILTER \t: on \n");
#else
		if (bf_flag) printf("\t        +-- SBLOOMFILTER \t: on \n");
#endif
		printf("\t  +-- BATCH_SIZE  \t: %d \n", BATCH_SIZE);
		printf("\t  +-- NUM_QUEUES  \t: %d \n", NUM_QUEUES);
		printf("\t  +-- NUM_CLIENT  \t: %d \n", NUM_CLIENT);
		printf("\n");
	}

	addr.sin_family = AF_INET;
	addr.sin_port = htons(tcp_port);

	TEST_NZ(alloc_control());
	TEST_Z(ec = rdma_create_event_channel());
	TEST_NZ(rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP));
	TEST_NZ(rdma_bind_addr(listener, (struct sockaddr *)&addr));
	TEST_NZ(rdma_listen(listener, NUM_QUEUES + 1));
	port = ntohs(rdma_get_src_port(listener));
	printf("[ INFO ] listening on port %d\n", port);

	std::thread indicator;
	std::thread bf_sender[NUM_CLIENT];
	for (unsigned int c = 0; c < NUM_CLIENT; ++c) {
		for (unsigned int i = 0; i < NUM_QUEUES; ++i) {
//			printf("[ INFO ] waiting for queue connection: %d\n", i);
			struct queue *q = &gctrl[c]->queues[i];

			// handle connection requests
			while (rdma_get_cm_event(ec, &event) == 0) {
				struct rdma_cm_event event_copy;

				memcpy(&event_copy, event, sizeof(*event));
				rdma_ack_cm_event(event);

				if (on_event(&event_copy) || q->state == queue::CONNECTED)
					break;
			}

			/* Prepost recv WQE */
			for (unsigned int j = 0; j < 100; ++j) {
#ifdef TWOSIDED
				if ( i < NUM_QUEUES / 2 ) {
					/* READ QUEUE */
					post_recv(c, i);
				} else {
					/* WRITE QUEUE */
					uint64_t local_page_offset = page_offset.fetch_add(BATCH_SIZE, std::memory_order_relaxed);
					void *save_page = (void *)(GET_FREE_PAGE_REGION(global_mr) + (PAGE_SIZE) * local_page_offset); 
					post_recv_with_addr((uint64_t) save_page, c, i);
				}
#else
				post_recv(c, i);
#endif
			}
		}
		printf("[ INFO ] queue connection established for client #%u.\n", c);

		/* servermr post send cq need polling */
		struct ibv_wc wc;
		int ne;
		do{
			ne = ibv_poll_cq(gctrl[c]->queues[0].qp->send_cq, 1, &wc);
			if(ne < 0){
				fprintf(stderr, "[%s] ibv_poll_cq failed\n", __func__);
				return 1;
			}
		}while(ne < 1);

		if(wc.status != IBV_WC_SUCCESS){
			fprintf(stderr, "[%s] sending rdma_write failed status %s (%d)\n", __func__, ibv_wc_status_str(wc.status), wc.status);
			return 1;
		}

#ifndef ONESIDED
		if (c == 0)
			indicator = std::thread( rdpma_indicator );

#ifdef CBLOOMFILTER
		bf_sender[c] = std::thread( rdpma_bf_sender, c );
#endif
#endif
	}

	// handle disconnects, etc.
	while (rdma_get_cm_event(ec, &event) == 0) {
		struct rdma_cm_event event_copy;

		memcpy(&event_copy, event, sizeof(*event));
		rdma_ack_cm_event(event);

		if (on_event(&event_copy))
			break;
	}

	indicator.join();
	bf_sender[0].join();

	rdma_destroy_event_channel(ec);
	rdma_destroy_id(listener);
	for (unsigned int c = 0; c < NUM_CLIENT; ++c) {
		destroy_device(gctrl[c]);
	}

	return 0;
}

