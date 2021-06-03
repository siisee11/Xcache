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
#include "IKV.h"
#include "circular_queue.h"
#include "util/atomic.h"
#include "util/counting_bloom_filter.h"

#define MSG_EMPTY 0
#define MSG_INSERT 1
#define MSG_GET 2

using namespace std;

struct Extent{
	uint64_t _key;
	uint64_t _len;
	Value_t _value;

	Extent(void)
	{  
		_key = 0;
		_len = 0;
		_value = NULL;
	}

	Extent(uint64_t key, Value_t value, uint64_t len)
	{  
		_key = key;
		_len = len;
		_value = value;
	}
};

class KV : public KVStore {
	public:
		KV(void);
		KV(size_t);
		KV(size_t, CountingBloomFilter<Key_t>*);
		~KV(void);
		bool Insert(Key_t&, Value_t);
		void InsertExtent(Key_t&, Value_t, uint64_t);
		bool Delete(Key_t&);
		Value_t Get(Key_t&);
		Value_t GetExtent(Key_t&);
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
		IHash* hash;
		CountingBloomFilter<Key_t>* bf;
		void* global_chunk = NULL;
		vector<thread> receivers;
		vector<thread> cq_pollers;
		vector<thread> numakvThreads;
		atomic<int> nr_completed = 0;
		int nr_data;
		std::mutex m;
#ifdef KV_DEBUG
		uint64_t insertTime = 0;
		uint64_t getTime = 0;
#endif
};

#endif  // KV_H_
