#ifndef CCEH_H_
#define CCEH_H_

#include <cstring>
#include <vector>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <pthread.h>
#include <libpmemobj.h>
#include <bits/stdc++.h>

#include "util/persist.h"  /* clflush */
#include "util/pair.h"
#include "src/IHash.h"
#include "src/variables.h"
#include "src/log.h"

using namespace std;

constexpr size_t SIBLING_MASK = ((size_t)1 << (8*sizeof(size_t)-1));
constexpr size_t DEPTH_MASK = (((size_t)1 << (8*sizeof(size_t)-1)) - 1);
#define TOID_ARRAY(x) TOID(x)

struct Metric {
	unsigned atime;
	double crf; /* Combined Recency and Frequency */
};

struct Segment_root;
struct Segment;

POBJ_LAYOUT_BEGIN(HashTable);
POBJ_LAYOUT_ROOT(HashTable, struct Segment_root);
POBJ_LAYOUT_TOID(HashTable, struct Segment);
POBJ_LAYOUT_END(HashTable);

constexpr size_t kSegmentBits = 8;
constexpr size_t kNumCacheLine = 4; 	/* Linear probing distance */
//constexpr size_t kSegmentBits = 1;
//constexpr size_t kNumCacheLine = 1; 	/* Linear probing distance */
constexpr size_t kMask = (1 << kSegmentBits)-1;
constexpr size_t kShift = kSegmentBits;
constexpr size_t kNumPairPerCacheLine = 4;
constexpr size_t kSegmentSize = (1 << kSegmentBits) * sizeof(Pair) * kNumPairPerCacheLine;
constexpr size_t kCuckooThreshold = 16;

class CCEH;
 
class LRUCache {
    // store keys of cache
    list<uint64_t> dq;
 
    // store references of key in cache
    unordered_map<uint64_t, list<uint64_t>::iterator> ma;
    int csize; // maximum capacity of cache
	std::mutex m1;
	CCEH *cceh;
 
public:
    LRUCache(int, CCEH *);
    void refer(uint64_t);
    void display();
};

struct Segment{
	static const size_t kNumSlot = kSegmentSize/sizeof(Pair); /* # of buckets in a Segment */

	Segment(void){ }
	~Segment(void){ }

	void initSegment(TOID(struct Segment) _sibling){
		for(int i=0; i<kNumSlot; ++i){
			bucket[i].key = INVALID;
		}
		local_depth = 0;
		sema = 0;
		sibling[0] = _sibling;
	}

	void initSegment(size_t depth){
		for(int i=0; i<kNumSlot; ++i){
			bucket[i].key = INVALID;
		}
		local_depth = depth;
		sema = 0;
	}

	void initSegment(TOID(struct Segment) _sibling, size_t depth){
		for(int i=0; i<kNumSlot; ++i){
			bucket[i].key = INVALID;
		}
		local_depth = depth;
		sema = 0;
		sibling[0] = _sibling;
	}

	bool suspend(void){
		int64_t val;
		do{
			val = sema;
			if(val < 0)
				return false;
		}while(!CAS(&sema, &val, -1));

		int64_t wait = 0 - val - 1;
		while(val && sema != wait){
			asm("nop");
		}
		return true;
	}

	bool lock(void){
		int64_t val = sema;
		while(val > -1){
			if(CAS(&sema, &val, val+1))
				return true;
			val = sema;
		}
		return false;
	}

	void unlock(void){
		int64_t val = sema;
		while(!CAS(&sema, &val, val-1)){
			val = sema;
		}
	}

	int Insert(PMEMobjpool*, Key_t&, Value_t, size_t, size_t);
	bool Insert4split(Key_t&, Value_t, size_t);
	Segment** Split();
//	TOID(struct Segment)* Split(PMEMobjpool*);
	std::vector<std::pair<size_t, size_t>> find_path(size_t, size_t);
	void execute_path(PMEMobjpool*, std::vector<std::pair<size_t, size_t>>&, Key_t&, Value_t);
	void execute_path(std::vector<std::pair<size_t, size_t>>&, Pair);
	size_t numElement(void);

	Pair bucket[kNumSlot];
	int64_t sema = 0;
	size_t local_depth;
	TOID(struct Segment) sibling[2];

};

struct Segment_root{
	TOID(struct Segment) segment;
	bool normal_shutdown;
};

struct Directory{
	static const size_t kDefaultDepth = 10;

	TOID(struct Segment)* segment;
	int64_t sema = 0;
	size_t capacity;		
	size_t depth;	

	bool suspend(void){
		int64_t val;
		do{
			val = sema;
			if(val < 0)
				return false;
		}while(!CAS(&sema, &val, -1));

		int64_t wait = 0 - val - 1;
		while(val && sema != wait){
			asm("nop");
		}
		return true;
	}

	bool lock(void){
		int64_t val = sema;
		while(val > -1){
			if(CAS(&sema, &val, val+1))
				return true;
			val = sema;
		}
		return false;
	}

	void unlock(void){
		int64_t val = sema;
		while(!CAS(&sema, &val, val-1)){
			val = sema;
		}
	}

	Directory(void){
		depth = kDefaultDepth;
		capacity = pow(2, depth);
		segment = new TOID(struct Segment)[capacity];
		sema = 0;
	}

	Directory(size_t _depth){
		depth = _depth;
		capacity = pow(2, depth);
		segment = new TOID(struct Segment)[capacity];
		sema = 0;
	}

	~Directory(void){
		delete[] segment;
	}

	void* operator new(size_t size){
		void* ret;
		if (posix_memalign(&ret, 64, size) ) ret=NULL;
		return ret;
	}

	void* operator new[](size_t size){
		void* ret;
		if (posix_memalign(&ret, 64, size) ) ret=NULL;
		return ret;
	}
};

class CCEH : public IHash {
	public:
		CCEH(PMEMobjpool**);
		CCEH(PMEMobjpool**, bool);
		CCEH(PMEMobjpool**, PMEMobjpool**, size_t);
		~CCEH(void){ }

		int GetNodeID(Key_t&);
		int GetNodeID(TOID(struct Segment));
		void Insert(Key_t&, Value_t);
		bool InsertOnly(Key_t&, Value_t);	
		bool Delete(Key_t&);
		Value_t Get(Key_t&);
		Value_t FindAnyway(Key_t&);

		void PersistSegment(struct Segment *);

		double Utilization(void);
		size_t Capacity(void);
		size_t Depth(void);
		bool Recovery(void);
		std::vector<size_t> SegmentLoads(void);
		std::vector<unsigned> Freqs(void);
		std::vector<double> Metrics(void);

		void* operator new(size_t size){
			void* ret;
			if (posix_memalign(&ret, 64, size) ) ret=NULL;
			return ret;
		}
		PMEMobjpool* lpop[NUM_NUMA];
		PMEMobjpool* pop[NUM_NUMA];

	private:
		size_t segments_in_node[NUM_NUMA];
		unsigned freq[NUM_NUMA];
		struct Directory* dir;
		LRUCache *ca;
#ifdef LRFU
		unsigned gtime;
		struct Metric lrfu[NUM_NUMA];
#endif
};



#endif

