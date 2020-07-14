#include "CCEH.h"
#include "persist.h"
#include <iostream>
#include <ctime>
#include <cstdlib>
#include <unistd.h>

using namespace std;

int main(int argc, char* argv[]){
    int numData;
    if(argc < 2){
	cerr << "Usage ./%s numData" << argv[0] << endl;
	return 0;
    }

    numData = atoi(argv[1]);
    srand(time(NULL));
    void** pages = (void**)malloc(sizeof(void*)*numData);
    uint64_t* keys = new uint64_t[numData];
    for(int i=0; i<numData; i++){
	pages[i] = (void*)malloc(sizeof(char)*4096);
	memset(pages[i], i+1, 4096);
	keys[i] = rand()+1;
	//keys[i] = rand() % (numData);
	memcpy(pages[i], &keys[i], sizeof(uint64_t));
	//memcpy(&keys[i], pages[i], sizeof(uint64_t));
    }

    CCEH* hashtable = new CCEH(1024*16);
    cout << "numData: " << numData << endl;
    
    int insertCount = 0;

    for(int i=0; i<numData; i++){
	hashtable->Insert(keys[i], (char*)&keys[i]);
    }

    int failedSearch = 0;
    for(int i=0; i<numData; i++){
	const char* ret = hashtable->Get(keys[i]);
	if(ret == NONE){
	    failedSearch++;
	}
    }


    cout << "failedSearch: " << failedSearch << endl;

    delete[] keys;

    return 0;
}

