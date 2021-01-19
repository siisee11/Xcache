#ifndef NUMA_KV_H_
#define NUMA_KV_H_

#include <atomic>
#include <cstring>
#include <cmath>
#include <vector>
#include <thread>
#include <pthread.h>
#include <iostream>
#include <bitset>
#include <mutex>
#include <condition_variable>
#include <numa.h>

#include "IHash.h"
#include "Ikvstore.h"
#include "CCEH_hybrid.h"
#include "circular_queue.h"
#include "util/atomic.h"

#define kNumNodes 2

#define MSG_EMPTY 0
#define MSG_INSERT 1
#define MSG_GET 2

using namespace std;

struct work_request
{
	uint16_t msg_type;
	Key_t key;
	Value_t value;
	int unique_id;
#if KV_DEBUG
	struct timespec time; 
#endif
};

class NUMA_KV : public KVStore {
	public:
		NUMA_KV(void);
		NUMA_KV(size_t, size_t, size_t);
		~NUMA_KV(void);
		void Insert(Key_t&, Value_t, int, int);
		bool Delete(Key_t&);
		void Get(Key_t&, int, int);
		Value_t Get(Key_t&, int);
		int GetNodeID(Key_t&);
		Value_t FindAnyway(Key_t&);
		bool Recovery(void);
		bool WaitComplete(void);
		double Utilization(void);
		size_t Capacity(void);
		void PrintStats(void);

		void* operator new(size_t size) {
			void *ret;
			if (posix_memalign(&ret, 64, size) ) ret=NULL;
			return ret;
		}

	private:
		IHash* cceh;
		void* global_chunk = NULL;
		queue_t **perNodeQueue = NULL;
		queue_t **completionQueue = NULL;
		vector<thread> receivers;
		vector<thread> cq_pollers;
		vector<thread> numakvThreads;
		atomic<int> nr_completed = 0;
		int nr_data;

#ifdef KV_DEBUG
		uint64_t perNodeQueueTime = 0;	
		uint64_t insertTime = 0;
		uint64_t getTime = 0;
#endif
};

#endif  // NUMA_KV_H_
