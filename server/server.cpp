#include <stdio.h> 
#include <netdb.h> 
#include <netinet/in.h> 
#include <stdlib.h> 
#include <string.h> 
#include <sys/socket.h> 
#include <sys/types.h> 
#include <fcntl.h> 
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
#include <boost/lockfree/queue.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread.hpp>
#include <boost/atomic.hpp>

#include "util/pair.h"
#include "src/CCEH.h"

#include "tcp.h"
#include "tcp_internal.h"

#define TIME_CHECK 1

#define  BUFF_SIZE   1024
#define SA struct sockaddr 

#define htonll(x)   ((((uint64_t)htonl(x)) << 32) + htonl(x >> 32))
#define ntohll(x)   ((((uint64_t)ntohl(x)) << 32) + ntohl(x >> 32))

using std::deque;
using namespace boost::lockfree;

struct pmnet_msg_in {
	int sockfd;
	struct pmnet_msg *hdr;
	void *page;
	size_t page_off;
#if TIME_CHECK
	timespec timer;
#endif
};

/* pre-defined message structure */
static struct pmnet_handshake *pmnet_hand;
static struct pmnet_msg *pmnet_keep_req, *pmnet_keep_resp;

/* lock-free-queue */
boost::lockfree::queue<pmnet_msg_in *> new_queue(512);
boost::atomic<bool> done (false);
boost::atomic<bool> alldone (false);

int putcnt = 0;
int getcnt = 0;

/* CCEH hashTable */
CCEH* hashTable;

/* performance timer */
uint64_t network_elapsed=0, pmput_elapsed=0, pmalloc_elapsed = 0, pmget_elapsed=0;
uint64_t pmnet_rx_elapsed=0; 
uint64_t pmget_notexist_elapsed=0, pmget_exist_elapsed=0;
uint64_t pmput_queue_elapsed=0, pmget_queue_elapsed=0;

static void pmnet_rx_until_empty(struct pmnet_sock_container *sc);
static int pmnet_process_message(struct pmnet_sock_container *sc, 
		struct pmnet_msg *hdr, struct pmnet_msg_in *msg_in);

static struct pmnet_sock_container *sc_alloc()
{
	struct pmnet_sock_container *sc, *ret = NULL;
	int status = 0;

	sc = (pmnet_sock_container *)calloc(1, sizeof(*sc));
	if (sc == NULL)
		goto out;

	printf("sc alloced\n");

	ret = sc;
	sc = NULL;

out:
	free(sc);

	return ret;
}

void producer(struct pmnet_sock_container *sc) {
	std::thread::id this_id = std::this_thread::get_id(); 
	printf("[ new PRODUCER %lx Running... ]\n", this_id);

	/* Function read bytes from connfd */
	pmnet_rx_until_empty( sc ); 
	printf("[ PRODUCER %lx Exit ]\n", this_id);
	done = true;
}

void consumer(struct pmnet_sock_container *sc) {
	int ret;
	struct pmnet_msg_in *msg_in;

	std::thread::id this_id = std::this_thread::get_id(); 
	printf("[ new CONSUMER %lx Running... ]\n", this_id);

	/* consume request from queue */
	while (!done) {
		while (new_queue.pop(msg_in)){
			ret = pmnet_process_message(sc, msg_in->hdr, msg_in);
			free(msg_in);
		}
	}

	while (new_queue.pop(msg_in)) {
		ret = pmnet_process_message(sc, msg_in->hdr, msg_in);
	}

	close( sc->sockfd );
	printf("[ CONSUMER %lx Exit ]\n", this_id);
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
	clock_gettime(CLOCK_MONOTONIC, &msg_in->timer);
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
//	pmnet_hand->pmhb_heartbeat_timeout_ms = cpu_to_be32(
//			PMHB_MAX_WRITE_TIMEOUT_MS);
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
	ret = sendmsg(sc->sockfd, &msghdr1, MSG_DONTWAIT);

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
			printf("%s: sendmsg(%d)\n", __func__, ret);
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
			struct pmnet_msg *hdr, struct pmnet_msg_in *msg_in)
{
	int ret = 0;
	int status;
	char reply[4096];
	void *data;
	size_t datalen;
	void *to_va, *from_va;
	char temp[4096];
	uint64_t key;
	uint64_t index;
	void *saved_page;
	struct timespec start,end;

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
			clock_gettime(CLOCK_MONOTONIC, &start);

#if defined(TIME_CHECK)
			pmput_queue_elapsed += start.tv_nsec - msg_in->timer.tv_nsec + 1000000000 * (start.tv_sec - msg_in->timer.tv_sec);
#endif

			/* TODO: 4byte key and index should be change on demand */
			key = pmnet_long_key(ntohl(hdr->key), ntohl(hdr->index));
//			printf("GET PAGE FROM CLIENT (key=%lx, index=%lx, longkey=%lx)\n", ntohl(hdr->key), ntohl(hdr->index), key);

			/* copy page from message to local memory */
			from_va = msg_in->page;

			clock_gettime(CLOCK_MONOTONIC, &start);
			/* Insert received page into hash */
			hashTable->Insert(key,hashTable->save_page((char *)msg_in->page).oid.off);
//
			clock_gettime(CLOCK_MONOTONIC, &end);
			pmput_elapsed += end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);

			printf("[ Inserted %lx : ", key);
			printf("%lx ]\n", hashTable->Get(key));
			break;
		}

		case PMNET_MSG_GETPAGE:{
			getcnt++;
//			printf("CLIENT-->SERVER: PMNET_MSG_GETPAGE\n");
			clock_gettime(CLOCK_MONOTONIC, &start);

#if defined(TIME_CHECK)
			pmget_queue_elapsed += start.tv_nsec - msg_in->timer.tv_nsec + 1000000000 * (start.tv_sec - msg_in->timer.tv_sec);
#endif

			/* alloc new page pointer to send */
			saved_page = calloc(1, PAGE_SIZE);

			/* key */
			key = pmnet_long_key(ntohl(hdr->key), ntohl(hdr->index));

			clock_gettime(CLOCK_MONOTONIC, &start);
			/* Get page from Hash and copy to saved_page */
			ret = hashTable->load_page(hashTable->Get(key), (char *)saved_page, 4096);

			if (ret != 0) {
				/* page not exists */
				memset(&reply, 0, PAGE_SIZE);
				ret = pmnet_send_message(sc, PMNET_MSG_NOTEXIST, ntohl(hdr->key), ntohl(hdr->index), 
						ntohl(hdr->msg_num), NULL, 0);

				clock_gettime(CLOCK_MONOTONIC, &end);
				pmget_notexist_elapsed += end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);

				printf("PAGE NOT EXIST (key=%lx, index=%lx, msg_num=%lx)\n", ntohl(hdr->key), ntohl(hdr->index), ntohl(hdr->msg_num));
//				printf("SERVER-->CLIENT: PMNET_MSG_NOTEXIST(%d)\n",ret);
			} else {
				/* page exists */
//				printf("SEND PAGE with long key=%lx\n", key);
//				printf(">> GET: %s\n", saved_page);
				ret = pmnet_send_message(sc, PMNET_MSG_SENDPAGE, ntohl(hdr->key), ntohl(hdr->index), 
					ntohl(hdr->msg_num), saved_page, PAGE_SIZE);

				clock_gettime(CLOCK_MONOTONIC, &end);
				pmget_exist_elapsed += end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);

				printf("[ Retrived (key=%lx, index=%lx, msg_num=%lx) ", ntohl(hdr->key), ntohl(hdr->index), ntohl(hdr->msg_num));
				printf("%lx ]\n", hashTable->Get(key));
//				printf("SERVER-->CLIENT: PMNET_MSG_SENDPAGE(%d)\n",ret);
			}
			break;

		}

		case PMNET_MSG_SENDPAGE:
			printf("SERVER-->CLIENT: PMNET_MSG_SENDPAGE\n");
			break;

		case PMNET_MSG_INVALIDATE: {
			/* key */
			key = pmnet_long_key(ntohl(hdr->key), ntohl(hdr->index));
			/* delete key */
			hashTable->Delete(key);

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
	printf("Handshake ok!\n");

	pmnet_sendpage(sc, pmnet_hand, sizeof(*pmnet_hand));

	/* shift everything up as though it wasn't there */
	msg_in->page_off -= sizeof(struct pmnet_handshake);
	if (msg_in->page_off)
		memmove(hand, hand + 1, msg_in->page_off);

	return 0;
}


/* 
 * Read from socket
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
		clock_gettime(CLOCK_MONOTONIC, &curr_time);
		pmnet_rx_elapsed += curr_time.tv_nsec - msg_in->timer.tv_nsec + 1000000000 * (curr_time.tv_sec - msg_in->timer.tv_sec);
		clock_gettime(CLOCK_MONOTONIC, &msg_in->timer);
#endif 
		if (new_queue.push(msg_in)) {
			pushed = true;
			ret = 1;
		}
	}

out:
	return ret;
}

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


/*
 * Initialize network server
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
		printf("socket creation failed...\n"); 
		exit(0); 
	} 
	else
		printf("Socket successfully created..\n"); 
	bzero(&servaddr, sizeof(servaddr)); 

	// assign IP, PORT 
	servaddr.sin_family = AF_INET; 
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY); 
	servaddr.sin_port = htons(PORT); 

	// Binding newly created socket to given IP and verification 
	if ((bind(sockfd, (SA*)&servaddr, sizeof(servaddr))) != 0) { 
		printf("socket bind failed...\n"); 
		exit(0); 
	} 
	else
		printf("Socket successfully binded..\n"); 

	// Now server is ready to listen and verification 
	if ((listen(sockfd, 5)) != 0) { 
		printf("Listen failed...\n"); 
		exit(0); 
	} 
	else
		printf("Server listening..\n"); 

	/* Thread pool */
	boost::thread_group producer_threads, consumer_threads;

	/*
	 * Loop and accept client.
	 * create thread for each client.
	 */
	while (!done) {
		len = sizeof(cli); 
		// Accept the data packet from client and verification 
		connfd = accept(sockfd, (SA*)&cli, &len); 
		if (connfd < 0) { 
			printf("server acccept failed...\n"); 
			exit(0); 
		} 
		else
			printf("server acccept the client...\n"); 

		sc = sc_alloc();
		sc->sockfd = connfd;

		boost::thread p = boost::thread( producer, sc );
		producer_threads.add_thread(&p);
		boost::thread c1 = boost::thread( consumer, sc );
		consumer_threads.add_thread(&c1);
		boost::thread c2 = boost::thread( consumer, sc );
		consumer_threads.add_thread(&c2);
		boost::thread c3 = boost::thread( consumer, sc );
		consumer_threads.add_thread(&c3);
		boost::thread c4 = boost::thread( consumer, sc );
		consumer_threads.add_thread(&c4);
		boost::thread c5 = boost::thread( consumer, sc );
		consumer_threads.add_thread(&c5);
		boost::thread c6 = boost::thread( consumer, sc );
		consumer_threads.add_thread(&c6);
		boost::thread c7 = boost::thread( consumer, sc );
		consumer_threads.add_thread(&c7);
		boost::thread c8 = boost::thread( consumer, sc );
		consumer_threads.add_thread(&c8);
		boost::thread c9 = boost::thread( consumer, sc );
		consumer_threads.add_thread(&c9);
		boost::thread c10 = boost::thread( consumer, sc );
		consumer_threads.add_thread(&c10);

		producer_threads.join_all();
		consumer_threads.join_all();

		printf("\n--------------------REPORT---------------------\n");

		printf("PUT : %lu (us), GET_E : %lu (us), GET_NE : %lu (us)\n",
				pmput_elapsed/1000,
				pmget_exist_elapsed/1000, 
				pmget_notexist_elapsed/1000);
		printf("PM alloc : %lu (us)\n\n",
				hashTable->pmalloc_elapsed/1000);

		printf("RX & Queuing delay\n");
		printf("RX : %lu (us), PUT_Q : %lu (us), GET_Q : %lu (us)\n",
				pmnet_rx_elapsed/1000,
				pmput_queue_elapsed/1000,
				pmget_queue_elapsed/1000);

		printf("# of puts : %d , # of gets : %d \n",
				putcnt, getcnt);

		if (putcnt == 0)
			putcnt++;
		if (getcnt == 0)
			getcnt++;

		printf("\n--------------------SUMMARY--------------------\n");
		printf("Average (divided by number of ops)\n");
		printf("PUT : %lu (us), PUT_ALLOC : %lu (us), GET_TOTAL : %lu (us)\n",
				pmput_elapsed/1000/putcnt,
				hashTable->pmalloc_elapsed/1000/putcnt,
				(pmget_exist_elapsed/1000 + pmget_notexist_elapsed/1000)/getcnt);

		printf("PUT_Q : %lu (us), GET_Q : %lu (us)\n",
				pmput_queue_elapsed/1000/putcnt,
				pmget_queue_elapsed/1000/getcnt);

		printf("\n--------------------FIN------------------------\n");
	} 


	if (sockfd)
		close(sockfd); 
}

/*
 * Initialize CCEH
 * return: CCEH
 */
CCEH *init_cceh(char* file)
{
	CCEH* ht = new CCEH(file);
	printf("CCEH create....\n");

	return ht;
}


int main(int argc, char* argv[]) 
{ 	
	/* New CCEH hash table */
	hashTable = init_cceh(argv[1]);

	using namespace std;
	cout << "boost::lockfree::queue is ";
	if (!new_queue.is_lock_free())
		cout << "not ";
	cout << "lockfree" << endl;

	/* 
	 * Listen and accept socket 
	 * Looping inside this fuction
	 */
	init_network_server();

	return 0;
} 
