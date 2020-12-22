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
#include <libpmemobj.h>
#include <numa.h>
#include <ctime>
#include "common.h"

#include "src/IHash.h"
#include "src/Ikvstore.h"
#include "src/CCEH_PM_hybrid.h"
#include "src/atomic.h"
#include "circular_queue.h"

#define kNumNodes 2
#define WR_BUF_SIZE 1024

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

struct wr_pool
{
	atomic<int> cnt;
	struct work_request buf[WR_BUF_SIZE];	
};

class RequestPool { 
	private: 
		RequestPool() {
			struct wr_pool *pool = (struct wr_pool *)malloc(sizeof(struct wr_pool));	
			pools.push_back(pool);
			offset = 0;
			cur_buf = 0;
			printf("[ INFO ] RequestPool created\n");
		} 
		~RequestPool() {};	

		static RequestPool* instance;
		vector<struct wr_pool *>pools;
		atomic<int> offset;
		atomic<int> cur_buf;

	public: 
		static RequestPool* GetInstance() { 
			if(instance == NULL) instance = new RequestPool();
			return instance;
		} 

		inline void *get_wr() {
RETRY:
			auto off = offset.fetch_add(1);
			if ( off == WR_BUF_SIZE ) {
				struct wr_pool *pool = (struct wr_pool *)malloc(sizeof(struct wr_pool));	
				pools.push_back(pool);
				cur_buf++;
				offset = 0;
				off = 0;
			} else if ( off > WR_BUF_SIZE - 1 ){
				goto RETRY;
			} 
			pools[cur_buf]->cnt++;
			return (void *)(&pools[cur_buf]->buf[off]);
		}

		void free_wr(void *wr) {
			for (auto& pool : pools) { 
		 		if ( (unsigned *)&pool->buf[0] < (unsigned*)wr && (unsigned*)&pool->buf[1024*1024] > (unsigned *)wr) {
					pool->cnt--;
					if (pool->cnt == 0) {
						printf("free this pool\n");
					}
					break;
				}
			}
		}
};

class NUMA_KV : public KVStore {
	public:
		NUMA_KV(void);
		NUMA_KV(PMEMobjpool**, size_t, size_t, size_t);
		NUMA_KV(PMEMobjpool**, bool, size_t, size_t);
		~NUMA_KV(void);
		void Insert(Key_t&, Value_t, int, int);
		bool Delete(Key_t&);
		void Get(Key_t&, int, int);
		Value_t Get(Key_t&, int);
		int GetNodeID(Key_t&);
		Value_t FindAnyway(Key_t&);
		bool Recovery(void);
		bool WaitComplete(void);
		vector<size_t> SegmentLoads(void);
		double Utilization(void);
		vector<unsigned> Freqs(void);
		size_t Capacity(void);
		int GetFailedSearch(void);
		void PrintStats(void);
		void PrintStats(bool);

		void* operator new(size_t size) {
			void *ret;
			if (posix_memalign(&ret, 64, size) ) ret=NULL;
			return ret;
		}

	private:
		IHash* cceh;
		queue_t **perNodeQueue = NULL;
		queue_t **perNodeGetQueue = NULL;
		queue_t **completionQueue = NULL;
		vector<thread> receivers;
		vector<thread> cq_pollers;
		vector<thread> numakvThreads;
		atomic<int> miss_cnt[kNumNodes];
		atomic<int> nr_completed = 0;
		atomic<int> failedSearch = 0;
		atomic<bool> done = false;

#ifdef KV_DEBUG
		uint64_t perNodeQueueTime = 0;	
		uint64_t insertTime = 0;
		uint64_t getTime = 0;
#endif
};

#endif  // NUMA_KV_H_
