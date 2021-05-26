#include <iostream>
#include <cmath>
#include <thread>
#include <bitset>
#include <cassert>
#include <strings.h>

#include <algorithm>
#include <unordered_map>
#include <sys/types.h>

#include "util/persist.h"
#include "util/hash.h"
#include "static_hash.h"

#define EXTENT_MAX_HEIGHT 30

using namespace std;
extern size_t perfCounter;

using namespace std;

HashTable::HashTable(size_t size)
	: hashSize{size}, bucket{new Pair[size]}
{
}

HashTable::~HashTable(void)
{ }

void HashTable::Insert(Key_t& key, Value_t value) {
	int hKey = key % hashSize;

	auto ret = Get(key);

	if(ret != NONE){
		cout << "insert failed: " << key << " already exists in table" <<  endl;
	}
	else{
		if(bucket[hKey].key == INVALID){
			cout << "success: insert of " << key  << " into slot " << hKey <<  endl;
			bucket[hKey].key = key;
			bucket[hKey].value = value;
			return;
		}
		else if(bucket[hKey].key != INVALID){
			cout << "eviction needed" << endl;
		}
	}

	return;
}

Value_t HashTable::Get(Key_t& key) {
	int hKey = key % hashSize;
	if(bucket[hKey].key == key){
		cout << key << " found at index " << hKey << " in table h1" << endl;
		return bucket[hKey].value;
	}
	return NONE;
}

bool HashTable::Delete(Key_t& key){
	int hKey = key % hashSize;
	if(bucket[hKey].key == key){
		cout << key << " deleted from index " << hKey << " in table h1" << endl;
		bucket[hKey].key = -INVALID;

		return true;
	}
	cout << "delete failed: " << key << " Not found" << endl;
	return false;
}

// [ key, pointer to Extent ]
void HashTable::Insert_extent(Key_t key, uint64_t cluster_num,  uint64_t len, Value_t value){
	if (len <= 0) return;
	uint64_t subextent_size = 0;
	uint64_t order = 0;

	Key_t current_key = key + cluster_num;
	Pair *b = &bucket[current_key % hashSize];
	b->key = key;
	b->value = value;
	if(len == 1){
		return;
	} else if (current_key % 2 == 1) {
		subextent_size = 1;
	} else {
		if (current_key != 0) {
			order = ffs(current_key) - 1;
			subextent_size = 1 << (__builtin_ctz(min(len, (uint64_t) 1 << order)));
		} else {
			subextent_size = (len % 2 == 0) ? len / 2 : len / 2;
		}
	}
	Insert_extent(key, cluster_num + subextent_size, len - subextent_size, value);

	return;
}

Value_t HashTable::Get_extent(Key_t& key, uint64_t cluster_num){
	Key_t current_key = key + cluster_num;
	unsigned int mask = (1 << __builtin_ctz(current_key));
	while(true) {
		Pair b = bucket[current_key % hashSize];
		/*
		if (b.value->contains(key, cluster_num)){
			return b.value;
		}
		*/
		if (current_key == 0)
			break;
		current_key = current_key & ((mask << ffs(key)) & mask);
	}
	return NONE;
}

double HashTable::Utilization(void){
	size_t sum = 0;
	size_t cnt = 0;
	for(size_t i=0; i<hashSize; cnt++){
		auto target = bucket[i];
		if(target.key != INVALID){
				sum++;
		}
	}
	return ((double)sum) / ((double)cnt)*100.0;
}

size_t HashTable::Capacity(void) {
	return hashSize;
}

// for debugging
Value_t HashTable::FindAnyway(Key_t& key) {
	using namespace std;
	for (size_t i = 0; i < hashSize; ++i) {
		if (bucket[i].key == key) {
			cout << "bucket: " << i << endl;
			return bucket[i].value;
		}
	}
	return NONE;
}
