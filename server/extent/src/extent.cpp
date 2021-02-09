#include<stdio.h>
#include<ctime>
#include<stdlib.h>
#include<unistd.h>
#include<strings.h>
#include<string.h>
#include<math.h>

#include "src/CCEH.h"
#include "src/log.h"
#define MAX_HEIGHT 20
#define POOL_SIZE (10737418240)

int NO=0;
struct timespec start_t, end_t;
long negative_search=0;
long failed_search=0;
long elapsed=0;
int main(int argc, char* argv[]){
	if(argc!=3){
		printf("./exe CCEH_path LOG_path");
		exit(1);
	}
	Log *log = new Log(argv[2]);	
	const size_t initialSize = 1024;
	PMEMobjpool* pop;
	TOID(CCEH) HashTable = OID_NULL;
	if(access(argv[1], 0) != 0){
		pop = pmemobj_create(argv[1], "CCEH", PMEMOBJ_MIN_POOL*10, 0666);
		if(!pop){
			perror("pmemobj_create");
			exit(1);
		}
		HashTable = POBJ_ROOT(pop, CCEH);
		D_RW(HashTable)->initCCEH(pop, initialSize);
	}else{
		pop = pmemobj_open(argv[1], "CCEH");
		if(pop == NULL){
			perror("pmemobj_open");
			exit(1);
		}
		HashTable = POBJ_ROOT(pop, CCEH);
		if(D_RO(HashTable)->crashed){
			D_RW(HashTable)->Recovery(pop);
		}
	}
	
	int inst,b,start,target;
	printf("pmem create finished\n");
	printf("inst start len\n");fflush(stdout);

	clock_gettime(CLOCK_MONOTONIC, &start_t);
	while(1){
		scanf("%ld ",&inst);
		if(inst==-1) break;	
		if(inst==1){
			NO++;
			scanf("%d %d",&start,&b);
			Key_t s = (Key_t)start;
//			printf("Insert query : start(%d, %d) len(%d)>",s,start,b);
			D_RW(HashTable)->Insert_extent(pop, s, b, log->insert_extent(start, b));
		}
		if(inst==2){
			scanf("%ld",&target);
			Key_t t = (Key_t)target;
			Value_t result = D_RW(HashTable)->Get_extent(t);
			if(!result){
				negative_search++;
			}else{
				struct Extent tmp;
				memcpy(&tmp, result, sizeof(struct Extent));
				if(target>=tmp.start && target<tmp.start+tmp.len){
//					printf("[target:%d %d %d]",target,tmp.start, tmp.len);
//					printf("%d %c\n",target,D_RO(tmp.page)[target-tmp.start].data[0]);
				}else{
					negative_search++;
				}
			}
		}
		
		if(inst==11){
			NO++;
			scanf("%d",&b);
			Key_t s = (Key_t)b;
//			printf("Insert query : %d",b);
			D_RW(HashTable)->Insert(pop, s, log->insert(s));
		}
		if(inst==12){
			scanf("%d",&target);
			Key_t t = (Key_t)target;
			Value_t result = D_RW(HashTable)->Get(t);
			if(result==NONE){
				negative_search++;
			}else{
				struct PAGE p;
				memcpy(&p, result, sizeof(struct PAGE));
//				printf("%d %c\n",target,p.data[0]);
			}
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &end_t);
	elapsed = (end_t.tv_sec - start_t.tv_sec)*1000000000 + (end_t.tv_nsec - start_t.tv_nsec);
	printf("Negative_search: %ld\n",negative_search);
	printf("elapsed: (%ld %ld) %ld usec\n",end_t.tv_sec, start_t.tv_sec, elapsed/1000);
}
