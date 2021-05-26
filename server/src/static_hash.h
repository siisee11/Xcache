#ifndef CCEH_H_
#define CCEH_H_

#include <cstring>
#include <cmath>
#include <vector>
#include <pthread.h>
#include <iostream>

#include "util/pair.h"
#include "IHash.h"
#include "variables.h"

class HashTable : public IHash {
	public:
		HashTable(void);
		HashTable(size_t);
		~HashTable(void);

		void Insert(Key_t&, Value_t);
		void Insert_extent(Key_t, uint64_t, uint64_t, Value_t);
		bool Delete(Key_t&);
		Value_t Get(Key_t&);
		Value_t Get_extent(Key_t&, uint64_t);
		Value_t FindAnyway(Key_t&);

		double Utilization(void);
		size_t Capacity(void);
		bool Recovery(void);

		void* operator new(size_t size) {
			void *ret;
			if (posix_memalign(&ret, 64, size) ) ret=NULL;
			return ret;
		}

		bool suspend(void){
			int64_t val;
			do{
				val = sema;
				if(val < 0)
					return false;
			}while(!CAS(&sema, &val, -1));

			int64_t wait = 0 - val - 1;
			while(val && sema != wait){
				asm("nop");
			}
			return true;
		}

		bool lock(void){
			int64_t val = sema;
			while(val > -1){
				if(CAS(&sema, &val, val+1))
					return true;
				val = sema;
			}
			return false;
		}

		void unlock(void){
			int64_t val = sema;
			while(!CAS(&sema, &val, val-1)){
				val = sema;
			}
		}

		void reset(void){
			int64_t val = sema;
			while(!CAS(&sema, &val, 0)){
				val = sema;
			}
		}

	private:
		size_t hashSize;
		Pair *bucket;
		int64_t sema = 0;
		unsigned gtime;
};

#endif  // EXTENDIBLE_PTR_H_
