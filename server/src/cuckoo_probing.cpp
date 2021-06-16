#include <iostream>
#include <cstring>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include "util/persist.h"
#include "util/hash.h"
#include "cuckoo_probing.h"

LinearProbingHash::LinearProbingHash(void)
	: capacity{0}, dict{nullptr} { }

LinearProbingHash::LinearProbingHash(size_t _capacity)
	: capacity{_capacity}, dict{new Pair[capacity]}
{
	locksize = 16;
	nlocks = (capacity)/locksize+1;
	mutex = new std::shared_mutex[nlocks];
}

LinearProbingHash::~LinearProbingHash(void) {
	if (dict != nullptr) delete[] dict;
}


/*
 * cluster (always sorted)
 * -----------------------------------------
 * |  cuckoo-ed   |  old      ->      new  |
 * -----------------------------------------
 *
 * return deleted key
 */
Key_t LinearProbingHash::Insert(Key_t& key, Value_t value) {
	using namespace std;
	auto key_hash = h(&key, sizeof(key));

	auto slot = key_hash % capacity;
	auto off = slot % locksize;
	auto firstIndex = slot - off;
	unique_lock<shared_mutex> lock(mutex[slot/locksize]);
	for ( int j = 0 ; j < locksize - 1; j++ ) {
		slot = firstIndex + j;

		// if there is available slot, insert and return
		if (dict[slot].key == INVALID) {
			dict[slot].value = value;
			mfence();
			dict[slot].key = key;
			clflush((char*)&dict[slot].key, sizeof(Pair));
			auto _size = size;
			while (!CAS(&size, &_size, _size+1)) {
				_size = size;
			}
			return -1;
		}
	}

	// If there is no available slot. 
	// If first element is cuckoo-ed Pair,
	// Delete first element of this cluster and Insert new element at head.
	if ( (uint64_t)dict[firstIndex].value & cuckooBit ) {
		auto deleteKey = dict[firstIndex].key;
		for (int j = 0 ; j < locksize -1 ; j++) {
			auto target = firstIndex + j;
			dict[target].key = dict[target + 1].key;
			dict[target].value = dict[target + 1].value;
		}

		dict[firstIndex + locksize - 1].key = key;
		dict[firstIndex + locksize - 1].value = value;	
		clflush((char*)&dict[firstIndex].key, sizeof(Pair) * locksize);
		return deleteKey;
	}
	
	// Cuckoo (Delete oldest one in first hash cluster, and move it to next hash cluster)
	// Delete first element and shift.
	// Insert new element at tail.
	Pair cuckooPair = dict[firstIndex];
	for (int j = 0 ; j < locksize -1 ; j++) {
		auto target = firstIndex + j;
		dict[target].key = dict[target + 1].key;
		dict[target].value = dict[target + 1].value;
	}
	dict[firstIndex + locksize - 1].key = key;
	dict[firstIndex + locksize - 1].value = value;	
	clflush((char*)&dict[firstIndex].key, sizeof(Pair) * locksize);

	// Check Next hash position.
	auto next_hash = hash_funcs[0](&key, sizeof(key), 951125);
	slot = next_hash % capacity;
	off = slot % locksize;
	firstIndex = slot - off;
	for ( int j = 0 ; j < locksize - 1; j++ ) {
		slot = firstIndex + j;

		// +-----------------------+    +-----------------------+
		// | a | b | c | INV | ... | -> | new | a | b | c | ... |
		// +-----------------------+    +-----------------------+
		// if there is available slot, shift left element to the right.

		if (dict[slot].key == INVALID) {
			for (int i = j ; i > 0 ; i-- ) {
				dict[firstIndex + i].value = dict[firstIndex + i - 1].value;
				mfence();
				dict[firstIndex + i].key = dict[firstIndex + i - 1].key;
			}
			dict[firstIndex].key = cuckooPair.key;
			mfence();
			dict[firstIndex].value = (Value_t)((uint64_t)cuckooPair.value | cuckooBit);
			clflush((char*)&dict[firstIndex].key, sizeof(Pair) * locksize);
			auto _size = size;
			while (!CAS(&size, &_size, _size+1)) {
				_size = size;
			}

			return -1;
		}
	}

	// Delete first element of this cluster.
	// Insert new element at head.
	auto deleteKey = dict[firstIndex].key;
	dict[firstIndex].key = cuckooPair.key;
	dict[firstIndex].value = (Value_t)((uint64_t)cuckooPair.value | cuckooBit);
	clflush((char*)&dict[firstIndex].key, sizeof(Pair));
	
	return deleteKey;
}

bool LinearProbingHash::InsertOnly(Key_t& key, Value_t value) {
	auto key_hash = h(&key, sizeof(key)) % capacity;
	auto loc = getLocation(key_hash, capacity, dict);
	if (loc == INVALID) {
		return false;
	} else {
		dict[loc].value = value;
		mfence();
		dict[loc].key = key;
		clflush((char*)&dict[loc], sizeof(Pair));
		size++;
		return true;
	}
}

bool LinearProbingHash::Delete(Key_t& key) {
	return false;
}

Value_t LinearProbingHash::Get(Key_t& key) {
	auto key_hash = h(&key, sizeof(key)) % capacity;
	auto loc = key_hash % capacity; // target location of key
	auto off = loc % locksize;
	auto firstIndex = loc - off;
	{
		std::shared_lock<std::shared_mutex> lock(mutex[loc/locksize]);
		for (int i = 0; i < locksize - 1; ++i) {
			auto id = firstIndex + i;
			if (dict[id].key == key) return std::move(dict[id].value);
		}
	}

	// Check Next hash position.
	auto next_hash = hash_funcs[0](&key, sizeof(key),951125);
	loc = next_hash % capacity; // target location of key
	off = loc % locksize;
	firstIndex = loc - off;
	{
		std::shared_lock<std::shared_mutex> lock(mutex[loc/locksize]);
		for (int i = 0; i < locksize - 1; ++i) {
			auto id = firstIndex + i;
			if (dict[id].key == key) {
				return std::move((Value_t)((uint64_t)dict[id].value & ~cuckooBit));
			}
		}
	}

	return NONE;
}

void LinearProbingHash::Insert_extent(Key_t, uint64_t, uint64_t, Value_t) {
	return ;
}
Value_t LinearProbingHash::Get_extent(Key_t&, uint64_t) {
	return NONE;
}

Value_t LinearProbingHash::FindAnyway(Key_t&) {
	return NONE;
}

double LinearProbingHash::Utilization(void) {
	size_t size = 0;
	for (size_t i = 0; i < capacity; ++i) {
		if (dict[i].key != INVALID) {
			++size;
		}
	}
	return ((double)size)/((double)capacity)*100;
}

size_t LinearProbingHash::getLocation(size_t hash_value, size_t _capacity, Pair* _dict) {
	Key_t LOCK = INVALID;
	size_t cur = hash_value;
	size_t i = 0;
FAILED:
	while (_dict[cur].key != INVALID) {
		cur = (cur + 1) % _capacity;
		++i;
		if (!(i < capacity)) {
			return INVALID;
		}
	}
	if (CAS(&_dict[cur].key, &LOCK, SENTINEL)) {
		return cur;
	} else {
		goto FAILED;
	}
}

void LinearProbingHash::resize(size_t _capacity) {
	std::unique_lock<std::shared_mutex> *lock[nlocks];
	for(int i=0; i<nlocks; i++){
		lock[i] = new std::unique_lock<std::shared_mutex>(mutex[i]);
	}
	int prev_nlocks = nlocks;
	nlocks = _capacity/locksize+1;
	std::shared_mutex* old_mutex = mutex;

	Pair* newDict = new Pair[_capacity];
	for (size_t i = 0; i < capacity; i++) {
		if (dict[i].key != INVALID) {
			auto key_hash = h(&dict[i].key, sizeof(Key_t)) % _capacity;
			auto loc = getLocation(key_hash, _capacity, newDict);
			newDict[loc].key = dict[i].key;
			newDict[loc].value = dict[i].value;
		}
	}
	mutex = new std::shared_mutex[nlocks];
	clflush((char*)&newDict[0], sizeof(Pair)*_capacity);
	old_cap = capacity;
	old_dic = dict;
	clflush((char*)&old_cap, sizeof(size_t));
	clflush((char*)&old_dic, sizeof(Pair*));
	dict = newDict;
	clflush((char*)&dict, sizeof(void*));
	capacity = _capacity;
	clflush((char*)&capacity, sizeof(size_t));
	auto tmp = old_dic;
	old_cap = 0;
	old_dic = nullptr;
	clflush((char*)&old_cap, sizeof(size_t));
	clflush((char*)&old_dic, sizeof(Pair*));

	delete [] tmp;
	for(int i=0; i<prev_nlocks; i++) {
		delete lock[i];
	}
	delete[] old_mutex;
}
