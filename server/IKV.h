#ifndef KVSTORE_INTERFACE_H_
#define KVSTORE_INTERFACE_H_

#define CAS(_p, _u, _v)  (__atomic_compare_exchange_n (_p, _u, _v, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))

#include "util/pair.h"
#include "util/timer.h"

class KVStore {
  public:
    KVStore(void) = default;
    ~KVStore(void) = default;
    virtual bool Insert(Key_t&, Value_t) = 0;
	virtual void InsertExtent(Key_t&, Value_t, uint64_t) = 0;
    virtual bool Delete(Key_t&) = 0;
    virtual Value_t Get(Key_t&) = 0;
	virtual Value_t GetExtent(Key_t&) = 0;
	virtual Value_t FindAnyway(Key_t&) = 0;
	virtual bool Recovery(void) = 0;
    virtual double Utilization(void) = 0;
    virtual size_t Capacity(void) = 0;
	virtual void PrintStats(void) = 0;
};


#endif  // _KVSTORE_INTERFACE_H_
