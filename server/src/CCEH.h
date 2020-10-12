#ifndef CCEH_H_
#define CCEH_H_

#include <cstring>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <pthread.h>
#include <libpmemobj.h>

#define TOID_ARRAY(x) TOID(x)

typedef size_t Key_t;
typedef const char* Value_t;

const Key_t SENTINEL = -2;
const Key_t INVALID = -1;
const Value_t NONE = 0x0;

struct Pair{
    Key_t key;
    Value_t value;
};

class CCEH;
struct Directory;
struct Segment;
POBJ_LAYOUT_BEGIN(HashTable);
POBJ_LAYOUT_ROOT(HashTable, CCEH);
POBJ_LAYOUT_TOID(HashTable, struct Directory);
POBJ_LAYOUT_TOID(HashTable, struct Segment);
POBJ_LAYOUT_TOID(HashTable, TOID(struct Segment));
POBJ_LAYOUT_END(HashTable);

constexpr size_t kSegmentBits = 8;
constexpr size_t kMask = (1 << kSegmentBits)-1;
constexpr size_t kShift = kSegmentBits;
constexpr size_t kSegmentSize = (1 << kSegmentBits) * 16 * 4;
constexpr size_t kNumPairPerCacheLine = 4;
constexpr size_t kNumCacheLine = 8;

struct Segment{
    static const size_t kNumSlot = kSegmentSize/sizeof(Pair);
    static const size_t numLocks = kNumSlot/kNumSlot;

    Segment(void){ }
    ~Segment(void){ }

    void initSegment(void){
	for(int i=0; i<kNumSlot; ++i){
	    pair[i].key = INVALID;
	}
	local_depth = 0;
	lock = new pthread_rwlock_t[numLocks];
	for(int i=0; i<numLocks; ++i){
	    pthread_rwlock_init(&lock[i], NULL);
	}
    }

    void initSegment(size_t depth){
	for(int i=0; i<kNumSlot; ++i){
	    pair[i].key = INVALID;
	}
	local_depth = depth;
	lock = new pthread_rwlock_t[numLocks];
	for(int i=0; i<numLocks; ++i){
	    pthread_rwlock_init(&lock[i], NULL);
	}
    }

    int Insert(PMEMobjpool*, Key_t&, Value_t, size_t, size_t);
    void Insert4split(Key_t&, Value_t, size_t);
    TOID(struct Segment)* Split(PMEMobjpool*);
    size_t numElement(void);

    Pair pair[kNumSlot];
    pthread_rwlock_t* lock;
    size_t local_depth;
    
};

struct Directory{
    static const size_t kDefaultDepth = 10;
    /* if you want more fine-grained locking in directory, decrease the locksize here */
    static const size_t locksize = 2;
    //static const size_t locksize = 64;

    TOID_ARRAY(TOID(struct Segment)) segment;	
    pthread_rwlock_t* lock;		
    size_t capacity;		
    size_t depth;	
    size_t nlocks;

    Directory(void){ }

    ~Directory(void){ }

    void initDirectory(void){
	depth = kDefaultDepth;
	capacity = pow(2, depth);
	nlocks = capacity/locksize + 1;
	lock = new pthread_rwlock_t[nlocks];
	for(int i=0; i<nlocks; ++i){
	    pthread_rwlock_init(&lock[i], NULL);
	}
    }

    void initDirectory(size_t _depth){
	depth = _depth;
	capacity = pow(2, _depth);
	nlocks = capacity/locksize + 1;
	lock = new pthread_rwlock_t[nlocks];
	for(int i=0; i<nlocks; ++i){
	    pthread_rwlock_init(&lock[i], NULL);
	}
    }
};

class CCEH{
    public:
	CCEH(void){ }
	~CCEH(void){ }
	void initCCEH(PMEMobjpool*);
	void initCCEH(PMEMobjpool*, size_t);

	void Insert(PMEMobjpool*, Key_t&, Value_t);
	bool InsertOnly(PMEMobjpool*, Key_t&, Value_t);	
	bool Delete(Key_t&);
	Value_t Get(Key_t&);
	Value_t FindAnyway(Key_t&);
	bool RecoverLocks(void);

	double Utilization(void);
	size_t Capacity(void);
	bool Recovery(void);

    private:
	//PMEMobjpool* pop;
	TOID(struct Directory) dir;
};

#endif

