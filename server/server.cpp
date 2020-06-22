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
#include "util/pair.h"
#include "src/CCEH.h"

#include "tcp.h"
#include "tcp_internal.h"

#define  BUFF_SIZE   1024
#define SA struct sockaddr 

class Buffer;

static void pmnet_rx_until_empty(int sockfd, Buffer&);
static int pmnet_process_message(int sockfd, struct pmnet_msg *hdr, struct pmnet_msg_in *msg_in);

using std::deque;
std::mutex cout_mu;
std::condition_variable cond;

char client_message[2000];
char buffer[1024];
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

struct msg_with_page
{
	struct pmnet_msg msg;
	char *page;
};


struct pmnet_msg_in {
	struct pmnet_msg *hdr;
	void *page;
	size_t page_off;
};


/* CCEH hashTable */
CCEH* hashTable;

class Buffer
{
public:
    void add(struct pmnet_msg_in *msg) {
        while (true) {
			printf("[Buffer] producer (msg key=%lx, index=%lx)\n", msg->hdr->key, msg->hdr->index);
            std::unique_lock<std::mutex> locker(mu);
            cond.wait(locker, [this](){return buffer_.size() < size_;});
            buffer_.push_back(msg);
            locker.unlock();
            cond.notify_all();
            return;
        }
    }
    struct pmnet_msg_in *remove() {
        while (true)
        {
			printf("[Buffer] consumer \n");
            std::unique_lock<std::mutex> locker(mu);
            cond.wait(locker, [this](){return buffer_.size() > 0;});
            struct pmnet_msg_in* back = buffer_.back();
            buffer_.pop_back();
            locker.unlock();
            cond.notify_all();
            return back;
        }
    }
    Buffer() {}
private:
   // Add them as member variables here
    std::mutex mu;
    std::condition_variable cond;

   // Your normal variables here
    deque<pmnet_msg_in *> buffer_;
    const unsigned int size_ = 100;
};

class Producer
{
private:
    Buffer &buffer_;
	int client_socket;

public:
    Producer(Buffer& buffer, int sockfd)
		: buffer_(buffer), client_socket(sockfd)
    {}

	void run() {
		std::thread::id this_id = std::this_thread::get_id(); 
		printf("[ new PRODUCER %lx Running... ]\n", this_id);

		/* Function read bytes from connfd */
		pmnet_rx_until_empty(client_socket, buffer_); 
		printf("[ PRODUCER %lx Exit ]\n", this_id);
		close( client_socket);
    }


};

class Consumer
{
    Buffer &buffer_;
	int client_socket;

public:
    Consumer(Buffer& buffer, int sockfd)
		: buffer_(buffer), client_socket(sockfd)
	{}

    void run() {
		int ret;

		std::thread::id this_id = std::this_thread::get_id(); 
		printf("[ new CONSUMER %lx Running... ]\n", this_id);

		/* consume request from queue */
        while (true) {
            struct pmnet_msg_in *msg_in = buffer_.remove();
			printf("CONSUMER buffer_.remove()\n");

			ret = pmnet_process_message(client_socket, msg_in->hdr, msg_in);
			free(msg_in);

			if (ret == 0)
				ret = 1;
        }
    }
};

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
			printf("bad magic\n");
			ret = -EINVAL;
			goto out;
			break;
	}

	switch(ntohs(hdr->msg_type)) {
		case PMNET_MSG_HOLA: {
			printf("CLIENT-->SERVER: PMNET_MSG_HOLA\n");

			/* send hello message */
			memset(&reply, 0, 1024);

			ret = pmnet_send_message(sockfd, PMNET_MSG_HOLASI, 0, 
				reply, 1024);

			printf("SERVER-->CLIENT: PMNET_MSG_HOLASI(%d)\n", ret);
			break;
		}

		case PMNET_MSG_HOLASI: {
			printf("SERVER-->CLIENT: PMNET_MSG_HOLASI\n");
			break;
		}

		case PMNET_MSG_PUTPAGE: {
			printf("CLIENT-->SERVER: PMNET_MSG_PUTPAGE\n");
			/* TODO: 4byte key and index should be change on demand */
			key = pmnet_long_key(ntohl(hdr->key), ntohl(hdr->index));
			printf("GET PAGE FROM CLIENT (key=%lx, index=%lx, longkey=%lx)\n", ntohl(hdr->key), ntohl(hdr->index), key);

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
static int pmnet_advance_rx(int sockfd, struct pmnet_msg_in *msg_in, Buffer& buffer_)
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

	printf("at page_off %zu, datalen=%u\n", msg_in->page_off, ntohs(hdr->data_len));

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
		buffer_.add(msg_in);
		ret = 1;
//		ret = pmnet_process_message(sockfd, hdr, msg_in);
//		if (ret == 0)
//			ret = 1;
		msg_in->page_off = 0;
	}

out:
	return ret;
}

static void pmnet_rx_until_empty(int sockfd, Buffer& _buffer)
{
	int ret = 1;
	struct pmnet_msg_in *msg_in; // structure for message processing
	do {
		/* prepare new msg */
		if (ret == 1)
			msg_in = init_msg();
		ret = pmnet_advance_rx(sockfd, msg_in, _buffer);
	} while (ret > 0);

	if (ret <= 0 && ret != -EAGAIN) {
		printf("pmnet_rx_until_empty: saw error %d, closing\n", ret);
		/* not permanent so read failed handshake can retry */
	}
}


/*
 * *sockfd : listen socket fd
 * *connfd : fd of connected socket
 * *shared_buf : shared buffer for Producer and Consumer
 */
void init_network_server(int *sockfd, int *connfd, Buffer& shared_buf)
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

	/*
	 * Loop and accept client.
	 * create thread for each client.
	 */
	while (1) {
		Buffer new_buf;		// Shared buffer

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
		Producer p(new_buf, *connfd);
		std::thread	produce_thread = std::thread(&Producer::run, &p);
//		threads.push_back(produce_thread);
		produce_thread.detach();

		/* Consumer start */
		Consumer c(new_buf, *connfd);
		std::thread	consume_thread = std::thread(&Consumer::run, &c);
		consume_thread.detach();

		/* TODO: join threads */
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
	printf("CCEH creation");fflush(stdout);

	return ht;
}


int main(int argc, char* argv[]) 
{ 	
	int sockfd, connfd;
	struct timespec start, end;
	uint64_t elapsed;
    Buffer shared_buf;		// Shared buffer

	clock_gettime(CLOCK_MONOTONIC, &start);

	/* New CCEH hash table */
	hashTable = init_cceh(argv[1]);


	/* 
	 * Listen and accept socket 
	 * Looping inside this fuction
	 */
	init_network_server(&sockfd, &connfd, shared_buf);

	close(sockfd); 

	return 0;
} 
