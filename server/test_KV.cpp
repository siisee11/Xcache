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

#define POOL_SIZE (10737418240) // 10GB

using namespace std;

size_t initialTableSize = 32*1024;
size_t numData = 0;
size_t numKVThreads = 0;
size_t numNetworkThreads = 0;
size_t numPollThreads = 0;
bool numa_on = false;
bool verbose_flag = false;
bool bf_flag = false;
bool human = false;
struct bitmask *netcpubuf;
struct bitmask *kvcpubuf;
struct bitmask *pollcpubuf;

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
	printf("./bin/kv --pm_path /jy/1 --dataset <text file> --nr_data 10000000 -W 0-3 -K 4-7,14-17 -P 8-9,18-19 --tablesize 32768 --verbose\n");
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
	char *pm_path;

	const char *short_options = "vbut:n:d:z:hK:P:W:";
	static struct option long_options[] =
	{
		// --verbose 옵션을 만나면 "verbose_flag = 1"이 세팅된다.
		{"verbose", 0, NULL, 'v'},
		{"bloomfilter", 0, NULL, 'b'},
		{"tablesize", 1, NULL, 't'},
		{"dataset", 1, NULL, 'd'},
		{"pm_path", 1, NULL, 'z'},
		{"nr_data", 1, NULL, 'n'},
		{"netcpubind", 1, NULL, 'W'},
		{"kvcpubind", 1, NULL, 'K'},
		{"pollcpubind", 1, NULL, 'P'},
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

	struct timespec i_start, i_end, g_start, g_end;
	uint64_t i_elapsed, g_elapsed, m_elapsed;

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
	CountingBloomFilter<Key_t>* bf = new CountingBloomFilter<Key_t>(2,100000);

	auto totalSize = 1073741824; // 10GiB
	kv = new KV( totalSize / 4096, bf );
	dprintf("[  OK  ] KVStore Initialized\n");

	uint64_t* keys = (uint64_t*)malloc(sizeof(uint64_t)*numData);

	ifstream ifs;
	ifs.open(data_path);
	if(!ifs){
		cerr << "no file" << endl;
		return 0;
	}

	dprintf("[ INFO ] %s is used\n", data_path);
	for(unsigned int i=0; i<numData; i++){
		ifs >> keys[i];
	}
	dprintf("[  OK  ] Completed reading dataset\n");


	vector<thread> insertingThreads;
	vector<thread> searchingThreads;
	vector<thread> mixThreads;
	vector<int> failed(numNetworkThreads);
	vector<Key_t> notfoundKeys[numNetworkThreads];

	auto insert = [&kv, &keys](int from, int to){
		for(int i=from; i<to; i++){
			kv->Insert(keys[i], reinterpret_cast<Value_t>(keys[i]));
		}
	};

	auto search = [&kv, &keys, &failed, &notfoundKeys](int from, int to, int tid){
		int fail = 0;
		for(int i = from; i < to; i++){
			auto ret = kv->Get(keys[i]);
			if(ret != reinterpret_cast<Value_t>(keys[i])){
				fail++;
				notfoundKeys[tid].push_back(keys[i]);
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
				insertingThreads.emplace_back(thread(insert, chunk*t_id, chunk*(t_id+1)));
			else
				insertingThreads.emplace_back(thread(insert, chunk*t_id, numData));

			cpu_set_t cpuset;
			CPU_ZERO(&cpuset);
			CPU_SET(cpu_id, &cpuset);
			int rc = pthread_setaffinity_np(insertingThreads[t_id].native_handle(),
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

	for(auto& t: insertingThreads) t.join();
	dprintf("[ INFO ] insertingThreads all joined\n");

	clock_gettime(CLOCK_MONOTONIC, &i_end);
	i_elapsed = i_end.tv_nsec - i_start.tv_nsec + (i_end.tv_sec - i_start.tv_sec)*1000000000;
	if (human) printf("Insertion: %.3f usec/req \t %.3f ops/sec\n", i_elapsed/1000.0/numData , (numData/(i_elapsed/1000000000.0)));

//	clear_cache();
	dprintf("[ INFO ] Start Search\n");
	clock_gettime(CLOCK_MONOTONIC, &g_start);

	/* Equally distribute NetworkThread */
	t_id = 0;
	cpu_id = 0;
	while(true) {
		if (numa_bitmask_isbitset(netcpubuf, cpu_id)) {
			if(t_id != numNetworkThreads-1) {
				searchingThreads.emplace_back(thread(search, chunk * t_id, chunk * (t_id+1), t_id));
			}
			else {
				searchingThreads.emplace_back(thread(search, chunk*t_id, numData, t_id));
			}

			cpu_set_t cpuset;
			CPU_ZERO(&cpuset);
			CPU_SET(cpu_id, &cpuset);
			int rc = pthread_setaffinity_np(searchingThreads[t_id].native_handle(),
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

	for(auto& t: searchingThreads) t.join();

	clock_gettime(CLOCK_MONOTONIC, &g_end);
	g_elapsed = g_end.tv_nsec - g_start.tv_nsec + (g_end.tv_sec - g_start.tv_sec)*1000000000;
	if (human) printf("Search: %.3f usec/req \t %.3f ops/sec\n", g_elapsed/1000.0/numData , (numData/(g_elapsed/1000000000.0)));

	int failedSearch = 0;
	for(auto& v: failed) failedSearch += v;

	if (human) cout << failedSearch << " failedSearch" << endl;

	vector<Key_t> notFoundKeys;
	for(int i=0; i<numNetworkThreads; i++){
		for(auto& k: notfoundKeys[i]){
			notFoundKeys.push_back(k);
		}
	}

#if 0
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
	} else {
		printf("%lu, %lu, %lu, %lu, %.3f, %.3f, %.3f, %.3f",  
				numData, numNetworkThreads, numKVThreads, numPollThreads, 
				i_elapsed/1000.0/numData , (numData/(i_elapsed/1000000000.0))/1000.0, 
				g_elapsed/1000.0/numData , (numData/(g_elapsed/1000000000.0))/1000.0);
		kv->PrintStats();
	}

	delete(kv);

	return 0;
}

