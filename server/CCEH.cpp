#include <iostream>
#include <thread>
#include <bitset>
#include <cassert>
#include <unordered_map>
#include <vector>
#include "CCEH.h"
#include "hash.h"
#include "util.h"

using namespace std;

void Segment::Insert4split(Key_t& key, Value_t value, size_t loc){
    for(int i=0; i<kNumPairPerCacheLine*kNumCacheLine; ++i){
	auto slot = (loc+i) % kNumSlot;
	if(pair[slot].key == INVALID){
	    pair[slot].key = key;
	    pair[slot].value = value;
	    return;
	}
    }
    cerr << "[" << __func__ << "]: something wrong -- need to adjust linear probing distance" << endl;
}

TOID(struct Segment)* Segment::Split(PMEMobjpool* pop){
#ifdef INPLACE
    TOID(struct Segment)* split = new TOID(struct Segment)[2];
    split[0] = pmemobj_oid(this);
    POBJ_ALLOC(pop, &split[1], struct Segment, sizeof(struct Segment), NULL, NULL);
    D_RW(split[1])->initSegment(local_depth+1);

    auto pattern = ((size_t)1 << (sizeof(Key_t)*8 - local_depth - 1));
    for(int i=0; i<kNumSlot; ++i){
	auto key_hash = h(&pair[i].key, sizeof(Key_t));
	if(key_hash & pattern){
	    D_RW(split[1])->Insert4split(pair[i].key, pair[i].value, (key_hash & kMask)*kNumPairPerCacheLine);
	}
    }

    pmemobj_persist(pop, (char*)D_RO(split[1]), sizeof(struct Segment));
    return split;
#else
    TOID(struct Segment)* split = new TOID(struct Segment)[2];
    POBJ_ALLOC(pop, &split[0], struct Segment, sizeof(struct Segment), NULL, NULL);
    POBJ_ALLOC(pop, &split[1], struct Segment, sizeof(struct Segment), NULL, NULL);
    D_RW(split[0])->initSegment(local_depth+1);
    D_RW(split[1])->initSegment(local_depth+1);

    auto pattern = ((size_t)1 << (sizeof(Key_t)*8 - local_depth - 1));
    for(int i=0; i<kNumSlot; ++i){
	auto key_hash = h(&pair[i].key, sizeof(Key_t));
	if(key_hash & pattern){
	    D_RW(split[1])->Insert4split(pair[i].key, pair[i].value, (key_hash & kMask)*kNumPairPerCacheLine);
	}
	else{
	    D_RW(split[0])->Insert4split(pair[i].key, pair[i].value, (key_hash & kMask)*kNumPairPerCacheLine);
	}
    }

    pmemobj_persist(pop, (char*)D_RO(split[0]), sizeof(struct Segment));
    pmemobj_persist(pop, (char*)D_RO(split[1]), sizeof(struct Segment));

    return split;
#endif
}


void CCEH::initCCEH(PMEMobjpool* pop){
    POBJ_ALLOC(pop, &dir, struct Directory, sizeof(struct Directory), NULL, NULL);
    D_RW(dir)->initDirectory();
    POBJ_ALLOC(pop, &D_RW(dir)->segment, TOID(struct Segment), sizeof(TOID(struct Segment))*D_RO(dir)->capacity, NULL, NULL);

    for(int i=0; i<D_RO(dir)->capacity; ++i){
	POBJ_ALLOC(pop, &D_RO(D_RO(dir)->segment)[i], struct Segment, sizeof(struct Segment), NULL, NULL);
	D_RW(D_RW(D_RW(dir)->segment)[i])->initSegment();
    }
}

void CCEH::initCCEH(PMEMobjpool* pop, size_t initCap){
    POBJ_ALLOC(pop, &dir, struct Directory, sizeof(struct Directory), NULL, NULL);
    D_RW(dir)->initDirectory(static_cast<size_t>(log2(initCap)));
    POBJ_ALLOC(pop, &D_RW(dir)->segment, TOID(struct Segment), sizeof(TOID(struct Segment))*D_RO(dir)->capacity, NULL, NULL);

    for(int i=0; i<D_RO(dir)->capacity; ++i){
	POBJ_ALLOC(pop, &D_RO(D_RO(dir)->segment)[i], struct Segment, sizeof(struct Segment), NULL, NULL);
	D_RW(D_RW(D_RW(dir)->segment)[i])->initSegment(static_cast<size_t>(log2(initCap)));
    }
}
 
void CCEH::Insert(PMEMobjpool* pop, Key_t& key, Value_t value){
    auto key_hash = h(&key, sizeof(Key_t));
    auto y = (key_hash & kMask) * kNumPairPerCacheLine;

RETRY:
    auto dir_depth = D_RO(dir)->depth;

    auto idx = (key_hash >> (8*sizeof(key_hash) - dir_depth));
    auto slot = idx/D_RO(dir)->locksize;

    /* acquire directory shared lock */
    if(pthread_rwlock_tryrdlock(&D_RO(dir)->lock[slot]) != 0){
	goto RETRY;
    }

    auto x = idx;
    auto target = D_RO(D_RO(dir)->segment)[x];

    /* double check -- we acquired dir_depth without lock */
    if(dir_depth != D_RO(dir)->depth){
	pthread_rwlock_unlock(&D_RO(dir)->lock[slot]);
	std::this_thread::yield();
	goto RETRY;
    }

    /* acquire segment exclusive lock */
    if(pthread_rwlock_trywrlock(&D_RW(target)->lock[0]) != 0){
	pthread_rwlock_unlock(&D_RW(dir)->lock[slot]);
	std::this_thread::yield();
	goto RETRY;
    }
    pthread_rwlock_unlock(&D_RO(dir)->lock[slot]);

    auto pattern = (x >> (dir_depth - D_RO(target)->local_depth));
    for(unsigned i=0; i<kNumPairPerCacheLine * kNumCacheLine; ++i){
	auto loc = (y + i) % Segment::kNumSlot;
	/* validity check for entry keys */
	if(((h(&D_RO(target)->pair[loc].key, sizeof(Key_t)) >> (8*sizeof(key_hash)-D_RO(target)->local_depth)) != pattern) || (D_RO(target)->pair[loc].key == INVALID)){
	    D_RW(target)->pair[loc].value = value;
	    mfence();
	    D_RW(target)->pair[loc].key = key;
	    pmemobj_persist(pop, (char*)&D_RO(target)->pair[loc], sizeof(Pair));
	    /* release segment exclusive lock */
	    pthread_rwlock_unlock(&D_RO(target)->lock[0]);
	    return;
	}
    }

    // COLLISION !!

    auto target_local_depth = D_RO(target)->local_depth;
    /* need to split segment but release the exclusive lock first to avoid deadlock */
    pthread_rwlock_unlock(&D_RO(target)->lock[0]);

    /* we do not hold any lock here */
    while(pthread_rwlock_trywrlock(&D_RO(dir)->lock[0]) != 0){
	asm("nop");
    }

    /* need to check whether the directory has been doubled */
    if(dir_depth != D_RO(dir)->depth){
	pthread_rwlock_unlock(&D_RO(dir)->lock[0]);
	std::this_thread::yield();
	goto RETRY;
    }

    pthread_rwlock_wrlock(&D_RO(target)->lock[0]);
    /* need to check whether the target segment has been split */
#ifdef INPLACE
    if(target_local_depth != D_RO(target)->local_depth){
	pthread_rwlock_unlock(&D_RO(target)->lock[0]);
	pthread_rwlock_unlock(&D_RO(dir)->lock[0]);
	std::this_thread::yield();
	goto RETRY;
    }
#else
    if(target_local_depth != D_RO(D_RO(D_RO(dir)->segment)[x])->local_depth){
	pthread_rwlock_unlock(&D_RO(target)->lock[0]);
	pthread_rwlock_unlock(&D_RO(dir)->lock[0]);
	std::this_thread::yield();
	goto RETRY;
    }
#endif

    /* need to double the directory */
    if(target_local_depth == dir_depth){
	for(int i=1; i<D_RO(dir)->nlocks; ++i){
	    pthread_rwlock_wrlock(&D_RO(dir)->lock[i]);
	}

	TOID(struct Segment)* s = D_RW(target)->Split(pop);

	auto dir_old = dir;
	TOID_ARRAY(TOID(struct Segment)) d = D_RO(dir)->segment;
	TOID(struct Directory) _dir;
	POBJ_ALLOC(pop, &_dir, struct Directory, sizeof(struct Directory), NULL, NULL);
	POBJ_ALLOC(pop, &D_RO(_dir)->segment, TOID(struct Segment), sizeof(TOID(struct Segment))*D_RO(dir)->capacity*2, NULL, NULL);
	D_RW(_dir)->initDirectory(dir_depth+1);

	for(int i=0; i<D_RO(dir)->capacity; ++i){
	    if(i == x){
		D_RW(D_RW(_dir)->segment)[2*i] = s[0];
		D_RW(D_RW(_dir)->segment)[2*i+1] = s[1];
	    }
	    else{
		D_RW(D_RW(_dir)->segment)[2*i] = D_RO(d)[i];
		D_RW(D_RW(_dir)->segment)[2*i+1] = D_RO(d)[i];
	    }
	}

	pmemobj_persist(pop, (char*)&D_RO(D_RO(_dir)->segment)[0], sizeof(TOID(struct Segment))*D_RO(_dir)->capacity);
	pmemobj_persist(pop, (char*)&_dir, sizeof(struct Directory));
	dir = _dir;
	pmemobj_persist(pop, (char*)&dir, sizeof(TOID(struct Directory)));
#ifdef INPLACE
	D_RW(s[0])->local_depth++;
	pmemobj_persist(pop, (char*)&D_RO(s[0])->local_depth, sizeof(size_t));
	/* release segment exclusive lock */
	pthread_rwlock_unlock(&D_RO(s[0])->lock[0]);
#endif

	/*
	for(int i=0; i<D_RO(dir_old)->nlocks; ++i){
	    pthread_rwlock_unlock(&D_RO(dir_old)->lock[i]);
	}*/

	/* TBD */
	// POBJ_FREE(&dir_old);

    }
    else{ // normal split
	if(dir_depth == target_local_depth + 1){
	    /* two directory entries are pointing to the current target segment,
	       so we need to acquire just one lock here */

	    if(x/D_RO(dir)->locksize != 0){
		pthread_rwlock_wrlock(&D_RO(dir)->lock[x/D_RO(dir)->locksize]);
		pthread_rwlock_unlock(&D_RO(dir)->lock[0]);
	    }

	    TOID(struct Segment)* s = D_RW(target)->Split(pop);

	    if(x%2 == 0){
		D_RW(D_RW(dir)->segment)[x+1] = s[1];
#ifdef INPLACE
		pmemobj_persist(pop, (char*)&D_RO(D_RO(dir)->segment)[x+1], sizeof(TOID(struct Segment)));
#else
		mfence();
		D_RW(D_RW(dir)->segment)[x] = s[0];
		pmemobj_persist(pop, (char*)&D_RO(D_RO(dir)->segment)[x], sizeof(TOID(struct Segment))*2);
#endif
	    }
	    else{
		D_RW(D_RW(dir)->segment)[x] = s[1];
#ifdef INPLACE
		pmemobj_persist(pop, (char*)&D_RO(D_RO(dir)->segment)[x], sizeof(TOID(struct Segment)));
#else
		mfence();
		D_RW(D_RW(dir)->segment)[x-1] = s[0];
		pmemobj_persist(pop, (char*)&D_RO(D_RO(dir)->segment)[x-1], sizeof(TOID(struct Segment))*2);
#endif
	    }

#ifdef INPLACE
	    D_RW(s[0])->local_depth++;
	    pmemobj_persist(pop, (char*)&D_RO(s[0])->local_depth, sizeof(size_t));
	    /* release target segment exclusive lock */
	    pthread_rwlock_unlock(&D_RO(s[0])->lock[0]);
#endif
	    /* release directory entry exclusive lock */
	    pthread_rwlock_unlock(&D_RO(dir)->lock[x/D_RO(dir)->locksize]);
	}
	else{
	    /* more than two directory entries are pointing to the current target
	       segment, so we need to acquire more locks depending on the stride
	       value and directory lock granularity */
	    int stride = pow(2, dir_depth - target_local_depth);
	    int from = (x - x%stride)/D_RO(dir)->locksize;
	    int to = (x - x%stride + stride)/D_RO(dir)->locksize + 1;

	    if(from != 0){
		for(int i=from; i<to; ++i){
		    pthread_rwlock_wrlock(&D_RO(dir)->lock[i]);
		}
		pthread_rwlock_unlock(&D_RO(dir)->lock[0]);
	    }
	    else{
		for(int i=from+1; i<to; ++i){
		    pthread_rwlock_wrlock(&D_RO(dir)->lock[i]);
		}
	    }

	    TOID(struct Segment)* s = D_RW(target)->Split(pop);

	    auto loc = x - (x%stride);
	    for(int i=0; i<stride/2; ++i){
		D_RW(D_RW(dir)->segment)[loc+stride/2+i] = s[1];
	    }
#ifdef INPLACE
	    pmemobj_persist(pop, (char*)&D_RO(D_RO(dir)->segment)[loc+stride/2], sizeof(TOID(struct Segment))*stride/2);
#else
	    for(int i=0; i<stride/2; ++i){
		D_RW(D_RW(dir)->segment)[loc+i] = s[0];
	    }
	    pmemobj_persist(pop, (char*)&D_RO(D_RO(dir)->segment)[loc], sizeof(TOID(struct Segment))*stride);
#endif

#ifdef INPLACE
	    D_RW(s[0])->local_depth++;
	    pmemobj_persist(pop, (char*)&D_RO(s[0])->local_depth, sizeof(size_t));
	    /* release target segment exclusive lock */
	    pthread_rwlock_unlock(&D_RO(s[0])->lock[0]);
#endif

	    for(int i=from; i<to; ++i){
		pthread_rwlock_unlock(&D_RO(dir)->lock[i]);
	    }
	}
    }
    std::this_thread::yield();
    goto RETRY;
}

bool CCEH::RecoverLocks(void){
    D_RW(dir)->lock = new pthread_rwlock_t[D_RO(dir)->nlocks];
    for(int i=0; i<D_RO(dir)->nlocks; ++i){
	if(pthread_rwlock_init(&D_RO(dir)->lock[i], NULL))
	    return false;
    }

    for(int i=0; i<D_RO(dir)->capacity;){
	auto target = D_RO(D_RO(dir)->segment)[i];
	int stride = pow(2, D_RO(dir)->depth - D_RO(target)->local_depth);

	D_RW(target)->lock = new pthread_rwlock_t[Segment::numLocks];
	for(int j=0; j<Segment::numLocks; ++j){
	    if(pthread_rwlock_init(&D_RO(target)->lock[j], NULL))
		return false;
	}
	i += stride;
    }
    return true;
}

bool CCEH::Delete(Key_t& key){
    return false;
}

Value_t CCEH::Get(Key_t& key){
    auto key_hash = h(&key, sizeof(key));
    auto y = (key_hash & kMask) * kNumPairPerCacheLine;

RETRY:
    auto dir_depth = D_RO(dir)->depth;

    auto idx = (key_hash >> (8*sizeof(key_hash) - dir_depth));
    auto slot = idx/D_RO(dir)->locksize;

    /* acquire directory entry shared lock */
    if(pthread_rwlock_tryrdlock(&D_RO(dir)->lock[slot]) != 0){
	goto RETRY;
    }

    /* need double check if directory has been doubled */
    if(dir_depth != D_RO(dir)->depth){
	pthread_rwlock_unlock(&D_RO(dir)->lock[slot]);
	goto RETRY;
    }

    auto x = idx;
    auto target = D_RO(D_RO(dir)->segment)[x];

#ifdef INPLACE
    /* acquire segment shared lock */
    if(pthread_rwlock_tryrdlock(&D_RO(target)->lock[0]) != 0){
	pthread_rwlock_unlock(&D_RO(dir)->lock[slot]);
	goto RETRY;
    }
#endif
    /* release directory entry shared lock */
    pthread_rwlock_unlock(&D_RO(dir)->lock[slot]);

    for(int i=0; i<kNumPairPerCacheLine*kNumCacheLine; ++i){
	auto loc = (y+i) % Segment::kNumSlot;
	if(D_RO(target)->pair[loc].key == key){
	    Value_t v = D_RO(target)->pair[loc].value;
#ifdef INPLACE
	    /* key found, release segment shared lock */
	    pthread_rwlock_unlock(&D_RO(target)->lock[0]);
#endif
	    return v;
	}
    }

#ifdef INPLACE
    /* key not found, release segment shared lock */ 
    pthread_rwlock_unlock(&D_RO(target)->lock[0]);
#endif
    return NONE;
}

double CCEH::Utilization(void){
    size_t sum = 0;
    size_t cnt = 0;
    for(int i=0; i<D_RO(dir)->capacity; ++cnt){
	auto target = D_RO(D_RO(dir)->segment)[i];
	int stride = pow(2, D_RO(dir)->depth - D_RO(target)->local_depth);
	auto pattern = (i >> (D_RO(dir)->depth - D_RO(target)->local_depth));
	for(unsigned j=0; j<Segment::kNumSlot; ++j){
	    auto key_hash = h(&D_RO(target)->pair[j].key, sizeof(Key_t));
	    if(((key_hash >> (8*sizeof(key_hash)-D_RO(target)->local_depth)) == pattern) && (D_RO(target)->pair[j].key != INVALID)){
		sum++;
	    }
	}
	i += stride;
    }
    return ((double)sum) / ((double)cnt * Segment::kNumSlot)*100.0;
}

size_t CCEH::Capacity(void){
    size_t cnt = 0;
    for(int i=0; i<D_RO(dir)->capacity; cnt++){
	auto target = D_RO(D_RO(dir)->segment)[i];
	int stride = pow(2, D_RO(dir)->depth - D_RO(target)->local_depth);
	i += stride;
    }

    return cnt * Segment::kNumSlot;
}

// for debugging
Value_t CCEH::FindAnyway(Key_t& key){
    for(size_t i=0; i<D_RO(dir)->capacity; ++i){
	for(size_t j=0; j<Segment::kNumSlot; ++j){
	    if(D_RO(D_RO(D_RO(dir)->segment)[i])->pair[j].key == key){
		return D_RO(D_RO(D_RO(dir)->segment)[i])->pair[j].value;
	    }
	}
    }
    return NONE;
}
