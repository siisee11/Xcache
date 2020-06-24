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


using std::deque;
using namespace boost::lockfree;
std::mutex cout_mu;
std::condition_variable cond;

struct pmnet_msg_in {
	int sockfd;
	struct pmnet_msg *hdr;
	void *page;
	size_t page_off;
};

/* lock-free-queue */
boost::lockfree::queue<pmnet_msg_in *> new_queue(128);
boost::atomic<bool> done (false);
boost::atomic<int> putcnt (0);
boost::atomic<int> getcnt (0);


/* CCEH hashTable */
CCEH* hashTable;

static void pmnet_rx_until_empty(int sockfd);
static int pmnet_process_message(int sockfd, struct pmnet_msg *hdr, struct pmnet_msg_in *msg_in);

void producer(int client_socket) {
	std::thread::id this_id = std::this_thread::get_id(); 
	printf("[ new PRODUCER %lx Running... ]\n", this_id);

	/* Function read bytes from connfd */
	pmnet_rx_until_empty(client_socket); 
	printf("[ PRODUCER %lx Exit ]\n", this_id);
	close( client_socket);
	done = true;
}

void consumer(int client_socket) {
	int ret;
	struct pmnet_msg_in *msg_in;

	std::thread::id this_id = std::this_thread::get_id(); 
	printf("[ new CONSUMER %lx Running... ]\n", this_id);

	/* consume request from queue */
	while (!done) {
		while (new_queue.pop(msg_in)){
			getcnt++;
			log<LOG_DEBUG>(L"CONSUMER queue.pop() cnt=%1%") % (int)getcnt;
			ret = pmnet_process_message(client_socket, msg_in->hdr, msg_in);
			free(msg_in);
		}
	}

	while (new_queue.pop(msg_in)) {
		printf("CONSUMER buffer_.remove()\n");
		ret = pmnet_process_message(client_socket, msg_in->hdr, msg_in);
	}
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
		uint16_t msg_type, uint32_t key)
{
	memset(msg, 0, sizeof(struct pmnet_msg));
	msg->magic = htons(PMNET_MSG_MAGIC);
	msg->data_len = htons(data_len);
	msg->msg_type = htons(msg_type);
//	msg->sys_status = htonl(PMNET_ERR_NONE);
	msg->sys_status = htonl(0);
	msg->status = 0;
	msg->key = htonl(key);
}

/* send to client */
int pmnet_send_message(int sockfd, uint32_t msg_type, uint32_t key, 
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

	pmnet_init_msg(msg, datalen, msg_type, key); 

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
		case PMNET_MSG_HOLA: {
			log<LOG_INFO>(L"CLIENT-->SERVER: PMNET_MSG_HOLA");

			/* send hello message */
			memset(&reply, 0, 1024);

			ret = pmnet_send_message(sockfd, PMNET_MSG_HOLASI, 0, 
				reply, 1024);

			log<LOG_INFO>(L"SERVER-->CLIENT: PMNET_MSG_HOLASI(%1%)") % ret;
			break;
		}

		case PMNET_MSG_HOLASI: {
			log<LOG_INFO>(L"SERVER-->CLIENT: PMNET_MSG_HOLASI");
			break;
		}

		case PMNET_MSG_PUTPAGE: {
			log<LOG_INFO>(L"CLIENT-->SERVER: PMNET_MSG_PUTPAGE");
			/* TODO: 4byte key and index should be change on demand */
			key = pmnet_long_key(ntohl(hdr->key), ntohl(hdr->index));
			printf("GET PAGE FROM CLIENT (key=%lx, index=%lx, longkey=%lx)", ntohl(hdr->key), ntohl(hdr->index), key);

			/* copy page from message to local memory */
			from_va = msg_in->page;

			/* Insert received page into hash */
			hashTable->Insert(key,hashTable->save_page((char *)msg_in->page).oid.off);
			printf("[ Inserted %lx : ", key);
			printf("%lx ]\n", hashTable->Get(key));
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
				ret = pmnet_send_message(sockfd, PMNET_MSG_NOTEXIST, 0, 
					reply, PAGE_SIZE);
				printf("PAGE NOT EXIST (key=%lx, index=%lx, longkey=%lx)\n", ntohl(hdr->key), ntohl(hdr->index), key);
				printf("SERVER-->CLIENT: PMNET_MSG_NOTEXIST(%d)\n",ret);
			} else {
				/* page exists */
				printf("SEND PAGE with long key=%lx\n", key);
				ret = pmnet_send_message(sockfd, PMNET_MSG_SENDPAGE, 0, 
					saved_page, PAGE_SIZE);
				printf("[ Retrived (key=%lx, index=%lx, longkey=%lx) \n", ntohl(hdr->key), ntohl(hdr->index), key);
				printf("%lx ]\n", hashTable->Get(key));
				printf("SERVER-->CLIENT: PMNET_MSG_SENDPAGE(%d)\n",ret);
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


/* 
 * Read from socket
 */
static int pmnet_advance_rx(int sockfd, struct pmnet_msg_in *msg_in, bool& pushed)
{
	struct pmnet_msg *hdr;
	int ret = 0;
	void *data;
	size_t datalen;

	/* read header */
	if (msg_in->page_off < sizeof(struct pmnet_msg)) {
		data = msg_in->hdr + msg_in->page_off;
		datalen = sizeof(struct pmnet_msg) - msg_in->page_off;
		ret = read(sockfd, data, datalen);

		if (ret > 0) {
			msg_in->page_off += ret;
			if (msg_in->page_off == sizeof(struct pmnet_msg)) {
				hdr = msg_in->hdr;
				if (ntohs(hdr->data_len) > PMNET_MAX_PAYLOAD_BYTES)
					ret = -EOVERFLOW;
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
		ret = read(sockfd, data, datalen);
		if (ret > 0)
			msg_in->page_off += ret;
		if (ret <= 0)
			goto out;
	}

	if (msg_in->page_off - sizeof(struct pmnet_msg) == ntohs(hdr->data_len)) {
		/* we can only get here once, the first time we read
		 * the payload.. so set ret to progress if the handler
		 * works out. after calling this the message is toast */
		if (new_queue.push(msg_in)) {
			putcnt++;
			log<LOG_DEBUG>(L"PRODUCER queue.push() cnt=%1%") % (int)putcnt;
			pushed = true;
			ret = 1;
		}
	}

out:
	return ret;
}

static void pmnet_rx_until_empty(int sockfd)
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
		ret = pmnet_advance_rx(sockfd, msg_in, pushed);
	} while (ret > 0);

	if (ret <= 0 && ret !=-EAGAIN) {
		printf("pmnet_rx_until_empty: saw error %d, closing\n", ret);
		/* not permanent so read failed handshake can retry */
	}
}


/*
 * *sockfd : listen socket fd
 * *connfd : fd of connected socket
 * *shared_buf : shared buffer for Producer and Consumer
 */
void init_network_server(int *sockfd, int *connfd)
{
	socklen_t len; 
	struct sockaddr_in servaddr, cli; 

//	init_msg();

	// socket create and verification 
	*sockfd = socket(AF_INET, SOCK_STREAM, 0); 
	if (*sockfd == -1) { 
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
	if ((bind(*sockfd, (SA*)&servaddr, sizeof(servaddr))) != 0) { 
		printf("socket bind failed...\n"); 
		exit(0); 
	} 
	else
		printf("Socket successfully binded..\n"); 

	// Now server is ready to listen and verification 
	if ((listen(*sockfd, 5)) != 0) { 
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
		*connfd = accept(*sockfd, (SA*)&cli, &len); 
		if (*connfd < 0) { 
			printf("server acccept failed...\n"); 
			exit(0); 
		} 
		else
			printf("server acccept the client...\n"); 

		/* start thread */
		boost::thread p = boost::thread( producer, *connfd );
		p.detach();
		boost::thread c = boost::thread( consumer, *connfd );
		c.detach();
//		producer_threads.create_thread( producer, *connfd );
//		consumer_threads.create_thread( consumer, *connfd );
	} 
}

/*
 * Initialize CCEH
 * return: CCEH
 */
CCEH *init_cceh(char* file)
{
	printf("%s\n", file);
	CCEH* ht = new CCEH(file);
	log<LOG_INFO>(L"CCEH create...");

	return ht;
}


int main(int argc, char* argv[]) 
{ 	
	int sockfd, connfd;
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
	init_network_server(&sockfd, &connfd);

	close(sockfd); 

	return 0;
} 
