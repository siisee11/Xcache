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

#include "log.hpp"

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
};


static struct pmnet_msg *pmnet_keep_req, *pmnet_keep_resp;

/* lock-free-queue */
boost::lockfree::queue<pmnet_msg_in *> new_queue(128);
boost::atomic<bool> done (false);
boost::atomic<int> putcnt (0);
boost::atomic<int> getcnt (0);


/* CCEH hashTable */
CCEH* hashTable;

static void pmnet_rx_until_empty(struct pmnet_sock_container *sc);
static int pmnet_process_message(int sockfd, 
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
		/* TODO: Uncomment below after debugging */
		while (new_queue.pop(msg_in)){
			getcnt++;
//			log<LOG_DEBUG>(L"CONSUMER queue.pop() cnt=%1%") % (int)getcnt;
			ret = pmnet_process_message(sc->sockfd, msg_in->hdr, msg_in);
			free(msg_in);
		}
	}

	while (new_queue.pop(msg_in)) {
		getcnt++;
		log<LOG_DEBUG>(L"CONSUMER queue.pop() cnt=%1%") % (int)getcnt;
		ret = pmnet_process_message(sc->sockfd, msg_in->hdr, msg_in);
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

	return msg_in;
}

/* initialize message to send */
static void pmnet_init_msg(struct pmnet_msg *msg, uint16_t data_len, 
		uint16_t msg_type, uint32_t key, uint32_t index)
{
	memset(msg, 0, sizeof(struct pmnet_msg));
	msg->magic = htons(PMNET_MSG_MAGIC);
	msg->data_len = htons(data_len);
	msg->msg_type = htons(msg_type);
	msg->sys_status = htonl(PMNET_ERR_NONE);
//	msg->sys_status = htonl(0);
	msg->status = 0;
	msg->key = htonl(key);
	msg->index = htonl(index);
}

/* send to client */
int pmnet_send_message(int sockfd, uint32_t msg_type, uint32_t key, uint32_t index,
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

	pmnet_init_msg(msg, datalen, msg_type, key, index); 

	memset(iov_msg, 0, sizeof(iov_msg));
	iov_msg[0].iov_base = msg;
	iov_msg[0].iov_len = sizeof(struct pmnet_msg);
	iov_msg[1].iov_base = data;
	iov_msg[1].iov_len = datalen;

	memset(&msghdr1, 0, sizeof(msghdr1));
	msghdr1.msg_iov = iov_msg;
	msghdr1.msg_iovlen = 2;
	
	/* send message and data at once */
	ret = sendmsg(sockfd, &msghdr1, MSG_DONTWAIT);

out:
	return ret;
}

static void pmnet_sendpage(int sockfd,
			   struct pmnet_msg *msg,
			   size_t size)
{
	int ret;

	struct msghdr msghdr1;
	struct iovec iov_msg[1];

	memset(iov_msg, 0, sizeof(iov_msg));
	iov_msg[0].iov_base = msg;
	iov_msg[0].iov_len = size;

	while (1) {
		/* XXX: do we need lock here? */
		ret = sendmsg(sockfd, &msghdr1, MSG_DONTWAIT);
		if (ret == size)
			break;
		if (ret == (ssize_t)-EAGAIN) {
			printf("%s: have to resend page\n", __func__);
			continue;
		}
		break;
	}
}


/* this returns -errno if the header was unknown or too large, etc.
 * after this is called the buffer us reused for the next message */
static int pmnet_process_message(int sockfd, struct pmnet_msg *hdr, struct pmnet_msg_in *msg_in)
{
	int ret = 0;
	int status;
	char reply[4096];
	void *data;
	size_t datalen;
	void *to_va, *from_va;
	uint64_t key;
	uint64_t index;
	void *saved_page;

	switch(ntohs(hdr->magic)) {
		case PMNET_MSG_STATUS_MAGIC:
			goto out; 
		case PMNET_MSG_KEEP_REQ_MAGIC:
			pmnet_sendpage(sockfd, pmnet_keep_resp,
				       sizeof(*pmnet_keep_resp));
			goto out;
		case PMNET_MSG_KEEP_RESP_MAGIC:
			goto out;
		case PMNET_MSG_MAGIC:
			break;
		default:
			log<LOG_ERROR>(L"bad magic");
			ret = -EINVAL;
			goto out;
			break;
	}

	switch(ntohs(hdr->msg_type)) {
		case PMNET_MSG_PUTPAGE: {
//			log<LOG_INFO>(L"CLIENT-->SERVER: PMNET_MSG_PUTPAGE");
			/* TODO: 4byte key and index should be change on demand */
			key = pmnet_long_key(ntohl(hdr->key), ntohl(hdr->index));
//			printf("GET PAGE FROM CLIENT (key=%lx, index=%lx, longkey=%lx)", ntohl(hdr->key), ntohl(hdr->index), key);

//			ret = pmnet_send_message(sockfd, PMNET_MSG_SUCCESS, 0, 
//				NULL, 0);

			/* copy page from message to local memory */
//			from_va = msg_in->page;

			/* Insert received page into hash */
			hashTable->Insert(key,hashTable->save_page((char *)msg_in->page).oid.off);
//			printf("[ Inserted %lx : ", key);
//			printf("%lx ]\n", hashTable->Get(key));
			break;
		}

		case PMNET_MSG_GETPAGE:{
			printf("CLIENT-->SERVER: PMNET_MSG_GETPAGE\n");

			/* alloc new page pointer to send */
			saved_page = calloc(1, PAGE_SIZE);

			/* key */
			key = pmnet_long_key(ntohl(hdr->key), ntohl(hdr->index));

			/* Get page from Hash and copy to saved_page */
			ret = hashTable->load_page(hashTable->Get(key), (char *)saved_page, 4096);

			if (ret != 0) {
				/* page not exists */
				memset(&reply, 0, PAGE_SIZE);
				ret = pmnet_send_message(sockfd, PMNET_MSG_NOTEXIST, ntohl(hdr->key), ntohl(hdr->index),
					reply, PAGE_SIZE);
				printf("PAGE NOT EXIST (key=%lx, index=%lx, longkey=%lx)\n", ntohl(hdr->key), ntohl(hdr->index), key);
//				printf("SERVER-->CLIENT: PMNET_MSG_NOTEXIST(%d)\n",ret);
			} else {
				/* page exists */
//				printf("SEND PAGE with long key=%lx\n", key);
				ret = pmnet_send_message(sockfd, PMNET_MSG_SENDPAGE, ntohl(hdr->key), ntohl(hdr->index), 
					saved_page, PAGE_SIZE);
				printf("[ Retrived (key=%lx, index=%lx, longkey=%lx) ", ntohl(hdr->key), ntohl(hdr->index), key);
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

#if 0
	spin_lock(&nn->nn_lock);
	/* set valid and queue the idle timers only if it hasn't been
	 * shut down already */
	if (nn->nn_sc == sc) {
		pmnet_sc_reset_idle_timer(sc);
		atomic_set(&nn->nn_timeout, 0);
		pmnet_set_nn_state(nn, sc, 1, 0);
	}
	spin_unlock(&nn->nn_lock);
#endif

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
		if (new_queue.push(msg_in)) {
			putcnt++;
//			log<LOG_DEBUG>(L"PRODUCER queue.push() cnt=%1%") % (int)putcnt;
			pushed = true;
			ret = 1;
		}
		pushed = true;
#if 0
		putcnt++;
		log<LOG_DEBUG>(L"PRODUCER queue.push() cnt=%1%") % (int)putcnt;
		pushed = true;
		ret = 1;
		free(msg_in);
#endif
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

	pmnet_keep_req = (pmnet_msg *)calloc(1, sizeof(struct pmnet_msg));
	pmnet_keep_resp = (pmnet_msg *)calloc(1, sizeof(struct pmnet_msg));
	if (!pmnet_keep_req || !pmnet_keep_resp)
		return;

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
	std::vector<std::thread> threads;
	boost::thread_group producer_threads, consumer_threads;

	/*
	 * Loop and accept client.
	 * create thread for each client.
	 */
	while (1) {
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

		/* start thread */
		boost::thread p = boost::thread( producer, sc );
		p.detach();
		boost::thread c1 = boost::thread( consumer, sc );
		c1.detach();
		boost::thread c2 = boost::thread( consumer, sc );
		c2.detach();
		boost::thread c3 = boost::thread( consumer, sc );
		c3.detach();
		boost::thread c4 = boost::thread( consumer, sc );
		c4.detach();
		boost::thread c5 = boost::thread( consumer, sc );
		c5.detach();
		boost::thread c6 = boost::thread( consumer, sc );
		c6.detach();
//		producer_threads.create_thread( producer, connfd );
//		consumer_threads.create_thread( consumer, connfd );
	} 

	close(sockfd); 
}

/*
 * Initialize CCEH
 * return: CCEH
 */
CCEH *init_cceh(char* file)
{
	CCEH* ht = new CCEH(file);
	log<LOG_INFO>(L"CCEH create...");

	return ht;
}


int main(int argc, char* argv[]) 
{ 	
	struct timespec start, end;
	uint64_t elapsed;

	clock_gettime(CLOCK_MONOTONIC, &start);

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
