#ifdef CUCKOO 
#include "src/cuckoo_hash.h"
#elif defined DCCEH 
#include "src/cceh.h"
#elif defined PATH
#include "src/path_hashing.hpp"
#elif defined EXT
#include "src/extendible_hash.h"
#elif defined LEVEL
#include "src/Level_hashing.h"
#else
#include "src/linear_probing.h"
#endif

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

#include "KV.h"
#include "variables.h"

extern bool verbose_flag;
extern size_t numData;
extern struct bitmask *netcpubuf;
extern struct bitmask *kvcpubuf;
extern struct bitmask *pollcpubuf;

int deletecnt = 0;

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

/*
 * @size:
 * table size: Cuckoo, path, extendible, level
 */
KV::KV(size_t size, CountingBloomFilter<Key_t>* _bf)
{
#ifdef CUCKOO 
	hash = new CuckooHash(static_cast<size_t>(size));
#elif defined DCCEH 
	hash = new CCEH(static_cast<size_t>(size));
#elif defined PATH
	hash = new PathHashing(static_cast<size_t>(size));
#elif defined EXT
	hash = new ExtendibleHash(static_cast<size_t>(size));
#elif defined LEVEL
	hash = new LevelHashing(static_cast<size_t>(size));
#else
	hash = new LinearProbingHash(static_cast<size_t>(size));
#endif

	bf = _bf;

#ifdef KV_DEBUG
	insertTime = 0;
	getTime = 0;
#endif

	logger = new Logger(LOG_LEVEL_FATAL);

	logger->info("Logger Init", 1);

	dprintf("[  OK  ] KV init\n");
}

KV::~KV(void)
{ 
	return;
}

// return deleted or not
bool KV::Insert(Key_t& key, Value_t value) {
#ifdef KV_DEBUG
	struct timespec i_start;
	struct timespec i_end;
	clock_gettime(CLOCK_MONOTONIC, &i_start);
#endif
	auto deletedKey = hash->Insert(key, value);
	logger->info("Insert", 1);
	if (bf) {
		bf->Insert(key);
		if (deletedKey != (uint64_t)-1) {
			deletecnt++;
			bf->Delete(deletedKey);
//			printf("Key %lu deleted and query %s\n", deletedKey, bf->Query(deletedKey) ? "found":"not found");
		}
	}
#ifdef KV_DEBUG
	clock_gettime(CLOCK_MONOTONIC, &i_end);
	insertTime += i_end.tv_nsec - i_start.tv_nsec + (i_end.tv_sec - i_start.tv_sec)*1000000000;
#endif
	return deletedKey == (uint64_t)-1 ? false : true;
}

void KV::InsertExtent(Key_t& key, Value_t value, uint64_t len) {
#ifdef KV_DEBUG
	struct timespec i_start;
	struct timespec i_end;
	clock_gettime(CLOCK_MONOTONIC, &i_start);
#endif
	auto extent = new Extent(key, value, len);
	hash->Insert_extent(extent->_key, 0, extent->_len, (Value_t)extent);
#ifdef KV_DEBUG
	clock_gettime(CLOCK_MONOTONIC, &i_end);
	insertTime += i_end.tv_nsec - i_start.tv_nsec + (i_end.tv_sec - i_start.tv_sec)*1000000000;
#endif

	return;
}

Value_t KV::Get(Key_t& key) {
#ifdef KV_DEBUG
	struct timespec g_start;
	clock_gettime(CLOCK_MONOTONIC, &g_start);
#endif
	Value_t ret = hash->Get(key);
#ifdef KV_DEBUG
	struct timespec g_end;
	clock_gettime(CLOCK_MONOTONIC, &g_end);
	getTime += g_end.tv_nsec - g_start.tv_nsec + (g_end.tv_sec - g_start.tv_sec)*1000000000;
#endif
	return ret; 
}

/* extented get */
Value_t KV::GetExtent(Key_t& key) {
#ifdef KV_DEBUG
	struct timespec g_start;
	clock_gettime(CLOCK_MONOTONIC, &g_start);
#endif
	auto ret = hash->Get_extent(key, 0); 
	if (!ret)
		return ret;
	auto extent = (Extent *)ret; 
	auto key_diff = key - extent->_key;
	auto target = extent->_value + 4096 * key_diff;
	dprintf("key_diff=%lu, value=%llx\n", key_diff, target);
#ifdef KV_DEBUG
	struct timespec g_end;
	clock_gettime(CLOCK_MONOTONIC, &g_end);
	getTime += g_end.tv_nsec - g_start.tv_nsec + (g_end.tv_sec - g_start.tv_sec)*1000000000;
#endif
	return target;
}

Value_t KV::FindAnyway(Key_t& key) {
	return hash->FindAnyway(key);
}

bool KV::Recovery(void) {
	return hash->Recovery();
}

bool KV::Delete(Key_t& key) {
	return false;
}

double KV::Utilization(void) {
	return hash->Utilization();
}

size_t KV::Capacity(void) {
	return hash->Capacity();
}

void KV::PrintStats(void) {
#ifdef KV_DEBUG
	auto util = hash->Utilization();
	auto cap = hash->Capacity();

//	printf("Failed Search = %d\n", failedSearch.load());
	printf("Util =%.3f\t Capa =%lu\n", util, cap);
	printf("InsertTime = \t%.3f (usec)\n", insertTime/1000.0);
	printf("GetTime= \t%.3f (usec)\n", getTime/1000.0);
	printf("Key deleted %d\n", deletecnt);

//	printf("%.3f, %lu, %d, %d, %d, %d, %zu, %zu, %.3f, %.3f, %.3f, %.3f, %.3f\n", util, cap, freqs[0], freqs[1], miss_cnt[0].load(), miss_cnt[1].load(), segs[0], segs[1], metrics[0], metrics[1], 
//			perNodeQueueTime/1000.0/numData/2, insertTime/1000.0/numData, getTime/1000.0/numData);
#endif
	return;
}
