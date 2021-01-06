#include <iostream>
#include <cmath>
#include <thread>
#include <bitset>
#include <cassert>
#include <unordered_map>
#include <stdio.h>
#include <vector>

#include "src/CCEH_PM_hybrid.h"
#include "src/hash.h"
#include "src/variables.h"

#define f_seed 0xc70697UL
#define s_seed 0xc70697UL
//#define f_seed 0xc70f6907UL
//#define s_seed 0xc70f6907UL

#define RD_RO DIRECT_RW
#define RD_RW DIRECT_RW

#undef D_RW
#undef D_RO
#define D_RW(o) \
	( (o).oid.pool_uuid_lo != 0 ? (reinterpret_cast < __typeof__((o)._type) > (pmemobj_direct((o).oid))) : (reinterpret_cast < Segment * > ((o).oid.off) ) )

#define D_RO(o) \
	( (o).oid.pool_uuid_lo != 0 ? (reinterpret_cast < const __typeof__((o)._type) > (pmemobj_direct((o).oid))) : (reinterpret_cast < const Segment *> ((o).oid.off) ) )

using namespace std;


/* --------------------------------------- LRU Implementation -------------------------*/
// Declare the size
LRUCache::LRUCache(int n, CCEH *hash)
{
    csize = n;
	cceh = hash;
}
 
/* Refers key x (address of segment) with in the LRU cache */
void LRUCache::refer(uint64_t x)
{
	m1.lock();

    /* not present in cache */
    if (ma.find(x) == ma.end()) {
		/* cache is full */
		if ( dq.size() == csize ) {
			// delete least recently used element
			auto last = dq.back();
 
			// Pops the last elmeent
			dq.pop_back();

			/* Persist (Segment *)last and save pointer to dir */
			struct Segment *target = (struct Segment *)last;
			cceh->PersistSegment(target);

			printf("LRU FULL\n");
 
			// Erase the last
			ma.erase(last);
		}
    } else {
	/* present in cache */
        dq.erase(ma[x]);
	}
 
    // update reference
    dq.push_front(x);
    ma[x] = dq.begin();
	
	m1.unlock();
}
 
// Function to display contents of cache
void LRUCache::display()
{
 
    // Iterate in the deque and print
    // all the elements in it
    for (auto it = dq.begin(); it != dq.end(); it++)
        cout << (*it) << " ";
 
    cout << endl;
}


/* --------------------------------------- Segment Implementation -------------------------*/

void Segment::execute_path(PMEMobjpool* pop, vector<pair<size_t, size_t>>& path, Key_t& key, Value_t value){
	for(int i=path.size()-1; i>0; --i){
		bucket[path[i].first] = bucket[path[i-1].first];
		pmemobj_persist(pop, (char*)&bucket[path[i].first], sizeof(Pair));
	}
	bucket[path[0].first].value = value;
	mfence();
	bucket[path[0].first].key = key;
	pmemobj_persist(pop, (char*)&bucket[path[0].first], sizeof(Pair));
}

void Segment::execute_path(vector<pair<size_t, size_t>>& path, Pair _bucket){
	int i = 0;
	int j = (i+1) % 2;

	Pair temp[2];
	temp[0] = _bucket;
	for(auto p: path){
		temp[j] = bucket[p.first];
		bucket[p.first] = temp[i];
		i = (i+1) % 2;
		j = (i+1) % 2;
	}
}

vector<pair<size_t, size_t>> Segment::find_path(size_t target, size_t pattern){
	vector<pair<size_t, size_t>> path;
	path.reserve(kCuckooThreshold);
	path.emplace_back(target, bucket[target].key);

	auto cur = target;
	auto depth = local_depth & DEPTH_MASK;
	int i = 0;

	do{
		Key_t* key = &bucket[cur].key;
		auto f_hash = hash_funcs[0](key, sizeof(Key_t), f_seed);
		auto s_hash = hash_funcs[2](key, sizeof(Key_t), s_seed);

		if((f_hash >> (8*sizeof(f_hash) - depth)) != pattern || *key == INVALID){
			break;
		}

		for(int j=0; j<kNumPairPerCacheLine*kNumCacheLine; ++j){
			auto f_idx = (((f_hash & kMask) * kNumPairPerCacheLine) + j) % kNumSlot;
			auto s_idx = (((s_hash & kMask) * kNumPairPerCacheLine) + j) % kNumSlot;

			if(f_idx == cur){
				path.emplace_back(s_idx, bucket[s_idx].key);
				cur = s_idx;
				break;
			}
			else if(s_idx == cur){
				path.emplace_back(f_idx, bucket[f_idx].key);
				cur = f_idx;
				break;
			}
		}
		++i;
	}while(i < kCuckooThreshold);

	if(i == kCuckooThreshold){
		path.resize(0);
	}

	return move(path);
}


bool Segment::Insert4split(Key_t& key, Value_t value, size_t loc){
	for(int i=0; i<kNumPairPerCacheLine*kNumCacheLine; ++i){
		auto slot = (loc+i) % kNumSlot;
		if(bucket[slot].key == INVALID){
			bucket[slot].key = key;
			bucket[slot].value = value;
			return 1;
		}
	}
	return 0;
}

Segment** Segment::Split(){
	Segment** split = new Segment*[2];
	split[0] = this;
	split[1] = new Segment();
	size_t depth = (local_depth & DEPTH_MASK);
	split[1]->initSegment(depth + 1);

	auto pattern = ((size_t)1 << (sizeof(Key_t)*8 - depth - 1));
	for(int i=0; i<kNumSlot; ++i){
		auto f_hash = hash_funcs[0](&bucket[i].key, sizeof(Key_t), f_seed);
		if(f_hash & pattern){
			if(!split[1]->Insert4split(bucket[i].key, bucket[i].value, (f_hash & kMask)*kNumPairPerCacheLine)){
				auto s_hash = hash_funcs[2](&bucket[i].key, sizeof(Key_t), s_seed);
				if(!split[1]->Insert4split(bucket[i].key, bucket[i].value, (s_hash & kMask)*kNumPairPerCacheLine)){
#ifdef CUCKOO
					auto path1 = find_path((f_hash & kMask)*kNumPairPerCacheLine, pattern);
					auto path2 = find_path((s_hash & kMask)*kNumPairPerCacheLine, pattern);
					if(path1.size() == 0 && path2.size() == 0){
						cerr << "[" << __func__ << "]: something wrong -- need to adjust probing distance" << endl;
					}
					else{
						if(path1.size() == 0){
							execute_path(path2, bucket[i]);
						}
						else if(path2.size() == 0){
							execute_path(path1, bucket[i]);
						}
						else if(path1.size() < path2.size()){
							execute_path(path1, bucket[i]);
						}
						else{
							execute_path(path2, bucket[i]);
						}
					}
#else
					cerr << "[" << __func__ << "]: something wrong -- need to adjust probing distance" << endl;
#endif
				}
			}
		}
	}

	clflush((char*)split[1], sizeof(Segment));


	return split;
}

size_t Segment::numElement(void) {
	size_t sum = 0;
	for (unsigned i = 0; i < kNumSlot; ++i) {
		if (bucket[i].key != INVALID) {
			sum++;
		}
	}
	return sum;
}


CCEH::CCEH(PMEMobjpool** _pop, bool recovery){
	for(int i=0; i<NUM_NUMA; ++i){
		pop[i] = _pop[i];
		segments_in_node[i] = 0;
	}

#ifdef LRFU
	gtime = 0;
	for(int i=0; i<NUM_NUMA; ++i){
		lrfu[i].atime = 0;
		lrfu[i].crf= 0;
	}
#endif
}

CCEH::CCEH(PMEMobjpool** _pop)
	: dir{new Directory()}, ca{new LRUCache(8192, this)}
{
#ifdef RANDOM
	srand(time(NULL));
#endif
	for(int i=0; i<NUM_NUMA; ++i){
		pop[i] = _pop[i];
		segments_in_node[i] = 0;
		freq[i] = 0;
	}

#ifdef LRFU
	gtime = 0;
	for(int i=0; i<NUM_NUMA; ++i){
		lrfu[i].atime = 0;
		lrfu[i].crf= 0;
	}
#endif

	TOID(struct Segment_root) root = POBJ_ROOT(pop[0], struct Segment_root);

	for(int i=0; i<dir->capacity; ++i){
#ifdef BALANCED
		int node_id = i % NUM_NUMA;
#elif defined RANDOM
		int node_id = (i+ rand()) % NUM_NUMA;
#else
		int node_id = 0;
#endif
		POBJ_ALLOC(pop[node_id], &dir->segment[i], struct Segment, sizeof(struct Segment), NULL, NULL);
		segments_in_node[node_id]++;
	}
	for(int i=0; i<dir->capacity; ++i){
		if(i != dir->capacity-1)
			D_RW(dir->segment[i])->initSegment(dir->segment[i+1]);
		else
			D_RW(dir->segment[i])->initSegment(TOID_NULL(struct Segment));
	}
	RD_RW(root)->segment = dir->segment[0];
	pmemobj_persist(pop[0], (char*)&RD_RO(root)->segment, sizeof(TOID(struct Segment)));
}

CCEH::CCEH(PMEMobjpool** _pop, PMEMobjpool** _lpop, size_t initCap)
	: dir{new Directory(static_cast<size_t>(log2(initCap)))}, ca{new LRUCache(1024, this)}
{
#ifdef RANDOM
	srand(time(NULL));
#endif
	for(int i=0; i<NUM_NUMA; ++i){
		pop[i] = _pop[i];
		lpop[i] = _lpop[i];
		segments_in_node[i] = 0;
		freq[i] = 0;
	}
#ifdef LRFU
	gtime = 0;
	for(int i=0; i<NUM_NUMA; ++i){
		lrfu[i].atime = 0;
		lrfu[i].crf= 0;
	}
#endif

	TOID(struct Segment_root) root = POBJ_ROOT(pop[0], struct Segment_root);

	for(int i=0; i<dir->capacity; ++i){
#ifdef BALANCED
		int node_id = i % NUM_NUMA;
#elif defined BALANCED
		int node_id = (i + rand()) % NUM_NUMA;
#elif defined LRFU
		int node_id = ((i < dir->capacity / 2) ? 0 : 1) % NUM_NUMA;
#else
		int node_id = 0;
#endif
		POBJ_ALLOC(pop[node_id], &dir->segment[i], struct Segment, sizeof(struct Segment), NULL, NULL);
		segments_in_node[node_id]++;
	}
	for(int i=0; i<dir->capacity; ++i){
		if(i != dir->capacity-1)
			D_RW(dir->segment[i])->initSegment(dir->segment[i+1], static_cast<size_t>(log2(initCap)));
		else
			D_RW(dir->segment[i])->initSegment(TOID_NULL(struct Segment), static_cast<size_t>(log2(initCap)));
	}
	RD_RW(root)->segment = dir->segment[0];
	pmemobj_persist(pop[0], (char*)&RD_RO(root)->segment, sizeof(TOID(struct Segment)));
}


int CCEH::GetNodeID(Key_t& key){
	auto f_hash = hash_funcs[0](&key, sizeof(Key_t), f_seed);

RETRY:
	auto x = (f_hash >> (8*sizeof(f_hash) - dir->depth));

	if (dir->segment[x].oid.pool_uuid_lo == 0 ) return -1;

	uint64_t target_node = (uint64_t)D_RO(dir->segment[x]) - dir->segment[x].oid.off;
	for(int i=0; i<NUM_NUMA; ++i){
		if(target_node == (uint64_t)pop[i])
			return i;
	}
	goto RETRY;
	return -1;
}

int CCEH::GetNodeID(TOID(struct Segment) segment){
	if (segment.oid.pool_uuid_lo == 0 ) return -1;
	uint64_t target_node = (uint64_t)D_RO(segment) - segment.oid.off;
	for(int i=0; i<NUM_NUMA; ++i){
		if(target_node == (uint64_t)pop[i]){
			return i;
		}
	}
}

void CCEH::PersistSegment(struct Segment *_target) {
	auto numElem = _target->numElement();
	TOID(struct Segment) newSeg;
	Key_t key = 0;

	/* Alloc Segment on PMEM */
	POBJ_ALLOC(pop[0], &newSeg, struct Segment, sizeof(struct Segment), NULL, NULL);
	D_RW(newSeg)->initSegment(TOID_NULL(struct Segment), _target->local_depth);

	/* Alloc PAGE LOG on PMEM */
	TOID(char) temp;
	POBJ_ALLOC(lpop[0], &temp, char, sizeof(char)*4096 * numElem, NULL, NULL); /* XXX 0 */

	/* Copy pages from DRAM to PMEM */
	for (int i = 0; i < Segment::kNumSlot ; i++ ) {
		if (_target->bucket[i].key != INVALID) {
			if (key == 0)
				key = _target->bucket[i].key;
			uint64_t temp_addr = (uint64_t)lpop[0] + temp.oid.off + 4096 * i;
			memcpy((void *)temp_addr, (void *)_target->bucket[i].value, sizeof(char)*4096);
			D_RW(newSeg)->bucket[i].key = _target->bucket[i].key;
			D_RW(newSeg)->bucket[i].value = (Value_t)temp_addr;
		}
	}
	uint64_t temp_addr = (uint64_t)lpop[0] + temp.oid.off;
	pmemobj_persist(lpop[0], (void*)temp_addr, sizeof(char)*4096 * numElem); /* TODO: this cause slowdown */

	auto f_hash = hash_funcs[0](&key, sizeof(Key_t), f_seed);
	auto f_idx = (f_hash & kMask) * kNumPairPerCacheLine;
	auto x = (f_hash >> (8*sizeof(f_hash) - dir->depth)); /* MSB dir->depth bits*/

	dir->segment[x] = newSeg; 	/* Should be atomic */
#if 0
	if(CAS(&dir->segment[x], &target, newSeg)){
		printf("PersistSegment success\n");
	} else {
		printf("PersistSegment success\n");
	}
#endif
}

void CCEH::Insert(Key_t& key, Value_t value){

	auto f_hash = hash_funcs[0](&key, sizeof(Key_t), f_seed);
	auto f_idx = (f_hash & kMask) * kNumPairPerCacheLine;

RETRY:
	auto x = (f_hash >> (8*sizeof(f_hash) - dir->depth)); /* MSB dir->depth bits*/
	auto target = dir->segment[x];

	if(!D_RO(target)){
		std::this_thread::yield();
		goto RETRY;
	}

	/* acquire segment exclusive lock */
	if(!D_RW(target)->lock()){
		std::this_thread::yield();
		goto RETRY;
	}

	auto target_check = (f_hash >> (8*sizeof(f_hash) - dir->depth));
	if(D_RO(target) != D_RO(dir->segment[target_check])){
		D_RW(target)->unlock();
		std::this_thread::yield();
		goto RETRY;
	}

	/* increase global time */
#ifdef LRFU
	gtime++;
#endif
	int cur_node_id = GetNodeID(target);
	
	struct Segment *_target = D_RW(target);

	/* LRU */
	if (cur_node_id == -1) ca->refer((uint64_t) _target);

	auto target_local_depth = (_target->local_depth & DEPTH_MASK);
	auto pattern = (f_hash >> (8*sizeof(f_hash) - target_local_depth));
	for(unsigned i=0; i<kNumPairPerCacheLine * kNumCacheLine; ++i){
		/* i [0, 32) */
		auto loc = (f_idx + i) % Segment::kNumSlot;
		auto _key = _target->bucket[loc].key;
		/* validity check for entry keys */
		if((((hash_funcs[0](&_target->bucket[loc].key, sizeof(Key_t), f_seed) >> (8*sizeof(f_hash)-target_local_depth)) != pattern) || (_target->bucket[loc].key == INVALID)) && (D_RO(target)->bucket[loc].key != SENTINEL)){
			if(CAS(&_target->bucket[loc].key, &_key, SENTINEL)){
				_target->bucket[loc].value = value;
				mfence();
				_target->bucket[loc].key = key;

				if (cur_node_id != -1) {
					/* Target Segment on PMEM */
					pmemobj_persist(pop[cur_node_id], (char*)&_target->bucket[loc], sizeof(Pair));
				}
				/* release segment exclusive lock */
				_target->unlock();

#ifdef LRFU
				if (cur_node_id != -1) {
					/* Target Segment on PMEM */
					/* Calculate LRFU */
					auto crf = lrfu[cur_node_id].crf;
					auto atime = lrfu[cur_node_id].atime;
					crf = 1 + crf * pow(0.5, (gtime - atime) * 0.5); 
					lrfu[cur_node_id].crf = crf;
					lrfu[cur_node_id].atime = gtime;
				}
#endif

#ifdef STATISTIC
				if (cur_node_id != -1) {
					/* statistic */
					unsigned f = freq[cur_node_id];
					while(!CAS(&freq[cur_node_id], &f, f+1)){
						f = freq[cur_node_id];
					}
				}
#endif
				return;
			}
		}
	}

	/* If can't find location, Second hashing */
	auto s_hash = hash_funcs[2](&key, sizeof(Key_t), s_seed);
	auto s_idx = (s_hash & kMask) * kNumPairPerCacheLine;

	for(unsigned i=0; i<kNumPairPerCacheLine * kNumCacheLine; ++i){
		auto loc = (s_idx + i) % Segment::kNumSlot;
		auto _key = D_RO(target)->bucket[loc].key;
		if((((hash_funcs[0](&D_RO(target)->bucket[loc].key, sizeof(Key_t), f_seed) >> (8*sizeof(f_hash)-target_local_depth)) != pattern) || (D_RO(target)->bucket[loc].key == INVALID)) && (D_RO(target)->bucket[loc].key != SENTINEL)){
			if(CAS(&D_RW(target)->bucket[loc].key, &_key, SENTINEL)){
				D_RW(target)->bucket[loc].value = value;
				mfence();
				D_RW(target)->bucket[loc].key = key;
				if (cur_node_id != -1) {
					pmemobj_persist(pop[cur_node_id], (char*)&D_RO(target)->bucket[loc], sizeof(Pair));
				}
				D_RW(target)->unlock();

#ifdef LRFU
				if (cur_node_id != -1) {
					/* Calculate LRFU */
					auto crf = lrfu[cur_node_id].crf;
					auto atime = lrfu[cur_node_id].atime;
					crf = 1 + crf * pow(0.5, (gtime - atime) * 0.5); 
					lrfu[cur_node_id].crf = crf;
					lrfu[cur_node_id].atime = gtime;
				}
#endif

#ifdef STATISTIC
				if (cur_node_id != -1) {
					/* statistic */
					unsigned f = freq[cur_node_id];
					while(!CAS(&freq[cur_node_id], &f, f+1)){
						f = freq[cur_node_id];
					}
				}
#endif

				return;
			}
		}
	}

	// COLLISION !!
	/* need to split segment but release the exclusive lock first to avoid deadlock */
	D_RW(target)->unlock();

	if(!D_RW(target)->suspend()){
		std::this_thread::yield();
		goto RETRY;
	}

	/* need to check whether the target segment has been split */
	if(target_local_depth != (D_RO(target)->local_depth & DEPTH_MASK)){
		D_RW(target)->sema = 0;
		std::this_thread::yield();
		goto RETRY;
	}

#ifdef CUCKOO
	auto path1 = D_RW(target)->find_path(f_idx, pattern);
	auto path2 = D_RW(target)->find_path(s_idx, pattern);
	if(path1.size() != 0 || path2.size() != 0){
		auto path = &path1;
		if(path1.size() == 0 || (path2.size() != 0 && path2.size() < path1.size()) || (path2.size() != 0 && path1[0].second == INVALID)){
			path = &path2;
		}
		D_RW(target)->execute_path(pop[cur_node_id], *path, key, value);
		D_RW(target)->sema = 0;
		return;
	}
#endif

/* Split Segment Policy ( Balanced, random, LRFU, Skew) */
#ifdef BALANCED
	size_t min_load;
	int min_node_id;
	do{
		min_load = segments_in_node[0];
		min_node_id = 0;
		for(int i=1; i<NUM_NUMA; ++i){
			if(min_load > segments_in_node[i]){
				min_load = segments_in_node[i];
				min_node_id = i;
			}
		}
	}while(!CAS(&segments_in_node[min_node_id], &min_load, min_load+1));

	auto s = D_RW(target)->Split(pop[min_node_id]);
#elif defined RANDOM
	int node_id = rand() % NUM_NUMA;
	size_t load = segments_in_node[node_id];
	while(!CAS(&segments_in_node[node_id], &load, load+1)){
		load = segments_in_node[node_id];
	}

	auto s = D_RW(target)->Split(pop[node_id]);
#elif defined LRFU
	size_t min_load;
	int min_node_id;
	int target_node_id = cur_node_id;
	
	min_load = lrfu[0].crf;
	min_node_id = 0;
	for(int i=1; i<NUM_NUMA; ++i){
		if(min_load > lrfu[i].crf){
			min_load = lrfu[i].crf;
			min_node_id = i;
		}
	}

	if ( lrfu[cur_node_id].crf - min_load > 1 )
		target_node_id = min_node_id;

#ifdef STATISTIC
	size_t load = segments_in_node[target_node_id];
	while(!CAS(&segments_in_node[target_node_id], &load, load+1)){
		load = segments_in_node[target_node_id];
	}
#endif

	auto s = D_RW(target)->Split(pop[target_node_id]);

#else /* SKEWED */

#ifdef STATISTIC
	size_t load = segments_in_node[0];
	while(!CAS(&segments_in_node[0], &load, load+1)){
		load = segments_in_node[0];
	}
#endif
	auto s = D_RW(target)->Split();
#endif

	/* LRU */
	ca->refer((uint64_t)s[0]);
	ca->refer((uint64_t)s[1]);

DIR_RETRY:
	/* need to double the directory */
	if(target_local_depth == dir->depth){
		if(!dir->suspend()){
			std::this_thread::yield();
			goto DIR_RETRY;
		}

		x = (f_hash >> (8*sizeof(f_hash) - dir->depth));
		auto dir_old = dir;
		auto d = dir->segment;
		auto _dir = new Directory(dir->depth+1);
		for(int i=0; i<dir->capacity; ++i){
			if(i == x){
				_dir->segment[2*i].oid.off = (uint64_t)s[0]; 		/* Store segment pointer to oid.off */
				_dir->segment[2*i].oid.pool_uuid_lo = 0; 			/* set oid.pool_uuid_lo to identify DRAM segment */
				_dir->segment[2*i+1].oid.off = (uint64_t)s[1];
				_dir->segment[2*i+1].oid.pool_uuid_lo = 0; 		

			}
			else{
				_dir->segment[2*i] = d[i];
				_dir->segment[2*i+1] = d[i];
			}
		}
		dir = _dir;
		if(s[0]->local_depth >> (8*sizeof(size_t)-1)){
//			s[0]->sibling[0] = s[1];
			s[0]->local_depth = (s[0]->local_depth & DEPTH_MASK) + 1;
		}
		else{
//			s[0]->sibling[1] = s[1];
			s[0]->local_depth = (s[0]->local_depth | SIBLING_MASK) + 1;
		}
		/* release segment exclusive lock */
		s[0]->sema = 0;
		/* TBD */
		// delete dir_old;

	}
	else{ 
		/* normal split */
		while(!dir->lock()){
			asm("nop");
		}

		x = (f_hash >> (8*sizeof(f_hash) - dir->depth));
		if(dir->depth == target_local_depth + 1){
			if(x%2 == 0){
				dir->segment[x].oid.off = (uint64_t)s[0]; 	/* XXX */
				dir->segment[x].oid.pool_uuid_lo = 0;
				dir->segment[x+1].oid.off = (uint64_t)s[1];
				dir->segment[x+1].oid.pool_uuid_lo = 0;
			}
			else{
				dir->segment[x].oid.off = (uint64_t)s[1];
				dir->segment[x].oid.pool_uuid_lo = 0;
				dir->segment[x-1].oid.off = (uint64_t)s[0];   /* XXX */
				dir->segment[x-1].oid.pool_uuid_lo = 0;
			}
			dir->unlock();

			if(s[0]->local_depth >> (8*sizeof(size_t)-1)){
//				s[0]->sibling[0] = s[1];
				s[0]->local_depth = (s[0]->local_depth & DEPTH_MASK) + 1;
			}
			else{
//				s[0]->sibling[1] = s[1];
				s[0]->local_depth = (s[0]->local_depth | SIBLING_MASK) + 1;
			}
			/* release target segment exclusive lock */
			s[0]->sema = 0;
		}
		else{
			int stride = pow(2, dir->depth - target_local_depth);
			auto loc = x - (x%stride);
			for(int i=0; i<stride/2; ++i){
				dir->segment[loc+stride/2+i].oid.off = (uint64_t)s[1];
				dir->segment[loc+stride/2+i].oid.pool_uuid_lo = 0;
			}
			dir->unlock();
			if(s[0]->local_depth >> (8*sizeof(size_t)-1)){
//				s[0]->sibling[0] = s[1];
				s[0]->local_depth = (s[0]->local_depth & DEPTH_MASK) + 1;
			}
			else{
//				D_RW(s[0])->sibling[1] = s[1];
				s[0]->local_depth = (s[0]->local_depth | SIBLING_MASK) + 1;
			}
			/* release target segment exclusive lock */
			s[0]->sema = 0;
		}
	}
	std::this_thread::yield();
	goto RETRY;
}

bool CCEH::Delete(Key_t& key){
	return false;
}

Value_t CCEH::Get(Key_t& key){
	auto f_hash = hash_funcs[0](&key, sizeof(Key_t), f_seed);
	auto f_idx = (f_hash & kMask) * kNumPairPerCacheLine;

RETRY:
	while(dir->sema < 0){
		asm("nop");
	}

	auto x = (f_hash >> (8*sizeof(f_hash) - dir->depth));
	auto target = dir->segment[x];
	auto isDRAM = target.oid.pool_uuid_lo == 0  ?  true : false;

	if(!D_RO(target)){
		std::this_thread::yield();
		goto RETRY;
	}

	/* acquire segment shared lock */
	if(!D_RW(target)->lock()){
		std::this_thread::yield();
		goto RETRY;
	}

	auto target_check = (f_hash >> (8*sizeof(f_hash) - dir->depth));
	if(D_RO(target) != D_RO(dir->segment[target_check])){
		D_RW(target)->unlock();
		std::this_thread::yield();
		goto RETRY;
	}

	struct Segment *_target = D_RW(target); 	/* XXX: Change below with this value?? */

	for(int i=0; i<kNumPairPerCacheLine*kNumCacheLine; ++i){
		auto loc = (f_idx+i) % Segment::kNumSlot;
		if(D_RO(target)->bucket[loc].key == key){
			Value_t v = D_RO(target)->bucket[loc].value;
			/* key found, release segment shared lock */
			D_RW(target)->unlock();

			/* LRU */
			ca->refer( (uint64_t) _target );

			return v;
		}
	}

	auto s_hash = hash_funcs[2](&key, sizeof(Key_t), s_seed);
	auto s_idx = (s_hash & kMask) * kNumPairPerCacheLine;

	for(int i=0; i<kNumPairPerCacheLine*kNumCacheLine; ++i){
		auto loc = (s_idx+i) % Segment::kNumSlot;
		if(D_RO(target)->bucket[loc].key == key){
			Value_t v = D_RO(target)->bucket[loc].value;
			D_RW(target)->unlock();

			/* LRU */
			ca->refer( (uint64_t) _target );

			return v;
		}
	}

	/* key not found, release segment shared lock */ 
	D_RW(target)->unlock();
	return NONE;
}

/*
   CCEH::Recovery
   -- this function recovers CCEH hashtable traversing from root to tail segment.
   -- all the previously used pmem pools should be opened successfully before calling this function.
   */
bool CCEH::Recovery(void){
	std::vector<std::pair<TOID(struct Segment), size_t>> segments_info;

	TOID(struct Segment_root) root = POBJ_ROOT(pop[0], struct Segment_root);
	TOID(struct Segment) cur = RD_RW(root)->segment;
	size_t cur_depth = D_RO(cur)->local_depth & DEPTH_MASK;
	size_t dir_depth = cur_depth; 

	/* nothing to recover -- root is null */
	if(TOID_IS_NULL(cur)){
		return false;
	}

	while(!TOID_IS_NULL(cur)){
		segments_info.push_back(make_pair(cur, (D_RO(cur)->local_depth & DEPTH_MASK)));
		if(dir_depth < (D_RO(cur)->local_depth & DEPTH_MASK)){
			dir_depth = (D_RO(cur)->local_depth & DEPTH_MASK);
		}
		cur = D_RO(cur)->sibling[(D_RO(cur)->local_depth >> (8*sizeof(size_t)-1))];
	}

	int from = 0;
	int to = 0;
	dir = new Directory(dir_depth);
	for(auto& iter: segments_info){
		int stride = pow(2, (dir_depth - iter.second));
		to = from + stride;
		for(int i=from; i<to; ++i){
			dir->segment[i] = iter.first;
		}
		from = to;
		int node_id = GetNodeID(iter.first);
		segments_in_node[node_id]++;
	}

	return true;
}

double CCEH::Utilization(void){
	size_t sum = 0;
	size_t cnt = 0;
	for(int i=0; i<dir->capacity; ++cnt){
		auto target = dir->segment[i];
		int stride = pow(2, dir->depth - (D_RO(target)->local_depth & DEPTH_MASK));
		auto pattern = (i >> (dir->depth - (D_RO(target)->local_depth & DEPTH_MASK)));
		for(unsigned j=0; j<Segment::kNumSlot; ++j){
			auto f_hash = hash_funcs[0](&D_RO(target)->bucket[j].key, sizeof(Key_t), f_seed);
			if(((f_hash >> (8*sizeof(f_hash)-D_RO(target)->local_depth)) == pattern) && (D_RO(target)->bucket[j].key != INVALID)){
				sum++;
			}
		}
		i += stride;
	}
	return ((double)sum) / ((double)cnt * Segment::kNumSlot)*100.0;
}

vector<unsigned> CCEH::Freqs(void){
	vector<unsigned> freqs;
	for(int i = 0; i < NUM_NUMA; ++i){
		freqs.push_back(freq[i]);
	}
	return freqs;
}

size_t CCEH::Capacity(void){
	size_t cnt = 0;
	for(int i=0; i<dir->capacity; cnt++){
		auto target = dir->segment[i];
		int stride = pow(2, dir->depth - (D_RO(target)->local_depth & DEPTH_MASK));
		i += stride;
	}

	return cnt * Segment::kNumSlot;
}

vector<size_t> CCEH::SegmentLoads(void){
	vector<size_t> loads;
	for(int i=0; i<NUM_NUMA; ++i){
		loads.push_back(segments_in_node[i]);
	}
	return loads;
}

size_t CCEH::Depth(void){
	return dir->depth;
}

#ifdef LRFU
vector<double> CCEH::Metrics(void){
	vector<double> metrics;
	for (int i = 0; i < NUM_NUMA ; ++i) {
		metrics.push_back(lrfu[i].crf);
	}
	return metrics;
}
#else
vector<double> CCEH::Metrics(void){
	vector<double> metrics;
	for (int i = 0; i < NUM_NUMA ; ++i) {
		metrics.push_back(0);
	}
	return metrics;
}
#endif

// for debugging
Value_t CCEH::FindAnyway(Key_t& key){
	for(size_t i=0; i<dir->capacity; ++i){
		for(size_t j=0; j<Segment::kNumSlot; ++j){
			if(D_RO(dir->segment[i])->bucket[j].key == key){
				cout << "segment(" << i << ")" << endl;
				cout << "global_depth(" << dir->depth << "), local_depth(" << (D_RO(dir->segment[i])->local_depth & DEPTH_MASK) << ")" << endl;
				cout << "pattern: " << bitset<sizeof(int64_t)>(i >> (dir->depth - (D_RO(dir->segment[i])->local_depth & DEPTH_MASK))) << endl;
				cout << "Key MSB: " << bitset<sizeof(int64_t)>(h(&key, sizeof(key)) >> (8*sizeof(key) - (D_RO(dir->segment[i])->local_depth & DEPTH_MASK))) << endl;
				return D_RO(dir->segment[i])->bucket[j].value;
			}
		}
	}
	return NONE;
}
