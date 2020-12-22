#ifndef KVSTORE_INTERFACE_H_
#define KVSTORE_INTERFACE_H_

#define CAS(_p, _u, _v)  (__atomic_compare_exchange_n (_p, _u, _v, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))

#include "util/pair.h"
#include "util/timer.h"

class KVStore {
  public:
    KVStore(void) = default;
    ~KVStore(void) = default;
    virtual void Insert(Key_t&, Value_t, int, int) = 0;
    virtual bool Delete(Key_t&) = 0;
    virtual void Get(Key_t&, int, int) = 0;
    virtual Value_t Get(Key_t&, int) = 0;
	virtual int GetNodeID(Key_t&) = 0;
	virtual Value_t FindAnyway(Key_t&) = 0;
	virtual	std::vector<size_t> SegmentLoads(void) = 0;
	virtual bool WaitComplete(void) = 0;
	virtual bool Recovery(void) = 0;
    virtual double Utilization(void) = 0;
	virtual	std::vector<unsigned> Freqs(void) = 0;
    virtual size_t Capacity(void) = 0;
	virtual int GetFailedSearch(void) = 0;
	virtual void PrintStats(void) = 0;
	virtual void PrintStats(bool) = 0;

    Timer timer;
    double breakdown = 0;
};


#endif  // _KVSTORE_INTERFACE_H_
