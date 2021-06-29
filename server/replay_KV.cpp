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

#include "KV.h"
#include "variables.h"

#define ROP 1
#define WOP 2

using namespace std;


typedef vector<string> event_t;

#define SEQUENCE(t)     ((loff_t) stoull((t)[0]))
#define TIMESTAMP(t)    (t)[1]
#define OP(t)           (t)[2]
#define __INODE(t)      (t)[3]
#define INODE(t)        stoull(__INODE(t))
#define INODE_SIZE(t)   ((loff_t) stoull((t)[4]))
#define OFFSET(t)       ((loff_t) stoull((t)[5]))
#define SIZE(t)         ((size_t) stoull((t)[6]))

#define PAGE_SIZE                       (4096)

#define IS_OPEN(e)      (OP(e)[0] == 'O')
#define IS_CLOSE(e)     (OP(e)[0] == 'C')

enum { OPEN, CLOSE, READ, WRITE, FSYNC, FDATASYNC, FALLOCATE };
enum { ST_READ, ST_WRITE };

size_t initialTableSize = 0;
size_t numData = 0;
size_t numKVThreads = 0;
size_t numNetworkThreads = 0;
size_t numPollThreads = 0;
bool numa_on = false;
bool verbose_flag = false;
bool bf_flag = false;
bool human = false;
struct bitmask *netcpubuf;

static void dprintf( const char* format, ... ) {
	if (verbose_flag) {
		va_list args;
		va_start( args, format );
		vprintf( format, args );
		va_end( args );
	}
}

static void usage(){
	printf("Usage : \n");
	printf("./bin/kv --dataset <text file> --nr_data 10000000 -W 0-3 -K 4-7,14-17 -P 8-9,18-19 --tablesize 32768 --verbose\n");
}

void clear_cache(){
	int* dummy = new int[1024*1024*256];
	for(int i=0; i<1024*1024*256; i++){
		dummy[i] = i;
	}

	for(int i=100; i<1024*1024*256-100; i++){
		dummy[i] = dummy[i-rand()%100] + dummy[i+rand()%100];
	}

	delete[] dummy;
}

static void printCpuBuf(size_t nr_cpus, struct bitmask *bm, const char *str) {
	/* Print User specified cpus */
	dprintf("[ INFO ] %s\t threads: \t", str);
	for ( unsigned int i = 0; i< nr_cpus; i++) {
		if (i % 4 == 0)
			dprintf(" ");
		if (numa_bitmask_isbitset(bm, i))
			dprintf("1");
		else
			dprintf("0");
	}
	dprintf("\n");
}

int main(int argc, char* argv[]){
	char *data_path;

	const char *short_options = "vbut:n:d:z:hK:P:W:";
	static struct option long_options[] =
	{
		// --verbose 옵션을 만나면 "verbose_flag = 1"이 세팅된다.
		{"verbose", 0, NULL, 'v'},
		{"bloomfilter", 0, NULL, 'b'},
		{"tablesize", 1, NULL, 't'},
		{"dataset", 1, NULL, 'd'},
		{"nr_data", 1, NULL, 'n'},
		{"netcpubind", 1, NULL, 'W'},
		{"numa", 0, NULL, 'u'},
		{0, 0, 0, 0} 
	};

	while(1){
		int c = getopt_long(argc, argv, short_options, long_options, NULL);
		if(c == -1) break;
		switch(c){
			case 'n':
				numData = strtol(optarg, NULL, 0);
				if(numData <= 0){
					usage();
					return 0;
				}
				break;
			case 't':
				initialTableSize = strtol(optarg, NULL, 0);
				if(initialTableSize <= 0){
					usage();
					return 0;
				}
				break;
			case 'd':
				data_path = strdup(optarg);
				break;
			case 'W':
				netcpubuf = numa_parse_cpustring(optarg);
				if (!netcpubuf) {
					printf ("<%s> is invalid\n", optarg);
					usage();
				}
				break;
			case 'v':
				verbose_flag = true;
				break;
			case 'b':
				bf_flag = true;
				break;
			case 'h':
				human = true;
				break;
			case 'u':
				numa_on = true;
				break;
			default:
				usage();
				return 0;
		}
	}

	struct timespec i_start, i_end;
	uint64_t i_elapsed, g_elapsed;

	dprintf("START MAIN FUNCTION\n");

	/* GET NUMA and CPU information */
	auto nr_cpus = std::thread::hardware_concurrency();
	dprintf("[ INFO ] NR_CPUS= %d\n", nr_cpus);

	/* count number of threads */
	for (unsigned int i = 0; i < nr_cpus ; i++) {
		if (numa_bitmask_isbitset(netcpubuf, i))
			numNetworkThreads++;	
	}

	/* Print User specified cpu binding */
	printCpuBuf(nr_cpus, netcpubuf, "net");

	/* Create KV store */
	KVStore* kv = NULL;
	CountingBloomFilter<Key_t>* bf = NULL;
	if (bf_flag) {
		bf = new CountingBloomFilter<Key_t>(2,1000000);
		dprintf("[  OK  ] BF Initialized\n");
	}

	auto totalSize = 10737418240 * 10 ; // 10GiB

	if (initialTableSize == 0) 
		initialTableSize = totalSize / 4096;

	dprintf("[ INFO ] Hash Table Size : %lu\n", initialTableSize);

	kv = new KV( initialTableSize, NULL);
	dprintf("[  OK  ] KVStore Initialized\n");

	vector<uint64_t> keys;
	vector<int> ops;

	ifstream ifs(data_path, ifstream::in);
    if (!ifs.is_open()) {
        cerr << "can't open " << argv[1] << ": " << strerror(errno) << endl;
        return errno;
    }

	dprintf("[ INFO ] %s is used\n", data_path);

	atomic<uint64_t> done(0);

	int op, error;
	uint64_t key;
	uint64_t batch;
    string event_line;
    while (getline(ifs, event_line)) { /* parse all lines */
        string buf;
        stringstream ss(event_line);
        event_t e;

        error = 0;

        while (ss >> buf) { e.push_back(buf); }

		key = INODE(e);
		key <<= 32;
		key += OFFSET(e);

        switch (OP(e)[0]) {
            case 'W':
				op = 2;
				batch = SIZE(e) / 4096 + (SIZE(e) % 4096 ? 1 : 0);
                break;
            case 'R':
				op = 1;
				batch = SIZE(e) / 4096 + (SIZE(e) % 4096 ? 1 : 0);
//				cout << OP(e) << " " << key << " " << SIZE(e) << " " << batch << endl;
                break;
            default:
				batch = 0;
				break;
        }

		// processing batch
		for ( unsigned int b = 0 ; b < batch ; b++) {
			ops.push_back(op);
			keys.push_back(key + PAGE_SIZE * b);
//			cout << "push " << op << " " << key + PAGE_SIZE * b << endl;
		}
		done += batch;

		if ( done >= numData )
			break;
	}

//	for (unsigned int i = 0; i< numData; i++) {
//		cout << ops[i] << " " << keys[i] << endl;
//	}

	dprintf("[  OK  ] Completed reading dataset\n");

	vector<thread> goThreads;
	vector<thread> searchingThreads;
	vector<int> failed(numNetworkThreads);
	vector<Key_t> notfoundKeys[numNetworkThreads];

	auto goroutine = [&kv, &ops, &keys, &failed, &notfoundKeys](int from, int to, int tid){
		int fail = 0;
		for(int i = from; i < to; i++){
			if (ops[i] == 2)
				kv->Insert(keys[i], reinterpret_cast<Value_t>(keys[i]));
			else {
				auto ret = kv->Get(keys[i]);
				if(ret != reinterpret_cast<Value_t>(keys[i])){
					fail++;
					notfoundKeys[tid].push_back(keys[i]);
				}
			}
		}
		failed[tid] = fail;
	};

//	if (human) printf("NumData: %lu, NetworkT: %lu, numKVThreads: %lu, PollT %lu\n", numData, numNetworkThreads, numKVThreads, numPollThreads);
//	clear_cache();
	const size_t chunk = numData/numNetworkThreads;

	dprintf("[ INFO ] Start Insertion\n");
	clock_gettime(CLOCK_MONOTONIC, &i_start);

	/* Equally distribute NetworkThread */
	unsigned t_id = 0;
	size_t cpu_id = 0;
	while(true) {
		if (numa_bitmask_isbitset(netcpubuf, cpu_id)) {
			if(t_id != numNetworkThreads-1)
				goThreads.emplace_back(thread(goroutine, chunk*t_id, chunk*(t_id+1), t_id));
			else
				goThreads.emplace_back(thread(goroutine, chunk*t_id, numData, t_id));

			cpu_set_t cpuset;
			CPU_ZERO(&cpuset);
			CPU_SET(cpu_id, &cpuset);
			int rc = pthread_setaffinity_np(goThreads[t_id].native_handle(),
					sizeof(cpu_set_t), &cpuset);
			if (rc != 0) {
				std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
			}
//			dprintf("NewtorkT [%d/%d] bind on CPU %d\n", t_id, numNetworkThreads, cpu_id);

			t_id++;
		}
		cpu_id++;

		if ( cpu_id == nr_cpus )
			break;
	}

	for(auto& t: goThreads) t.join();
	dprintf("[ INFO ] goThreads all joined\n");

	clock_gettime(CLOCK_MONOTONIC, &i_end);
	i_elapsed = i_end.tv_nsec - i_start.tv_nsec + (i_end.tv_sec - i_start.tv_sec)*1000000000;
	if (human) printf("process: %.3f usec/req \t %.3f ops/sec\n", i_elapsed/1000.0/numData , (numData/(i_elapsed/1000000000.0)));

	int failedSearch = 0;
	for(auto& v: failed) failedSearch += v;

	if (human) cout << failedSearch << " failedSearch" << endl;

#if 0
	vector<Key_t> notFoundKeys;
	for(size_t i=0; i<numNetworkThreads; i++){
		for(auto& k: notfoundKeys[i]){
			notFoundKeys.push_back(k);
		}
	}

	for(auto& k: notFoundKeys){
		auto ret = kv->FindAnyway(k);
		if(ret == NONE){
			cout << "Key (" << k << ") does not exist" << endl;
		}
		else{
			cout << "Key (" << k << ") resides in different segment" << endl;
		}
	}
#endif

	if (human) {
		kv->PrintStats();
	} 

	return 0;
}

