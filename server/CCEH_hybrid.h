#ifndef CCEH_H_
#define CCEH_H_

#include <cstring>
#include <cmath>
#include <vector>
#include <pthread.h>
#include <iostream>

#include "util/pair.h"
#include "ICCEH.h"
#include "variables.h"

constexpr size_t kSegmentBits = 8;
constexpr size_t kMask = (1 << kSegmentBits)-1;
constexpr size_t kShift = kSegmentBits;
constexpr size_t kSegmentSize = (1 << kSegmentBits) * 16 * 4;
constexpr size_t kNumPairPerCacheLine = 4;
constexpr size_t kNumCacheLine = 8;

struct Metric {
	unsigned atime;
	double crf; /* Combined Recency and Frequency */
};


struct Segment {
  static const size_t kNumSlot = kSegmentSize/sizeof(Pair);

  Segment(void)
  : local_depth{0}
  {  }

  Segment(size_t depth)
  :local_depth{depth}
  {  }

  ~Segment(void) {  }

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

  void reset(void){
      int64_t val = sema;
      while(!CAS(&sema, &val, 0)){
	  val = sema;
      }
  }

  void* operator new(size_t size) {
    void* ret;
	if (posix_memalign(&ret, 64, size) ) ret=NULL;
    return ret;
  }

  void* operator new[](size_t size) {
    void* ret;
	if (posix_memalign(&ret, 64, size) ) ret=NULL;
    return ret;
  }

  int Insert(Key_t&, Value_t, size_t, size_t);
  void Insert4split(Key_t&, Value_t, size_t);
  bool Put(Key_t&, Value_t, size_t);
  Segment** Split(void);
  size_t numElem(void); 

  Pair _[kNumSlot];
  int64_t sema = 0;
  size_t local_depth;
};

struct Directory {
  static const size_t kDefaultDepth = 10;
  Segment** _;
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

  void reset(void){
      int64_t val = sema;
      while(!CAS(&sema, &val, 0)){
	  val = sema;
      }
  }



  Directory(void) {
    depth = kDefaultDepth;
    capacity = pow(2, depth);
    _ = new Segment*[capacity];
    sema = 0;
  }

  Directory(size_t _depth) {
    depth = _depth;
    capacity = pow(2, depth);
    _ = new Segment*[capacity];
    sema = 0;
  }

  ~Directory(void) {
    delete [] _;
  }

  void SanityCheck(void*);
  void LSBUpdate(int, int, int, int, Segment**);
};

class CCEH : public ICCEH {
  public:
    CCEH(void);
    CCEH(size_t);
    ~CCEH(void);

	int GetNodeID(Key_t&);
	void Insert(Key_t&, Value_t);
	void Insert_extent(Key_t, Value_t, uint64_t);
    bool InsertOnly(Key_t&, Value_t);
    bool Delete(Key_t&);
    Value_t Get(Key_t&);
	Value_t Get_extent(Key_t&);
    Value_t FindAnyway(Key_t&);

    double Utilization(void);
    size_t Capacity(void);
    bool Recovery(void);

	std::vector<size_t> SegmentLoads(void);
	std::vector<unsigned> Freqs(void);
	std::vector<double> Metrics(void);

	void* operator new(size_t size) {
		void *ret;
		if (posix_memalign(&ret, 64, size) ) ret=NULL;
		return ret;
	}

  private:
    Directory* dir;
	size_t segments_in_node[NUM_NUMA];
	unsigned freq[NUM_NUMA];
	unsigned gtime;
	struct Metric lrfu[NUM_NUMA];
};

#endif  // EXTENDIBLE_PTR_H_
