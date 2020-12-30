//#include <getopt.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <thread>
#include <getopt.h>
#include <vector>
#include <ctime>

#include <numa.h>
#include <netdb.h> 
#include <netinet/in.h> 
#include <stdlib.h> 
#include <string.h> 
#include <sys/socket.h> 
#include <sys/types.h> 
#include <fcntl.h> 
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <iostream>
#include <thread>
#include <deque>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <vector>
#include <cstdio>
#include <ctime>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <atomic>

#ifdef APPDIRECT
#include "NuMA_KV_PM.h"
#else
#include "NuMA_KV.h"
#endif

#include "variables.h"
#include "CCEH_PM_hybrid.h"
#include "circular_queue.h"

#include "tcp.h"
#include "server.h"
#include "tcp_internal.h"
#include "log.h"

#define SA struct sockaddr
#define htonll(x)   ((((uint64_t)htonl(x)) << 32) + htonl(x >> 32))
#define ntohll(x)   ((((uint64_t)ntohl(x)) << 32) + ntohl(x >> 32))

struct pmnet_msg_in {
	int sockfd;
	struct pmnet_msg *hdr;
	void *page;
	size_t page_off;
#if TIME_CHECK
	bool sampled = false;
	timespec timer;
#endif
};

/* pre-defined message structure */
static struct pmnet_handshake *pmnet_hand;
static struct pmnet_msg *pmnet_keep_req, *pmnet_keep_resp;


/* option values */
int tcp_port = -1;
int ib_port = 1;
static int rdma_flag= 0;
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

/* Global variables */
struct server_context* ctx = NULL;
queue_t **lfqs;
unsigned int nr_cpus;
std::atomic<bool> done(false);

/* counting valuse */
int putcnt = 0;
int getcnt = 0;
int sample_put_q_cnt = 0;
int sample_get_q_cnt = 0;
int sample_put_cnt = 0;
int sample_get_cnt = 0;
int sample_succ_get_cnt = 0;
int sample_fail_get_cnt = 0;

std::atomic<int> process_cnt[40];

/* performance timer */
uint64_t network_elapsed=0, pmput_elapsed=0, pmget_elapsed=0, pmlog_alloc_elapsed = 0;
uint64_t pmnet_rx_elapsed=0; 
uint64_t pmget_notexist_elapsed=0, pmget_exist_elapsed=0, pmget_send_elapsed =0;
uint64_t pmput_queue_elapsed=0, pmget_queue_elapsed=0;

static void pmnet_rx_until_empty(struct pmnet_sock_container *sc);
static int pmnet_process_message(struct pmnet_sock_container *sc, 
		struct pmnet_msg *hdr, struct pmnet_msg_in *msg_in, int thisNode);

static void dprintf( const char* format, ... ) {
	if (verbose_flag) {
		va_list args;
		va_start( args, format );
		vprintf( format, args );
		va_end( args );
	}
}

static void usage(){
	printf("Usage\n");
	printf("\nOptions:\n");
	printf("\t-t --tcp_port=<port> (required) use <port> to listen tcp connection\n");
	printf("\t-i --ib_port=<port> (required) use <port> of infiniband device (default=1)\n");
	printf("\t-p --path=<port> (required) use <port> of infiniband device (default=1)\n");
}


static void print_stats() {
	printf("\n--------------------REPORT---------------------\n");
	printf("SAMPLE RATE [1/%d]\n", SAMPLE_RATE);
	printf("# of puts : %d , # of gets : %d \n",
			putcnt, getcnt);
	printf("# of sample_put_cnt : %d , # of sample_get_cnt : %d \n",
			sample_put_cnt, sample_get_cnt);
	printf("# of requested processed on cpu\n[ ");
	for ( int i = 0 ; i < nr_cpus ; i++ ) {
		printf("%d ", process_cnt[i].load());
	}
	printf(" ]\n");

	if (putcnt == 0) putcnt++;
	if (getcnt == 0) getcnt++;
	if (sample_get_cnt == 0) sample_get_cnt++;
	if (sample_put_cnt == 0) sample_put_cnt++;
	if (sample_get_q_cnt == 0) sample_get_q_cnt++;
	if (sample_put_q_cnt == 0) sample_put_q_cnt++;

	printf("\n--------------------SUMMARY--------------------\n");
	printf("Average (divided by number of ops)\n");
	printf("PUT : %.3f (usec/op), GET: %.3f (usec/op), GET(SEND): %.3f (usec/op)\n",
			pmput_elapsed/1000.0/sample_put_cnt,
			pmget_elapsed/1000.0/sample_get_cnt,
			pmget_send_elapsed/1000.0/sample_get_cnt);
	printf("PUT_Q : %.3f (usec/op), GET_Q : %.3f (usec/op)\n",
			pmput_queue_elapsed/1000.0/sample_put_q_cnt,
			pmget_queue_elapsed/1000.0/sample_get_q_cnt);

	ctx->kv->PrintStats();

	printf("--------------------FIN------------------------\n");
}

static void printCpuBuf(size_t nr_cpus, struct bitmask *bm, const char *str) {
	/* Print User specified cpus */
	dprintf("[ INFO ] %s\t threads: \t", str);
	for ( int i = 0; i< nr_cpus; i++) {
		if (i % 4 == 0)
			dprintf(" ");
		if (numa_bitmask_isbitset(bm, i))
			dprintf("1");
		else
			dprintf("0");
	}
	dprintf("\n");
}

static struct pmnet_sock_container *sc_alloc()
{
	struct pmnet_sock_container *sc, *ret = NULL;
	int status = 0;

	sc = (pmnet_sock_container *)calloc(1, sizeof(*sc));
	if (sc == NULL)
		goto out;

	ret = sc;
	sc = NULL;

out:
	free(sc);

	return ret;
}

/**
 * indicator - Show stats periodically
 */
void indicator() {
	while (!done) {
		sleep(10);
		print_stats();
	}
}

/**
 * producer - Read from socket and add to queue 
 *
 * @sc: socket container struct that contains socket informantions
 */
void producer(struct pmnet_sock_container *sc) {
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	int cpu = sched_getcpu();
	int thisNode = numa_node_of_cpu(cpu);
	dprintf("[ INFO ] NetworkT woke on cpu %d|%d\n", cpu, thisNode); 

	/* Function read bytes from connfd */
	pmnet_rx_until_empty( sc ); 
	dprintf("[ NetworkT %d Exit ]\n", cpu);
	print_stats();
	done = true;
}

void consumer(struct pmnet_sock_container *sc) {
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	int cpu = sched_getcpu();
	int thisNode = numa_node_of_cpu(cpu);
	int ret;
	struct pmnet_msg_in *msg_in;

	printf("[ INFO ] CONSUMER Running on CPU %d|%d ... \n", cpu, thisNode);

	/* consume request from queue */
	/* TODO: Batching */
	while (!done) {
		msg_in = (struct pmnet_msg_in*)dequeue(lfqs[thisNode * 2 + (cpu % 2)]); /* lfqs node0-put-Q, node0-get-Q, node1-put-Q, node1-get-Q */
		process_cnt[cpu]++;
		ret = pmnet_process_message(sc, msg_in->hdr, msg_in, thisNode);
		free(msg_in);
	}

	close( sc->sockfd );
	printf("[ INFO ] CONSUMER on %d Exit \n", cpu);
}

/* longkey = [key, index] */
static long pmnet_long_key(long key, long index)
{
	uint64_t longkey;

	/* derive long key (8byte) */
	longkey = key << 32;
	longkey |= index;

	return longkey;
}

/* initialize struct pmnet_msg_in */
static struct pmnet_msg_in *init_msg()
{
	void *page;
	struct pmnet_msg *msg;
	struct pmnet_msg_in *msg_in;

	page = calloc(1, PAGE_SIZE);
	msg = (struct pmnet_msg *)calloc(1, sizeof(struct pmnet_msg));
	msg_in = (struct pmnet_msg_in *)calloc(1, sizeof(struct pmnet_msg_in));

	msg_in->page_off = 0;
	msg_in->page = page;
	msg_in->hdr= msg;

#if defined(TIME_CHECK)
	if ( (putcnt + getcnt) % NR_Q_TIME_CHECK == 0) {
		msg_in->sampled = true;
		clock_gettime(CLOCK_MONOTONIC, &msg_in->timer);
	}
#endif

	return msg_in;
}

/* initialize message to send */
static void pmnet_init_msg(struct pmnet_msg *msg, uint16_t data_len, 
		uint16_t msg_type, uint32_t key, uint32_t index, uint32_t msg_num)
{
	memset(msg, 0, sizeof(struct pmnet_msg));
	msg->magic = htons(PMNET_MSG_MAGIC);
	msg->data_len = htons(data_len);
	msg->msg_type = htons(msg_type);
	msg->sys_status = htonl(PMNET_ERR_NONE);
	msg->status = 0;
	msg->key = htonl(key);
	msg->index = htonl(index);
	msg->msg_num = htonl(msg_num);
}

static void pmnet_initialize_handshake(void)
{
	pmnet_hand->pmnet_idle_timeout_ms = htonl(PMNET_IDLE_TIMEOUT_MS_DEFAULT);
	pmnet_hand->pmnet_keepalive_delay_ms = htonl(PMNET_KEEPALIVE_DELAY_MS_DEFAULT);
	pmnet_hand->pmnet_reconnect_delay_ms = htonl(PMNET_RECONNECT_DELAY_MS_DEFAULT);
}

/* send to client */
int pmnet_send_message(struct pmnet_sock_container *sc, 
		uint32_t msg_type, uint32_t key, uint32_t index, uint32_t msg_num,
		void *data, uint16_t datalen)
{
	int ret = 0;

	struct msghdr msghdr1;
	struct pmnet_msg *msg = NULL;
	struct iovec iov_msg[2];

	msg = (struct pmnet_msg *)calloc(1, sizeof(struct pmnet_msg));
	if (!msg) {
		printf("failed to allocate a pmnet_msg!\n");
		ret = -ENOMEM;
		goto out;
	}

	pmnet_init_msg(msg, datalen, msg_type, key, index, msg_num); 

	memset(iov_msg, 0, sizeof(iov_msg));
	iov_msg[0].iov_base = msg;
	iov_msg[0].iov_len = sizeof(struct pmnet_msg);
	iov_msg[1].iov_base = data;
	iov_msg[1].iov_len = datalen;

	memset(&msghdr1, 0, sizeof(msghdr1));
	msghdr1.msg_iov = iov_msg;
	msghdr1.msg_iovlen = 2;

	/* send message and data at once */
	sc->sc_send_lock.lock();
	ret = sendmsg(sc->sockfd, &msghdr1, MSG_DONTWAIT);
	sc->sc_send_lock.unlock();

out:
	return ret;
}

static void pmnet_sendpage(struct pmnet_sock_container *sc,
			   void *msg,
			   size_t size)
{
	int ret;

	struct msghdr msghdr1;
	struct iovec iov_msg[1];

	memset(iov_msg, 0, sizeof(iov_msg));
	iov_msg[0].iov_base = msg;
	iov_msg[0].iov_len = size;

	memset(&msghdr1, 0, sizeof(msghdr1));
	msghdr1.msg_iov = iov_msg;
	msghdr1.msg_iovlen = 1;

	while (1) {
		/* XXX: do we need lock here? */
		ret = sendmsg(sc->sockfd, &msghdr1, MSG_DONTWAIT);
		if (ret == size) {
			break;
		}
		if (ret == (ssize_t)-EAGAIN) {
			printf("%s: have to resend page\n", __func__);
			continue;
		}
		break;
	}
}


/* this returns -errno if the header was unknown or too large, etc.
 * after this is called the buffer us reused for the next message */
static int pmnet_process_message(struct pmnet_sock_container *sc, 
			struct pmnet_msg *hdr, struct pmnet_msg_in *msg_in, int thisNode)
{
	int ret = 0;
	int status;
	void *data;
	size_t datalen;
	void *to_va, *from_va;
	char temp[4096];
	uint64_t key;
	uint64_t index;
	void *saved_page;
	struct timespec start,end;
#if defined(TIME_CHECK)
	bool checkit = false;
#endif

	switch(ntohs(hdr->magic)) {
		case PMNET_MSG_STATUS_MAGIC:
			goto out; 
		case PMNET_MSG_KEEP_REQ_MAGIC:
//			pmnet_sendpage(sc, pmnet_keep_resp,
//				       sizeof(*pmnet_keep_resp));
			goto out;
		case PMNET_MSG_KEEP_RESP_MAGIC:
			goto out;
		case PMNET_MSG_MAGIC:
			break;
		default:
			ret = -EINVAL;
			goto out;
			break;
	}

	switch(ntohs(hdr->msg_type)) {
		case PMNET_MSG_PUTPAGE: {
			putcnt++;

#if defined(TIME_CHECK)
			if (msg_in->sampled) {
				clock_gettime(CLOCK_MONOTONIC, &start);
				pmput_queue_elapsed += start.tv_nsec - msg_in->timer.tv_nsec + 1000000000 * (start.tv_sec - msg_in->timer.tv_sec);
				sample_put_q_cnt++;
			}
#endif

			/* TODO: 4byte key and index should be change on demand */
			key = pmnet_long_key(ntohl(hdr->key), ntohl(hdr->index));
//			printf("GET PAGE FROM CLIENT (key=%lx, index=%lx, longkey=%lx)\n", ntohl(hdr->key), ntohl(hdr->index), key);

			/* copy page from message to local memory */
			from_va = msg_in->page;

#if defined(TIME_CHECK)
			if (putcnt % NR_PUT_TIME_CHECK == 0) {
				clock_gettime(CLOCK_MONOTONIC, &start);
				sample_put_cnt++;
				checkit = true;
			}
#endif   /* --------------------------------------------------------------------- pmput */

#ifdef APPDIRECT
			/* 
			 * Insert received page into hash 
			 * 1. Alloc POBJ and get its address
			 */
			/* TODO: batch insertion */
			TOID(char) temp;
			POBJ_ALLOC(ctx->log_pop[thisNode], &temp, char, sizeof(char)*PAGE_SIZE, NULL, NULL); 	/* TODO: Prealloc pmem */
			uint64_t temp_addr = (uint64_t)ctx->log_pop[thisNode] + temp.oid.off;
			pmemobj_memcpy_persist(ctx->log_pop[thisNode], (void*)temp_addr, from_va, sizeof(char)*PAGE_SIZE); /* TODO: this cause slowdown */

			/*
			 * 2. Save that address with key
			 */
			ctx->kv->Insert(key, (Value_t)temp_addr, 0, thisNode);
#else /* MEMORY MODE */
//			uint64_t *temp_addr = (uint64_t *)malloc(sizeof(char) * PAGE_SIZE);
//			memcpy(temp_addr, from_va, sizeof(char) * PAGE_SIZE);  /* malloc and memcpy overhead is large */
			ctx->kv->Insert(key, (Value_t)from_va, 0, thisNode);
#endif

#if defined(TIME_CHECK)
			if (checkit) {
				clock_gettime(CLOCK_MONOTONIC, &end);
				pmput_elapsed += end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);
			}
#endif   /* --------------------------------------------------------------------- pmput */

//			printf("[ Inserted %lx : ", key);
//			printf("%lx ]\n", (void *)D_RW(ctx->kv->Get(key));
			break;
		}

		/* PMNET_MSG_GETPAGE */
		case PMNET_MSG_GETPAGE:{
			getcnt++;
#ifdef PRETEND_GET_FAIL
			/* page not exists */
			ret = pmnet_send_message(sc, PMNET_MSG_NOTEXIST, ntohl(hdr->key), ntohl(hdr->index), 
					ntohl(hdr->msg_num), NULL, 0);
#else

#if defined(TIME_CHECK)
			if (msg_in->sampled) {
				clock_gettime(CLOCK_MONOTONIC, &start);
				pmget_queue_elapsed += start.tv_nsec - msg_in->timer.tv_nsec + 1000000000 * (start.tv_sec - msg_in->timer.tv_sec);
				sample_get_q_cnt++;
				sample_get_cnt++;
				checkit = true;
			}
#endif   /* --------------------------------------------------------------------- pmget_queue */

			/* alloc new page pointer to send */
			saved_page = malloc(PAGE_SIZE);

			/* key */
			key = pmnet_long_key(ntohl(hdr->key), ntohl(hdr->index));

			/*
			 * Get saved page.
			 * 1. get POBJ address of page with key
			 * 2. get page by POBJ address
			 */
			bool abort = false;
			const void* addr = ctx->kv->Get(key, thisNode);
//			ctx->kv->Get(key, 0, thisNode);

			if(!addr){
				abort = true;
			}

#if defined(TIME_CHECK)
			if (checkit) {
				clock_gettime(CLOCK_MONOTONIC, &end);
				pmget_elapsed += end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);
				sample_succ_get_cnt++;
			}
#endif   /* --------------------------------------------------------------------- pmget */

			if(!abort){
				/* page exists */
				sample_succ_get_cnt++;
#ifdef APPDIRECT
				memcpy(saved_page, addr, PAGE_SIZE);
				ret = pmnet_send_message(sc, PMNET_MSG_SENDPAGE, ntohl(hdr->key), ntohl(hdr->index), 
					ntohl(hdr->msg_num), saved_page, PAGE_SIZE);
#else
				ret = pmnet_send_message(sc, PMNET_MSG_SENDPAGE, ntohl(hdr->key), ntohl(hdr->index), 
					ntohl(hdr->msg_num), (void *)addr, PAGE_SIZE);
#endif


//				dprintf("[ Retrived (key=%x, index=%x, msg_num=%x) ", ntohl(hdr->key), ntohl(hdr->index), ntohl(hdr->msg_num));
//				dprintf("%s ]\n", saved_page);
			}
			else{
				/* page not exists */
				sample_fail_get_cnt++;
				ret = pmnet_send_message(sc, PMNET_MSG_NOTEXIST, ntohl(hdr->key), ntohl(hdr->index), 
						ntohl(hdr->msg_num), NULL, 0);
//				printf("PAGE NOT EXIST (key=%x, index=%x, msg_num=%x)\n", ntohl(hdr->key), ntohl(hdr->index), ntohl(hdr->msg_num));
			}

#if defined(TIME_CHECK)
			if (checkit) {
				clock_gettime(CLOCK_MONOTONIC, &start);
				pmget_send_elapsed += start.tv_nsec - end.tv_nsec + 1000000000 * (start.tv_sec - end.tv_sec);
			}
#endif   /* --------------------------------------------------------------------- pmget_send */

#endif /* PRETEND_GET_FAIL */
			break;
		}

		case PMNET_MSG_SENDPAGE:
			printf("SERVER-->CLIENT: PMNET_MSG_SENDPAGE\n");
			break;

		case PMNET_MSG_INVALIDATE: {
			/* key */
			key = pmnet_long_key(ntohl(hdr->key), ntohl(hdr->index));
			/* delete key */
			ctx->kv->Delete(key);
			break;
		}

		default:
			break;
	}

out:
	return ret;
}

static int pmnet_check_handshake(struct pmnet_sock_container *sc, struct pmnet_msg_in *msg_in)
{
	struct pmnet_handshake *hand = (struct pmnet_handshake *)msg_in->page;

	if (ntohll(hand->protocol_version) != PMNET_PROTOCOL_VERSION) {
		/* don't bother reconnecting if its the wrong version. */
		return -1;
	}

	sc->sc_handshake_ok = 1;
	printf("[  OK  ] Handshake!\n");

	pmnet_sendpage(sc, pmnet_hand, sizeof(*pmnet_hand));

	/* shift everything up as though it wasn't there */
	msg_in->page_off -= sizeof(struct pmnet_handshake);
	if (msg_in->page_off)
		memmove(hand, hand + 1, msg_in->page_off);

	return 0;
}


/** 
 * pmnet_advance_rx - Read from socket and enqueue it
 *
 * @sc: Socket informantions
 * @msg_in: Msg from client, be passed to consumer
 * @pushed: Whether msg_in is pushed to queue or not
 *
 */
static int pmnet_advance_rx(struct pmnet_sock_container *sc, 
		struct pmnet_msg_in *msg_in, bool& pushed)
{
	struct pmnet_msg *hdr;
	int ret = 0;
	void *data;
	size_t datalen;
	timespec curr_time;

	/* handshake */
	if ((sc->sc_handshake_ok == 0)) {
		if(msg_in->page_off < sizeof(struct pmnet_handshake)) {
			data = (char *)msg_in->page + msg_in->page_off;
			datalen = sizeof(struct pmnet_handshake) - msg_in->page_off;
			ret = read(sc->sockfd, data, datalen);
			if (ret > 0)
				msg_in->page_off += ret;
		}

		if (msg_in->page_off == sizeof(struct pmnet_handshake)) {
			pmnet_check_handshake(sc, msg_in);
			if ((sc->sc_handshake_ok == 0))
				ret = -EPROTO;
		}
		goto out;
	}

	/* read header */
	if (msg_in->page_off < sizeof(struct pmnet_msg)) {
		data = (char *)msg_in->hdr + msg_in->page_off;
		datalen = sizeof(struct pmnet_msg) - msg_in->page_off;
		ret = read(sc->sockfd, data, datalen);

		if (ret > 0) {
			msg_in->page_off += ret;
			if (msg_in->page_off == sizeof(struct pmnet_msg)) {
				hdr = msg_in->hdr;
				if (ntohs(hdr->data_len) > PMNET_MAX_PAYLOAD_BYTES) {
					printf("ntohs(hdr->data_len) =%d\n", ntohs(hdr->data_len));
					ret = -EOVERFLOW;
				}
			}
		}
		if (ret <= 0)
			goto out;
	}

	if (msg_in->page_off < sizeof(struct pmnet_msg)) {
		/* oof, still don't have a header */
		goto out;
	}

	/* this was swabbed above when we first read it */
	hdr = msg_in->hdr;

//	printf("at page_off %zu, datalen=%u\n", msg_in->page_off, ntohs(hdr->data_len));

	/* 
	 * do we need more payload? 
	 * Store payload to sc->sc_clean_page
	 */
	if (msg_in->page_off - sizeof(struct pmnet_msg) < ntohs(hdr->data_len)) {
		/* need more payload */
		data = (char *)msg_in->page + msg_in->page_off - sizeof(struct pmnet_msg);
		datalen = (sizeof(struct pmnet_msg) + ntohs(hdr->data_len) -
			  msg_in->page_off);
		ret = read(sc->sockfd, data, datalen);
		if (ret > 0)
			msg_in->page_off += ret;
		if (ret <= 0)
			goto out;
	}

	if (msg_in->page_off - sizeof(struct pmnet_msg) == ntohs(hdr->data_len)) {
		/* we can only get here once, the first time we read
		 * the payload.. so set ret to progress if the handler
		 * works out. after calling this the message is toast */
		/* TODO: Uncomment below after debugging */

#if defined(TIME_CHECK)
		if (msg_in->sampled) {
			clock_gettime(CLOCK_MONOTONIC, &curr_time);
			pmnet_rx_elapsed += curr_time.tv_nsec - msg_in->timer.tv_nsec + 1000000000 * (curr_time.tv_sec - msg_in->timer.tv_sec);
			clock_gettime(CLOCK_MONOTONIC, &msg_in->timer);
		}
#endif 

#ifdef NOENQUEUE
		pushed = true;
		ret = 1;
#else
		uint64_t key = pmnet_long_key(ntohl(msg_in->hdr->key), ntohl(msg_in->hdr->index));
#ifdef APPDIRECT
		int targetNode = ctx->kv->GetNodeID(key);
#else
		int targetNode = rand() % NUM_NUMA;
#endif
		int targetQueue = ntohs(msg_in->hdr->msg_type) == PMNET_MSG_PUTPAGE ? 0 : 1;
		enqueue(lfqs[targetNode * 2 + targetQueue], msg_in);
		pushed = true;
		ret = 1;
#endif
	}

out:
	return ret;
}

/**
 * pmnet_rx_until_empty - Read from socket until get error
 *
 * @sc: socket informantions
 *
 * create pmnet_msg_in structure and pass it to pmnet_advance_rx
 * loop until connection break
 */
static void pmnet_rx_until_empty(struct pmnet_sock_container *sc)
{
	int ret = 1;
	bool pushed = true;
	struct pmnet_msg_in *msg_in; // structure for message processing
	do {
		/* prepare new msg */
		if (pushed) {
			msg_in = init_msg();
			pushed = false;
		}
		ret = pmnet_advance_rx(sc, msg_in, pushed);
	} while (ret > 0);

	if (ret <= 0 && ret !=-EAGAIN) {
		printf("pmnet_rx_until_empty: saw error %d, closing\n", ret);
		/* not permanent so read failed handshake can retry */
	}
}


/**
 * init_network_server - Initialize network server
 */
void init_network_server()
{
	int sockfd, connfd;
	socklen_t len; 
	struct sockaddr_in servaddr, cli; 
	struct pmnet_sock_container *sc = NULL;

	pmnet_hand = (pmnet_handshake *)calloc(1, sizeof(struct pmnet_handshake));
	pmnet_keep_req = (pmnet_msg *)calloc(1, sizeof(struct pmnet_msg));
	pmnet_keep_resp = (pmnet_msg *)calloc(1, sizeof(struct pmnet_msg));
	if (!pmnet_keep_req || !pmnet_keep_resp || !pmnet_hand )
		return;

	pmnet_hand->protocol_version = htonll(PMNET_PROTOCOL_VERSION);
	pmnet_hand->connector_id = htonl(1);
	pmnet_initialize_handshake();

	pmnet_keep_req->magic = htons(PMNET_MSG_KEEP_REQ_MAGIC);
	pmnet_keep_resp->magic = htons(PMNET_MSG_KEEP_RESP_MAGIC);

	// socket create and verification 
	sockfd = socket(AF_INET, SOCK_STREAM, 0); 
	if (sockfd == -1) { 
		printf("[ FAIL ] socket creation failed...\n"); 
		exit(0); 
	} 
	else
		printf("[  OK  ] Socket successfully created..\n"); 
	bzero(&servaddr, sizeof(servaddr)); 

	// assign IP, PORT 
	servaddr.sin_family = AF_INET; 
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY); 
	servaddr.sin_port = htons(tcp_port); 

	int optval = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

	// Binding newly created socket to given IP and verification 
	if ((bind(sockfd, (SA*)&servaddr, sizeof(servaddr))) != 0) { 
		printf("[ FAIL ] socket bind failed...\n"); 
		exit(0); 
	} 
	else
		printf("[  OK  ] Socket successfully binded..\n"); 

	// Now server is ready to listen and verification 
	if ((listen(sockfd, 5)) != 0) { 
		printf("Listen failed...\n"); 
		exit(0); 
	} 
	else
		printf("[  OK  ] Server listening..\n"); 

	/*
	 * Loop and accept client.
	 * create thread for each client.
	 */
	while (!done) {
		len = sizeof(cli); 
		// Accept the data packet from client and verification 
		connfd = accept(sockfd, (SA*)&cli, &len); 
		if (connfd < 0) { 
			printf("[ FAIL ] server acccept failed...\n"); 
			exit(0); 
		} 
		else
			printf("[  OK  ] server acccept the client...\n"); 

		sc = sc_alloc();
		sc->sockfd = connfd;

		std::thread p;
		std::thread i = std::thread( indicator );

		/* Equally distribute NetworkThread */
		auto cpu_id = 0;
		while(true) {
			if (numa_bitmask_isbitset(netcpubuf, cpu_id)) {
				numa_bitmask_clearbit(netcpubuf, cpu_id);

				p = std::thread( producer, sc );

				cpu_set_t cpuset;
				CPU_ZERO(&cpuset);
				CPU_SET(cpu_id, &cpuset);
				int rc = pthread_setaffinity_np(p.native_handle(),
						sizeof(cpu_set_t), &cpuset);
				if (rc != 0) {
					std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
				}
//				dprintf("NewtorkT [%d] bind on CPU %d\n", numNetworkThreads, cpu_id);
				break;
			}
			cpu_id++;
		}

		// A mutex ensures orderly access to std::cout from multiple threads.
		std::mutex iomutex;
		std::vector<std::thread> threads(nr_cpus/2);
		for (unsigned i = 0; i < nr_cpus/2; ++i) {   /* XXX */
			threads[i] = std::thread(consumer, sc);

			// Create a cpu_set_t object representing a set of CPUs. Clear it and mark
			// only CPU i as set.
			// threads[i] would be assigned to CPU i
			cpu_set_t cpuset;
			CPU_ZERO(&cpuset);
			CPU_SET(i, &cpuset);
			int rc = pthread_setaffinity_np(threads[i].native_handle(),
					sizeof(cpu_set_t), &cpuset);
			if (rc != 0) {
				std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
			}
		}

		for (auto& t : threads) {
			t.join();
		}

		p.join();
		i.join();
	} 


	if (sockfd)
		close(sockfd); 
}

/* Initialize server context including log and hashtable */
static struct server_context* server_init_ctx(char *ipath){
	int flags;
	void* ptr;
	char base_path[32] = "/mnt/pmem0";
	char log_path[32] = "/jy/log";

	ctx = (struct server_context*)malloc(sizeof(struct server_context));
	ctx->node_id = 0;

#ifdef APPDIRECT
	for(int i=0; i<NUM_NUMA; i++){
		snprintf(&base_path[9], sizeof(int), "%d", i);
		strncpy(&base_path[10], log_path, strlen(log_path));
		dprintf("[ INFO ] File used for log: %s\n", base_path);
		if(access(base_path, 0) != 0){
			ctx->log_pop[i] = pmemobj_create(base_path, POBJ_LAYOUT_NAME(LOG), LOG_SIZE, 0666);
			if(!ctx->log_pop[i]){
				perror("pmemobj_create");
				exit(1);
			}
		}
		else{
			ctx->log_pop[i] = pmemobj_open(base_path, POBJ_LAYOUT_NAME(LOG));
			if(!ctx->log_pop[i]){
				perror("pmemobj_open");
				exit(1);
			}
		}
	}
	printf("[  OK  ] log initialized\n");

	bool exists = false;
	char path[32] = "/mnt/pmem0";

	for(int i=0; i<NUM_NUMA; i++){
		snprintf(&path[9], sizeof(int), "%d", i);
		strncpy(&path[10], pm_path, strlen(pm_path));
		dprintf("[ INFO ] File used for index: %s\n", path);
		if(access(path, 0) != 0){
			ctx->pop[i] = pmemobj_create(path, POBJ_LAYOUT_NAME(HashTable), INDEX_SIZE, 0666);
			if(!ctx->pop[i]){
				perror("pmemobj_create");
				exit(1);
			}
		}
		else{
			ctx->pop[i] = pmemobj_open(path, POBJ_LAYOUT_NAME(HashTable));
			if(!ctx->pop[i]){
				perror("pmemobj_open");
				exit(1);
			}
			exists = true;
		}
	}

	if(!exists) {
		ctx->kv = new NUMA_KV(ctx->pop, initialTableSize/Segment::kNumSlot, numKVThreads, numPollThreads);
		dprintf("[  OK  ] KVStore Initialized\n");
	} else {
		ctx->kv = new NUMA_KV(ctx->pop, true, numKVThreads, numPollThreads);
		if(!ctx->kv->Recovery()) {
			dprintf("[ FAIL ] KVStore Recovered \n");
			exit(1);
		}
		dprintf("[  OK  ] KVStore Recovered \n");
	}
#else /* MEMORY MODE */
	ctx->kv = new NUMA_KV(initialTableSize/Segment::kNumSlot, numKVThreads, numPollThreads);
	dprintf("[  OK  ] KVStore Initialized\n");
#endif

	return ctx;
}

/* SIGINT handler */
void sigint_callback_handler(int signum) {
	print_stats();
	// Terminate program
	exit(signum);
}

/* SIGSEGV handler */
void sigsegv_callback_handler(int signum) {
	printf("segfault\n");
	print_stats();
	// Terminate program
	exit(signum);
}

int init_tcp_server(char* path) 
{ 	
	server_init_ctx(path);

	/* 
	 * Listen and accept socket 
	 * Looping inside this fuction
	 */
	init_network_server();

	return 0;
}

int main(int argc, char* argv[]){
	char hostname[64];

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
					usage();
					return 0;
				}
				break;
			case 't':
				tcp_port = strtol(optarg, NULL, 0);
				if(tcp_port <= 0){
					printf ("<%s> is invalid\n", optarg);
					usage();
					return 0;
				}
				break;
			case 'n':
				numData = strtol(optarg, NULL, 0);
				if(numData <= 0){
					printf ("<%s> is invalid\n", optarg);
					usage();
					return 0;
				}
				break;
			case 's':
				initialTableSize = strtol(optarg, NULL, 0);
				if(initialTableSize <= 0){
					printf ("<%s> is invalid\n", optarg);
					usage();
					return 0;
				}
				break;
			case 'd':
				data_path = strdup(optarg);
				break;
			case 'z':
				pm_path= strdup(optarg);
				break;
			case 'W':
				netcpubuf = numa_parse_cpustring(optarg);
				if (!netcpubuf) {
					printf ("<%s> is invalid\n", optarg);
					usage();
				}
				break;
			case 'K':
				kvcpubuf = numa_parse_cpustring(optarg);
				if (!kvcpubuf) {
					printf ("<%s> is invalid\n", optarg);
					usage();
				}
				break;
			case 'P':
				pollcpubuf = numa_parse_cpustring(optarg);
				if (!pollcpubuf) {
					printf ("<%s> is invalid\n", optarg);
					usage();
				}
				break;
			case 'h':
				human = true;
				break;
			case 'v':
				verbose_flag = 1;
				break;
			default:
				printf ("%c, <%s> is invalid\n", (char)c,optarg);
				usage();
				return 0;
		}
	}

	gethostname(hostname, 64);
	dprintf("[ INFO ] Hostname:\t %s IB port:\t %d TCP port:\t %d\n", hostname, ib_port, tcp_port);

	struct timespec i_start, i_end, g_start, g_end;
	uint64_t i_elapsed, g_elapsed;


	/* GET NUMA and CPU information */
	nr_cpus = std::thread::hardware_concurrency();
	dprintf("[ INFO ] NR_CPUS= %d\n", nr_cpus);

	/* count number of threads */
	for (int i = 0; i < nr_cpus ; i++) {
		if (numa_bitmask_isbitset(netcpubuf, i))
			numNetworkThreads++;	

		if (numa_bitmask_isbitset(kvcpubuf, i))
			numKVThreads++;	

		if (numa_bitmask_isbitset(pollcpubuf, i))
			numPollThreads++;	
	}

	/* Print User specified cpu binding */
	printCpuBuf(nr_cpus, netcpubuf, "net");
	printCpuBuf(nr_cpus, kvcpubuf, "kv");
	printCpuBuf(nr_cpus, pollcpubuf, "cqpoll");

	lfqs = (queue_t**)malloc(NUM_NUMA * sizeof(queue_t*) * 2); /* GET, PUT QUEUE seperation */
	for (int i = 0; i < NUM_NUMA * 2; i++) {
		lfqs[i] = create_queue("lfqs");
	}

	for (int i = 0; i < nr_cpus; i++) {
		process_cnt[i] = 0;
	}

	signal(SIGINT, sigint_callback_handler);
	signal(SIGSEGV, sigsegv_callback_handler);
	init_tcp_server(path);

	return 0;
}

