#include<libpmemobj.h>

#define LOG_SIZE PMEMOBJ_MIN_POOL
#define PAGE_SIZE 4096

POBJ_LAYOUT_BEGIN(LOG);
POBJ_LAYOUT_TOID(LOG, struct Extent);
POBJ_LAYOUT_TOID(LOG, struct PAGE);
POBJ_LAYOUT_END(LOG);

struct PAGE{
	char data[PAGE_SIZE];
};
struct Extent{
	uint64_t start;
	uint64_t len;
	TOID(struct PAGE) page;
};

char temp_data[PAGE_SIZE];
class Log{
public:
	PMEMobjpool* log_pop;
	Log(char* log_path){
		if(access(log_path, 0)!=0){
			log_pop = pmemobj_create(log_path, "log", LOG_SIZE, 0666);
			if(!log_pop){
				perror("pmemobj_create");
				exit(0);
			}
		}
		for(int i=0;i<PAGE_SIZE;i++){
			temp_data[i] = 'a'+i%26;
		}
		printf("Log Init\n");
	}
	Value_t insert(Key_t key){
		TOID(struct PAGE) p;
		POBJ_ALLOC(log_pop, &p, struct PAGE, sizeof(struct PAGE), NULL, NULL);
		uint64_t temp_addr = (uint64_t)log_pop + p.oid.off;
		memcpy(&D_RW(p)[0].data, temp_data, PAGE_SIZE);
		return (Value_t)temp_addr;
	}
	Value_t insert_extent(uint64_t start, uint64_t len){
		TOID(struct Extent) temp;
//		POBJ_ALLOC(log_pop, &temp, struct Extent, sizeof(struct Extent), NULL, NULL);
		uint64_t temp_addr = (uint64_t)log_pop + temp.oid.off;
/*		D_RW(temp)->start = start;
		D_RW(temp)->len = len;
		
		TOID(struct PAGE) p;
		POBJ_ALLOC(log_pop, &p, struct PAGE, sizeof(struct PAGE)*len, NULL, NULL);
		for(int i=0;i<len;i++){
			temp_data[0] = 'a'+i%26;
			memcpy(&D_RW(p)[i].data, temp_data, PAGE_SIZE);
		}
		D_RW(temp)->page = p;
*/		return (Value_t)temp_addr;
	}
};
