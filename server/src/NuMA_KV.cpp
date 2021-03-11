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

#include "src/NuMA_KV.h"
#include "src/variables.h"

extern bool verbose_flag;
extern size_t numData;
extern struct bitmask *netcpubuf;
extern struct bitmask *kvcpubuf;
extern struct bitmask *pollcpubuf;
extern int putcnt;
extern int getcnt;


using namespace std;

void *global_chunk = NULL;
atomic<int> offset = 0;

static void dprintf( const char* format, ... ) {
	if (verbose_flag) {
		va_list args;
		va_start( args, format );
		vprintf( format, args );
		va_end( args );
	}
}

NUMA_KV::NUMA_KV(void)
	: cceh{new CCEH}
{
	dprintf("[  OK  ] NUMA_KV \n");
}

NUMA_KV::NUMA_KV(size_t initCap, size_t nThreads, size_t nPollThreads)
	: cceh{new CCEH(static_cast<size_t>(initCap))}
{
	int numable = numa_available();
	if (numable == 0)
		dprintf("[  OK  ] NUMA feature ON\n");
	else
		dprintf("[ FAIL ] NUMA feature OFF\n");

#ifdef NUMAQ
	perNodeQueue = (queue_t**)malloc(kNumNodes * sizeof(queue_t*));
	for (int i = 0; i < kNumNodes; i++) {
		perNodeQueue[i] = create_queue("perNodeQueue");
	}

	completionQueue = (queue_t**)malloc(kNumNodes * sizeof(queue_t *));
	for (int i = 0; i < kNumNodes; i++) {
		completionQueue[i] = create_queue("completionQueue");
	}
	dprintf("[  OK  ] Lock Free Queue INIT\n");

	auto kvwork = [&] (int threadId) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		int cpu = sched_getcpu();
		int thisNode = numa_node_of_cpu(cpu);
//		printf("Thread %d working on cpu %d [node %d]\n", threadId, cpu, thisNode);
		while (1) {
			int caller;
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
				/* type == 0 is INSERT */
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
				/* type == 0 is SEARCH */
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
			}
		}
	};
	
	/* 일단 GET만 */
	auto cq_poll = [&] () {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		int cpu = sched_getcpu();
		int thisNode = numa_node_of_cpu(cpu);
		while (true) {
			struct work_request *wr = (struct work_request *)dequeue(completionQueue[thisNode]);	
			auto key = wr->key;
			auto value = wr->value;
			auto type = wr->msg_type;

			free(wr);
			wr = NULL;

			nr_completed++;

			/* all work done */
			if (nr_completed == numData)
				break;
		}
	};

	unsigned num_threads = nThreads;
	for (unsigned i = nPollThreads; i < num_threads + nPollThreads; ++i) {
		numakvThreads.emplace_back(thread(kvwork, i));

		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(i, &cpuset);
		int rc = pthread_setaffinity_np(numakvThreads[i - nPollThreads].native_handle(),
				sizeof(cpu_set_t), &cpuset);
		if (rc != 0) {
			std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
		}

		numakvThreads[i - nPollThreads].detach();
	}

	for (unsigned i = 0; i < nPollThreads; ++i) {
		cq_pollers.emplace_back(thread(cq_poll));

		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(i, &cpuset);
		int rc = pthread_setaffinity_np(cq_pollers[i].native_handle(),
				sizeof(cpu_set_t), &cpuset);
		if (rc != 0) {
			std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
		}
	}
#endif

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
//		miss_cnt[thisNode]++;
		struct work_request *wr = (struct work_request *)malloc(sizeof(struct work_request));
		wr->unique_id = unique_id;
		wr->msg_type = MSG_INSERT;
		wr->value = value;
		wr->key = key;

		enqueue( perNodeQueue[node], wr );
	}
	else {
		cceh->Insert(key, value);
		nr_completed++;

		/* all work done */
		if (nr_completed == numData)
			done = true;
	}

#else /* ************NUMAQ************** */

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

#endif /* NUMAQ */
	return;
}

/* Normal get non-queue */
Value_t NUMA_KV::Get(Key_t& key, int thisNode) {
#ifdef KV_DEBUG
	struct timespec g_start;
	clock_gettime(CLOCK_MONOTONIC, &g_start);
#endif
	auto ret = cceh->Get(key);
#ifdef KV_DEBUG
	struct timespec g_end;
	clock_gettime(CLOCK_MONOTONIC, &g_end);
	getTime += g_end.tv_nsec - g_start.tv_nsec + (g_end.tv_sec - g_start.tv_sec)*1000000000;
#endif
	return ret; 
}

void NUMA_KV::Get(Key_t& key, int unique_id, int thisNode) {
	auto node = 0;
#ifdef NUMAQ
	node = cceh->GetNodeID(key);
#else
	node = rand() % kNumNodes;
#endif

	struct work_request *wr = (struct work_request *)malloc(sizeof(struct work_request));
	wr->unique_id = unique_id;
	wr->msg_type = MSG_GET;
	wr->key = key;
	
	enqueue( perNodeQueue[node], wr );
	return; 
}

int NUMA_KV::GetNodeID(Key_t& key) {
	return cceh->GetNodeID(key);
}

Value_t NUMA_KV::FindAnyway(Key_t& key) {
	return cceh->FindAnyway(key);
}

bool NUMA_KV::WaitComplete(void) {
	for(auto& t: cq_pollers) t.join();
	return true;
}

bool NUMA_KV::Recovery(void) {
	return cceh->Recovery();
}

bool NUMA_KV::Delete(Key_t& key) {
	return false;
}

double NUMA_KV::Utilization(void) {
	return cceh->Utilization();
}

size_t NUMA_KV::Capacity(void) {
	return cceh->Capacity();
}

void NUMA_KV::PrintStats(void) {
#ifdef KV_DEBUG
	auto util = cceh->Utilization();
	auto cap = cceh->Capacity();
//	auto freqs = cceh->Freqs();
//	auto segs = cceh->SegmentLoads();
//	auto metrics = cceh->Metrics();

//	printf("Failed Search = %d\n", failedSearch.load());
	printf("Util =%.3f\t Capa =%lu\n", util, cap);
//	printf("Freqeuncy on Node \t0= %d, 1= %d   ( counted on only Insert )\n", freqs[0], freqs[1]);
//	printf("Remote request on Node \t0= %d, 1= %d\n", miss_cnt[0].load(), miss_cnt[1].load());
//	printf("Segments in Node \t0= %zu, 1= %zu\n", segs[0], segs[1]);
//	printf("LRFU Value on Node \t0= %f, 1=%f\n", metrics[0], metrics[1]);
//	printf("QueueTime = \t%.3f (usec/req)\n", perNodeQueueTime/1000.0/numData/2);
	printf("InsertTime = \t%.3f (usec/req)\n", insertTime/1000.0/putcnt);
	printf("GetTime= \t%.3f (usec/req)\n", getTime/1000.0/getcnt);

//	printf("%.3f, %lu, %d, %d, %d, %d, %zu, %zu, %.3f, %.3f, %.3f, %.3f, %.3f\n", util, cap, freqs[0], freqs[1], miss_cnt[0].load(), miss_cnt[1].load(), segs[0], segs[1], metrics[0], metrics[1], \
			perNodeQueueTime/1000.0/numData/2, insertTime/1000.0/numData, getTime/1000.0/numData);
#endif
	return;
}
