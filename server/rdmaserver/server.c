#include "server.h"
#include <getopt.h>
#include <fcntl.h>

static void usage(const char* argv){
    printf("Usage\n");
    printf("\tStart a server and wait for connection: %s\n", argv);
    printf("\nOptions:\n");
    printf("\t-t --tcp_port=<port> (required) use <port> to listen tcp connection\n");
    printf("\t-i --ib_port=<port> (required) use <port> of infiniband device (default=1)\n");
}

int tcp_port = -1;
int ib_port = 1;

int main(int argc, char* argv[]){
    char hostname[64];
    static struct option options[] = {
	{"ib_port", 1, 0, 'i'},
	{"tcp_port", 1, 0, 'i'},
	{NULL, 0, NULL, 0}
    };

    while(1){
	int c = getopt_long(argc, argv, "i:t:", options, NULL);
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
	    default:
		usage(argv[0]);
		return 0;
	}
    }

    gethostname(hostname, 64);
    printf("Hostname:\t %s\n", hostname);
    printf("IB port:\t %d\n", ib_port);
    printf("TCP port:\t %d\n",tcp_port);

    init_server();


//    test();
    return 0;
}

