#include <stdio.h>
#include <stdarg.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <numa.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <ctime>

#include "src/NuMA_KV_PM.h"
#include "src/variables.h"

extern bool verbose_flag;
extern size_t numData;
extern struct bitmask *netcpubuf;
extern struct bitmask *kvcpubuf;
extern struct bitmask *pollcpubuf;

RequestPool* RequestPool::instance = nullptr;

using namespace std;

static void dprintf( const char* format, ... ) {
	if (verbose_flag) {
		va_list args;
		va_start( args, format );
		vprintf( format, args );
		va_end( args );
	}
}

NUMA_KV::NUMA_KV(PMEMobjpool **pop, bool recovery, size_t nThreads, size_t nPollThreads)
	: cceh{new CCEH(pop, recovery)}
{
	dprintf("[  OK  ] NUMA_KV \n");
}

NUMA_KV::NUMA_KV(PMEMobjpool **pop, size_t initCap, size_t nThreads, size_t nPollThreads)
	: cceh{new CCEH(pop, static_cast<size_t>(initCap))}
{
	int numable = numa_available();
	if (numable == 0)
		dprintf("[  OK  ] NUMA feature ON\n");
	else
		dprintf("[ FAIL ] NUMA feature OFF\n");


	perNodeQueue = (queue_t**)malloc(kNumNodes * sizeof(queue_t*));
	for (int i = 0; i < kNumNodes; i++) {
		perNodeQueue[i] = create_queue("perNodeQueue");
	}

	completionQueue = (queue_t**)malloc(kNumNodes * sizeof(queue_t *));
	for (int i = 0; i < kNumNodes; i++) {
		completionQueue[i] = create_queue("completionQueue");
	}
	dprintf("[  OK  ] Lock Free Queue INIT\n");

	for (unsigned i = 0; i < kNumNodes ; i++) {
		miss_cnt[i] = 0;
	}

#ifdef REQUESTPOOL
	/* create new instance of RequestPool */
	RequestPool::GetInstance();
#endif

	/* Worker function running on request queue */
	auto kvwork = [&] (int threadId) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		int cpu = sched_getcpu();
		int thisNode = numa_node_of_cpu(cpu);
		while (1) {
			struct work_request *wr = (struct work_request *)dequeue(perNodeQueue[thisNode]);
			auto key = wr->key;
			auto value = wr->value;
			auto type = wr->msg_type;

#ifdef KV_DEBUG
			auto start = wr->time;
			struct timespec end;
			clock_gettime(CLOCK_MONOTONIC, &end);
			uint64_t elapsed = end.tv_nsec - start.tv_nsec + (end.tv_sec - start.tv_sec)*1000000000;
			perNodeQueueTime += elapsed;
#endif
			
			if (type == MSG_INSERT) {
#ifdef KV_DEBUG
				struct timespec i_start;
				clock_gettime(CLOCK_MONOTONIC, &i_start);
#endif
				cceh->Insert(key, value);
#ifdef KV_DEBUG
				struct timespec i_end;
				clock_gettime(CLOCK_MONOTONIC, &i_end);
				insertTime += i_end.tv_nsec - i_start.tv_nsec + (i_end.tv_sec - i_start.tv_sec)*1000000000;
#endif
				enqueue(completionQueue[thisNode], wr);
			} else if(type == MSG_GET) {
#ifdef KV_DEBUG
				struct timespec g_start;
				clock_gettime(CLOCK_MONOTONIC, &g_start);
#endif
				auto ret =cceh->Get(key);
#ifdef KV_DEBUG
				struct timespec g_end;
				clock_gettime(CLOCK_MONOTONIC, &g_end);
				getTime += g_end.tv_nsec - g_start.tv_nsec + (g_end.tv_sec - g_start.tv_sec)*1000000000;
#endif
				wr->value = ret;
				enqueue(completionQueue[thisNode], wr);
			} else {
				printf("MSG_EMPTY dequeued!\n");
			}
		}
	};
	
	auto cq_poll = [&] () {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		int cpu = sched_getcpu();
		int thisNode = numa_node_of_cpu(cpu);
		while (true) {
			struct work_request *wr = (struct work_request *)dequeue(completionQueue[thisNode]);	
			auto key = wr->key;
			auto value = wr->value;
			auto type = wr->msg_type;

#ifdef REQUESTPOOL
			REQUESTPOOL::GetInstance()->free_wr(wr);
#else
			free(wr);
#endif
			wr = NULL;

			if (type == MSG_GET) {
				if ( value == NONE ) {
//				if(value != reinterpret_cast<Value_t>(key)){
					failedSearch++;
				}
			}

			nr_completed++;

			/* all work done */
			if (nr_completed == numData)
				done = true;
		}
	};

	/* kvThreads equally allocate on NUMA nodes */
	unsigned t_id = 0;
	int cpu_id = 0;
	while(true) {
		if (numa_bitmask_isbitset(kvcpubuf, cpu_id)) {
			numakvThreads.emplace_back(thread(kvwork, t_id));

			cpu_set_t cpuset;
			CPU_ZERO(&cpuset);
			CPU_SET(cpu_id, &cpuset);
			int rc = pthread_setaffinity_np(numakvThreads[t_id].native_handle(),
					sizeof(cpu_set_t), &cpuset);
			if (rc != 0) {
				std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
			}
//			dprintf("kvThread %d bind on CPU %d\n", t_id, cpu_id);

			numakvThreads[t_id].detach();

			t_id++;
		}
		cpu_id++;

		if ( t_id == nThreads )
			break;
	}

	/* cq_poller equally allocate on NUMA nodes */
	t_id = 0;
	cpu_id = 0;
	while(true) {
		if (numa_bitmask_isbitset(pollcpubuf, cpu_id)) {
			cq_pollers.emplace_back(thread(cq_poll));

			cpu_set_t cpuset;
			CPU_ZERO(&cpuset);
			CPU_SET(cpu_id, &cpuset);
			int rc = pthread_setaffinity_np(cq_pollers[t_id].native_handle(),
					sizeof(cpu_set_t), &cpuset);
			if (rc != 0) {
				std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
			}
//			dprintf("cq_poller %d bind on CPU %d\n", t_id, cpu_id);

			cq_pollers[t_id].detach();

			t_id++;
		}
		cpu_id++;

		if ( t_id == nPollThreads )
			break;
	}

	dprintf("[  OK  ] Thread init\n");
}

NUMA_KV::~NUMA_KV(void)
{ 
	for (int i = 0; i < kNumNodes; i++) {
		destroy_queue(perNodeQueue[i]);
	}
}

void NUMA_KV::Insert(Key_t& key, Value_t value, int unique_id, int thisNode) {
#ifdef NUMAQ
	auto node = cceh->GetNodeID(key);

	if( thisNode != node ){ 
		miss_cnt[thisNode]++;
#ifdef REQUESTPOOL
		struct work_request *wr = (struct work_request *)RequestPool::GetInstance()->get_wr();
#else
		struct work_request *wr = (struct work_request *)malloc(sizeof(struct work_request));
#endif /* REQUESTPOOL */
		wr->unique_id = unique_id;
		wr->msg_type = MSG_INSERT;
		wr->value = value;
		wr->key = key;

#ifdef KV_DEBUG
		clock_gettime(CLOCK_MONOTONIC, &wr->time);
#endif
		enqueue( perNodeQueue[node], wr );
	}
	else {
#ifdef KV_DEBUG
		struct timespec i_start;
		clock_gettime(CLOCK_MONOTONIC, &i_start);
#endif
		cceh->Insert(key, value);
#ifdef KV_DEBUG
		struct timespec i_end;
		clock_gettime(CLOCK_MONOTONIC, &i_end);
		insertTime += i_end.tv_nsec - i_start.tv_nsec + (i_end.tv_sec - i_start.tv_sec)*1000000000;
#endif
		nr_completed++;

		/* all work done */
		if (nr_completed == numData)
			done = true;
	}
#else /* NUMAQ */

#ifdef KV_DEBUG
	auto node = cceh->GetNodeID(key);

	if( thisNode != node )
		miss_cnt[thisNode]++;

	struct timespec i_start;
	clock_gettime(CLOCK_MONOTONIC, &i_start);
#endif
	cceh->Insert(key, value);
#ifdef KV_DEBUG
	struct timespec i_end;
	clock_gettime(CLOCK_MONOTONIC, &i_end);
	insertTime += i_end.tv_nsec - i_start.tv_nsec + (i_end.tv_sec - i_start.tv_sec)*1000000000;
#endif

#endif /* NUMAQ */
	
	return;
}

void NUMA_KV::Get(Key_t& key, int unique_id, int thisNode) {
#ifdef NUMAQ
	auto node = cceh->GetNodeID(key);

#ifdef REQUESTPOOL
	struct work_request *wr = (struct work_request *)RequestPool::GetInstance()->get_wr();
#else
	struct work_request *wr = (struct work_request *)malloc(sizeof(struct work_request));
#endif /* REQUESTPOOL */


	wr->unique_id = unique_id;
	wr->msg_type = MSG_GET;
	wr->key = key;

	if ( thisNode != node) { 
#ifdef KV_DEBUG
		clock_gettime(CLOCK_MONOTONIC, &wr->time);
#endif
		enqueue( perNodeQueue[node], wr );
	} else {
		auto ret = cceh->Get(key);
		wr->value = ret;
		enqueue(completionQueue[thisNode], wr);
	}
#else /* NUMAQ */

#ifdef REQUESTPOOL
	struct work_request *wr = (struct work_request *)RequestPool::GetInstance()->get_wr();
#else
	struct work_request *wr = (struct work_request *)malloc(sizeof(struct work_request));
#endif /* REQUESTPOOL */

#ifdef KV_DEBUG
	auto node = cceh->GetNodeID(key);

	if( thisNode != node )
		miss_cnt[thisNode]++;

	struct timespec g_start;
	clock_gettime(CLOCK_MONOTONIC, &g_start);
#endif
	auto ret =cceh->Get(key);
#ifdef KV_DEBUG
	struct timespec g_end;
	clock_gettime(CLOCK_MONOTONIC, &g_end);
	getTime += g_end.tv_nsec - g_start.tv_nsec + (g_end.tv_sec - g_start.tv_sec)*1000000000;
#endif

//	if(ret != reinterpret_cast<Value_t>(key)){
	if ( ret == NONE ) {
		failedSearch++;
	}
	wr->key = key;
	wr->value = ret;
#endif /* NUMAQ */
	
	return; 
}

Value_t NUMA_KV::FindAnyway(Key_t& key) {
	return cceh->FindAnyway(key);
}

bool NUMA_KV::WaitComplete(void) {
#ifdef NUMAQ
	while(!done){ asm("nop"); }
#endif
	done = false;
	nr_completed = 0;
	return true;
}

bool NUMA_KV::Recovery(void) {
	return cceh->Recovery();
}

bool NUMA_KV::Delete(Key_t& key) {
	return false;
}

vector<size_t> NUMA_KV::SegmentLoads(void){
	return cceh->SegmentLoads();
}

double NUMA_KV::Utilization(void) {
	return cceh->Utilization();
}

vector<unsigned> NUMA_KV::Freqs(void) {
	return cceh->Freqs();
}

size_t NUMA_KV::Capacity(void) {
	return cceh->Capacity();
}

int NUMA_KV::GetFailedSearch(void) {
	return failedSearch.load();
}

void NUMA_KV::PrintStats(void) {
#ifdef KV_DEBUG
	auto util = cceh->Utilization();
	auto cap = cceh->Capacity();
	auto freqs = cceh->Freqs();
	auto segs = cceh->SegmentLoads();
	auto metrics = cceh->Metrics();

	printf("Failed Search = %d\n", failedSearch.load());
	printf("Util =%.3f\t Capa =%lu\n", util, cap);
	printf("Freqeuncy on Node \t0= %d, 1= %d   ( counted on only Insert )\n", freqs[0], freqs[1]);
	printf("Remote request on Node \t0= %d, 1= %d\n", miss_cnt[0].load(), miss_cnt[1].load());
	printf("Segments in Node \t0= %zu, 1= %zu\n", segs[0], segs[1]);
	printf("LRFU Value on Node \t0= %f, 1=%f\n", metrics[0], metrics[1]);
	printf("QueueTime = \t%.3f (usec/req)\n", perNodeQueueTime/1000.0/numData/2);
	printf("InsertTime = \t%.3f (usec/req)\n", insertTime/1000.0/numData);
	printf("GetTime= \t%.3f (usec/req)\n", getTime/1000.0/numData);

	printf("%.3f, %lu, %d, %d, %d, %d, %zu, %zu, %.3f, %.3f, %.3f, %.3f, %.3f\n", util, cap, freqs[0], freqs[1], miss_cnt[0].load(), miss_cnt[1].load(), segs[0], segs[1], metrics[0], metrics[1],
			perNodeQueueTime/1000.0/numData/2, insertTime/1000.0/numData, getTime/1000.0/numData);
#endif
	return;
}

void NUMA_KV::PrintStats(bool save) {
#ifdef KV_DEBUG
	auto util = cceh->Utilization();
	auto cap = cceh->Capacity();
	auto freqs = cceh->Freqs();
	auto segs = cceh->SegmentLoads();
	auto metrics = cceh->Metrics();

	printf("%.3f, %lu, %d, %d, %d, %d, %zu, %zu, %.3f, %.3f, %.3f, %.3f, %.3f\n", util, cap, freqs[0], freqs[1], miss_cnt[0].load(), miss_cnt[1].load(), segs[0], segs[1], metrics[0], metrics[1],
			perNodeQueueTime/1000.0/numData/2, insertTime/1000.0/numData, getTime/1000.0/numData);
#endif
	return;
}
