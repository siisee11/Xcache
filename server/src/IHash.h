#ifndef HASH_INTERFACE_H_
#define HASH_INTERFACE_H_

#define CAS(_p, _u, _v)  (__atomic_compare_exchange_n (_p, _u, _v, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))

#include "util/timer.h"
#include "util/pair.h"

class IHash {
  public:
    IHash(void) = default;
    ~IHash(void) = default;
//    virtual bool InsertOnly(Key_t&, Value_t) = 0;
	virtual int GetNodeID(Key_t&) = 0;
    virtual void Insert(Key_t&, Value_t) = 0;
    virtual bool Delete(Key_t&) = 0;
    virtual Value_t Get(Key_t&) = 0;
    virtual Value_t FindAnyway(Key_t&) = 0;
    virtual double Utilization(void) = 0;
    virtual size_t Capacity(void) = 0;
	virtual bool Recovery(void) = 0;
	virtual std::vector<unsigned> Freqs(void) = 0;
	virtual	std::vector<size_t> SegmentLoads(void) = 0;
	virtual	std::vector<double> Metrics(void) = 0;
};


#endif  // _HASH_INTERFACE_H_
