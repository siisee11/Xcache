#ifndef TABLEMANAGER_H_
#define TABLEMANAGER_H_

#include <unistd.h>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <map>
#include <atomic>
#include <libpmemobj.h>
#include "CCEH_hybrid.h"

#define POOL_SIZE (10737418240) // 10GB
#define NUM_NUMA 2

using namespace std;
char* paths[NUM_NUMA];

class TableManager{
	public:
		static const size_t initialTableSize = 1024 * 16;
		/* TODO: NUMA load tracker 
		*/

		TableManager(void);
		TableManager(bool);
		~TableManager(void);
		CCEH* Get(int64_t);
		PMEMobjpool* CreatePool(void);
		void Insert(int64_t, CCEH*);
		void HandleMessage(uint16_t, int64_t, int64_t, void*, int);
		bool AnonymousPage_Replace(int64_t);
		bool DirtyPage_Replace(int64_t);
		/* hashtable storing inodes */
		CCEH* iNodeTable;
		//unordered_map<pair<int64_t, CCEH*> iNodeTable;
	private:
		atomic<int> file_num[NUM_NUMA];
		/* pool pointer for inodes */
		PMEMobjpool* inode_pop;
		/* pool pointer for page logs */
		PMEMobjpool* log_pop[NUM_NUMA];
};


/* base constructor for initial startup */
TableManager::TableManager(void){
	char log_path[32] = "/mnt/pmem0/hk/log";
	char inode_path[32] = "/mnt/pmem0/hk/inode";
	inode_pop = pmemobj_create(inode_path, "INODE", POOL_SIZE, 0666);
	if(!inode_pop){
		perror("pmemobj_create");
		exit(1);
	}
	iNodeTable = new CCEH(inode_pop, initialTableSize/Segment::kNumSlot);

	for(int i=0; i<NUM_NUMA; i++){
		snprintf(&log_path[9], 1, "%d", i);
		file_num[i] = 0;
		log_pop[i] = pmemobj_create(log_path, "PAGE_LOG", LOG_SIZE, 0666);
		if(!log_pop[i]){
			perror("pmemobj_create");
			exit(1);
		}
	}
}

/* recovery constructor */
TableManager::TableManager(bool recovery){
	char log_path[32] = "/mnt/pmem0/hk/log";
	char inode_path[32] = "/mnt/pmem0/hk/inode";

	/* when first startup, create all the initial pools */
	if(!recovery){
		inode_pop = pmemobj_create(inode_path, "INODE", POOL_SIZE, 0666);
		if(!inode_pop){
			perror("pmemobj_create");
			exit(1);
		}
		iNodeTable = new CCEH(inode_pop, initialTableSize/Segment::kNumSlot);

		for(int i=0; i<NUM_NUMA; i++){
			snprintf(&log_path[9], 1, "%d", i);
			file_num[i] = 0;
			log_pop[i] = pmemobj_create(log_path, "PAGE_LOG", LOG_SIZE, 0666);
			if(!log_pop[i]){
				perror("pmemobj_create");
				exit(1);
			}
		}
	}
	else{
		/* TODO: use PM-only CCEH for data(files)?
		   if we use hybrid CCEH for files, recover is too slow
		   - need to update all the key-values for iNodeTable, since the volatile pointer for files-CCEH will be gone.
		   - if we do not use persistent data structure for iNodeTable, we cannot trace which inode key has been inserted/deleted upon recovery. 
		   */
		char data_path[32] = "/mnt/pmem0/hk/data";
		inode_pop = pmemobj_open(inode_path, "INODE");
		if(!inode_pop){
			perror("pmemobj_open");
			exit(1);
		}
		iNodeTable = new CCEH(inode_pop, recovery); 
		if(!iNodeTable->Recovery()){
			cerr << "[FAILED] iNodeTable Recovery Failed" << endl;
			exit(1);
		}

		for(int i=0; i<NUM_NUMA; i++){
			snprintf(&data_path[9], 1, "%d", i);
			snprintf(&log_path[9], 1, "%d", i);
			log_pop[i] = pmemobj_open(log_path, "PAGE_LOG");
			if(!log_pop[i]){
				perror("pmemobj_open");
				exit(1);
			}
			DIR* d = opendir(data_path); 
			if(d){
				int cnt = 0;
				struct dirent* dir;
				while((dir = readdir(d)) != NULL){
					cnt++;
					PMEMobjpool* pop = pmemobj_open(dir->d_name, "CCEH");
					if(!pop){
						perror("pmemobj_open");
						exit(1);
					}
					CCEH* hashtable = new CCEH(pop, recovery);
					if(!hashtable->Recovery()){
						cerr << "[FAILED] Hashtable Recovery Failed" << endl;
						exit(1);
					}
				}
				file_num[i] = cnt;
			}
			closedir(d);
		}
	}
}

TableManager::~TableManager(){
}   

/* return hashtable pointer of inode number from iNodeTable */
CCEH* TableManager::Get(int64_t inode_num){
	return (CCEH*)iNodeTable->Get(inode_num);
}

/* insert hashtable pointer of inode number into iNodeTable */
void TableManager::Insert(int64_t inode_num, CCEH* hashtable_ptr){
	iNodeTable->Insert(inode_num, hashtable_ptr);
}

/* create new pm pool -- called when new inode number comes in */
PMEMobjpool* TableManager::CreatePool(void){
	/* TODO: NUMA node decision */
	int node = rand() % NUM_NUMA;
	int file = file_num[node].fetch_add();
	char data_path[32] = "/mnt/pmem0/hk/data/";
	/* TODO: efficient path management without strcpy */
	snprintf(&data_path[9], 1, "%d", node);
	snprintf(&data_path[19], sizeof(int), "%d", file);
	PMEMobjpool* pop = pmemobj_create(data_path, "CCEH", POOL_SIZE, 0666);
	if(!pop){
		perror("pmemobj_create");
		return NULL;
	}
	return pop;
}

/* base work handler 
TODO: should be joined with server work handler */
void TableManager::HandleMessage(uint16_t msg_type, int64_t inode_num, int64_t key, void* value, int num){
	if(msg_type == PUT_PAGE){
		CCEH* hashtable_ptr = Get(inode_num);
		/* need to decide which node to put page logs */
		int node_id = rand() % NUM_NUMA;
		TOID(char) temp;
		POBJ_ALLOC(log_pop[node_id], &temp, char, sizeof(char)*PAGE_SIZE*num, NULL, NULL);
		uint64_t temp_addr = (uint64_t)log_pop[node_id] + temp.oid.off;
		memcpy((void*)temp_addr, value, sizeof(char)*PAGE_SIZE*num);
		pmemobj_persist(log_pop[node_id], (char*)temp_addr, sizeof(char)*PAGE_SIZE*num);

		/* if already exists */
		if(hashtable_ptr){
			hashtable_ptr->Insert(key, (Value_t)temp_addr);
		}
		/* create new pool and hashtable */
		else{
			TOID(char) temp;
			PMEMobjpool* pop = CreatePool();
			hashtable_ptr = new CCEH(pop);
			hashtable_ptr->Insert(key, (Value_t)temp_addr);
			Insert(inode_num, hashtable_ptr);
		}
	}
	else if(msg_type == GET_PAGE){
		CCEH* hashtable_ptr = Get(inode_num);
		value = (void*)hashtable_ptr->Get(key);
	}
	else{
		cerr << "[" << __func__ << "] invalid msg_type(" << msg_type << ")" << endl;
	}

	return;
}


/*
   a PM capacity manager thread should be running and track available capacity to decide whether call this function or not.
   the inode that will be replaced should be chosen based on LRU or other scheme.
   */
bool TableManager::AnonymousPage_Replace(int64_t inode_num){
	CCEH* hashtable_ptr = Get(inode_num);
	if(!hashtable_ptr){
		/* hashtable for the inode number does not exist */
		return false;
	}

	std::vector<char*> page_logs;
	TOID(struct Segment) segment_pm = OID_NULL;
	int stride = 0;
	/* TODO: line 219-248 should be implemented in CCEH class */
	for(int i=0; i<hashtable_ptr->dir->capacity; i+=stride){
		segment_pm = hashtable_ptr->dir->segment[i];
		if(segment_pm.oid.pool_uuid_lo == 0){
			/* segment is in dram */
			struct Segment* segment_dram = (struct Segment*)segment_pm.oid.off; 
			for(int j=0; j<Segment::kNumSlot; j++){
				if(segment_dram->bucket[j].key != INVALID){
					/* free page logs in current segment */
					char* temp = (char*)segment_dram->bucket[j].value;
					if(!temp){
						page_logs.push_back(temp);
					}
				}
			}
			stride = pow(2, hashtable_ptr->dir->depth - segment_dram->local_depth);
		}
		else{
			/* segment is in pm */
			for(int j=0; j<Segment::kNumSlot; j++){
				if(D_RO(segment_pm)->bucket[j].key != INVALID){
					/* free page logs in current segment */
					char* temp = (char*)D_RO(segment_pm)->bucket[j].value;
					if(!temp){
						page_logs.push_back(temp);
					}
				}
			}
			stride = pow(2, hashtable_ptr->dir->depth - D_RO(segment_pm)->local_depth);
		}
	}

	for(auto& it: page_logs){
		POBJ_FREE(&it);
	}

	return true;
}

/*
   a PM capacity manager thread should be running and track available capacity to decide whether call this function or not.
   the inode that will be replaced should be chosen based on LRU or other scheme.
   */
bool TableManager::DirtyPage_Replace(int64_t inode_num){
	CCEH* hashtable_ptr = Get(inode_num);
	if(!hashtable_ptr){
		/* hashtable for the inode number does not exist */
		return false;
	}

	std::vector<std::pair<int64_t, char*>> page_logs;
	TOID(struct Segment) segment_pm = OID_NULL;
	int stride = 0;
	/* TODO: line 271-301 should be implemented in CCEH class */
	for(int i=0; i<hashtable_ptr->dir->capacity; i+=stride){
		segment_pm = hashtable_ptr->dir->segment[i];
		if(segment_pm.oid.pool_uuid_lo == 0){
			/* segment is in dram */
			struct Segment* segment_dram = (struct Segment*)segment_pm.oid.off; 
			for(int j=0; j<Segment::kNumSlot; j++){
				if(segment_dram->bucket[j].key != INVALID){
					/* free page logs in current segment */
					char* temp = (char*)segment_dram->bucket[j].value;
					if(!temp){
						page_logs.push_back(make_pair(segment_dram->bucket[j].key, temp));
					}
				}
			}
			stride = pow(2, hashtable_ptr->dir->depth - segment_dram->local_depth);
		}
		else{
			/* segment is in pm */
			for(int j=0; j<Segment::kNumSlot; j++){
				if(D_RO(segment_pm)->bucket[j].key != INVALID){
					/* free page logs in current segment */
					char* temp = (char*)D_RO(segment_pm)->bucket[j].value;
					if(!temp){
						page_logs.push_back(make_pair(D_RO(segment_pm)->bucket[j].key, temp));
					}
				}
			}
			stride = pow(2, hashtable_ptr->dir->depth - D_RO(segment_pm)->local_depth);
		}
	}

	/* TODO:
	   implement draining the collected page logs in vector
	   to client first and then free
	   - maybe generate a work request in workqueue with the vector address referencing?
	   */

	for(auto& it: page_logs){
		POBJ_FREE(&it.second);
	}

	return true;
}

#endif
