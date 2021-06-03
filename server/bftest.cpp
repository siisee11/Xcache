#include <string>
#include <iostream>

#include "util/counting_bloom_filter.h"
#include "util/pair.h"

int main(int argc, char *argv[]){

	Key_t t[10000];
	for (int i = 0; i < 10000; i++ ) {
		t[i] = i * 13 + i;
	}

	CountingBloomFilter<Key_t> *bf;
	bf = new CountingBloomFilter<Key_t>(4, 100000);

	for (int i = 0; i < 9999; i++ ) {
		bf->Insert(t[i]);
	}

	if(!bf->Query(t[0])){
		std::cout << "Error: Query for first inserted element was false." << std::endl;
		return 1;
	}

	if(bf->Query(t[9999])){
		std::cout << "Error: Query for non-inserted element was true." << std::endl;
		return 1;
	}


	for (int i = 0; i < 9999; i++ ) {
		if (!bf->Query(t[i])) {
			std::cout << "Error: Query for inserted element was false." << std::endl;
		}
	}

	if(!bf->Delete(t[0])){
		std::cout << "Error: Failed to delete object." << std::endl;
		return 1;
	}

	if(bf->Query(t[0])){
		std::cout << "Error: Query for deleted object was true." << std::endl;
		return 1;
	}

	bf->ToOrdinaryBloomFilter();

	std::cout << "Tests passed." << std::endl;

	return 0;
}
