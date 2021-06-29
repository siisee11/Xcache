#ifndef LINEAR_HASH_H_
#define LINEAR_HASH_H_

#include <stddef.h>
#include <mutex>
#include <shared_mutex>
#include "util/pair.h"
#include "IHash.h"

class LinearProbingHash : public IHash {
	const float kResizingFactor = 2;
	const float kResizingThreshold = 0.95;
	public:
	LinearProbingHash(void);
	LinearProbingHash(size_t);
	~LinearProbingHash(void);
	Key_t Insert(Key_t&, Value_t);
	bool InsertOnly(Key_t&, Value_t);
	bool Delete(Key_t&);
	Value_t Get(Key_t&);
	double Utilization(void);

	void Insert_extent(Key_t, uint64_t, uint64_t, Value_t);
	Value_t Get_extent(Key_t&, uint64_t);
	Value_t FindAnyway(Key_t&);

	bool Recovery(void) {
		return false;
	}

	size_t Capacity(void) {
		return capacity;
	}

	void* operator new[] (size_t size) {
		void *ret;
		if (posix_memalign(&ret, 64, size)) ret = NULL;
		return ret;
	}

	void* operator new(size_t size) {
		void *ret;
		if (posix_memalign(&ret, 64, size)) ret = NULL;
		return ret;
	}

	private:
	void resize(size_t);
	size_t getLocation(size_t, size_t, Pair*);

	size_t capacity;
	Pair* dict;

	size_t old_cap;
	Pair* old_dic;

	size_t size = 0;

	int resizing_lock = 0;
	std::shared_mutex *mutex;
	int nlocks;
	int locksize;
};


#endif  // LINEAR_HASH_H_
