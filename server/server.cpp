#include <getopt.h>
#include <fcntl.h>
#include <thread>
#include <atomic>

#include "server.h"

static void usage(const char* argv){
	printf("Usage\n");
	printf("\tStart a server and wait for connection: %s\n", argv);
	printf("\nOptions:\n");
	printf("\t-t --tcp_port=<port> (required) use <port> to listen tcp connection\n");
	printf("\t-i --ib_port=<port> (required) use <port> of infiniband device (default=1)\n");
	printf("\t-p --path=<port> (required) use <port> of infiniband device (default=1)\n");
}

int tcp_port = -1;
int ib_port = 1;
static int rdma_flag= 0;
static int verbose_flag;
char *path;

/* lock free queues */
queue_t **lfqs = NULL;
unsigned int nr_cpus;

std::atomic<bool> done(false);

/* counting valuse */
int putcnt = 0;
int getcnt = 0;
int sample_put_q_cnt = 0;
int sample_get_q_cnt = 0;
int sample_put_cnt = 0;
int sample_get_cnt = 0;

/* performance timer */
uint64_t network_elapsed=0, pmput_elapsed=0, pmget_elapsed=0;
uint64_t pmnet_rx_elapsed=0; 
uint64_t pmget_notexist_elapsed=0, pmget_exist_elapsed=0;
uint64_t pmput_queue_elapsed=0, pmget_queue_elapsed=0;


int destroy_lock_free_queue(){
	for (int i = 0; i < nr_cpus ; i++) {
		destroy_queue(lfqs[i]);
	}
}

int init_lock_free_queue(){
	lfqs = (queue_t**)malloc(nr_cpus * sizeof(queue_t*));
	for (int i = 0; i < nr_cpus; i++) {
		lfqs[i] = create_queue();
	}
}

int main(int argc, char* argv[]){
	char hostname[64];

	const char *short_options = "vt:i:p:r";
	static struct option long_options[] =
	{
		// --verbose 옵션을 만나면 "verbose_flag = 1"이 세팅된다.
		{"verbose", 0, NULL, 1},
		{"tcp_port", 1, NULL, 't'},
		{"ib_port", 1, NULL, 'i'},
		{"path", 1, NULL, 'p'},
		{"rdma", 0, NULL, 'r'},
		{0, 0, 0, 0} 
	};

	while(1){
		int c = getopt_long(argc, argv, short_options, long_options, NULL);
		if(c == -1) break;
		switch(c){
			case 'i':
				ib_port = strtol(optarg, NULL, 0);
				if(ib_port <= 0){
					usage(argv[0]);
					return 0;
				}
				break;
			case 't':
				tcp_port = strtol(optarg, NULL, 0);
				if(tcp_port <= 0){
					usage(argv[0]);
					return 0;
				}
				break;
			case 'p':
				path = strdup(optarg);
				break;
			case 'r':
				rdma_flag = 1;
				break;
			case 'v':
				verbose_flag = 1;
				break;
			default:
				usage(argv[0]);
				return 0;
		}
	}

	nr_cpus = std::thread::hardware_concurrency();

	gethostname(hostname, 64);
	printf("Hostname:\t %s\n", hostname);
	printf("IB port:\t %d\n", ib_port);
	printf("TCP port:\t %d\n",tcp_port);
	printf("Transport:\t %s\n", rdma_flag ? "RDMA" : "TCP/IP");
	printf("threads:\t %d CPUs\n", nr_cpus);

	init_lock_free_queue();

	if ( rdma_flag )
		init_rdma_server(path);
	else {
		signal(SIGINT, sigint_callback_handler);
		signal(SIGSEGV, sigsegv_callback_handler);
		init_tcp_server(path);
	}

	destroy_lock_free_queue();

	return 0;
}

