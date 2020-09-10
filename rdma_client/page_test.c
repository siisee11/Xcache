#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/memory.h>
#include <linux/mm_types.h>
#include <linux/random.h>
#include <linux/kthread.h>
#include "net.h"

#define THREAD_NUM 1
#define TOTAL_CAPACITY (PAGE_SIZE * 65)
//#define TOTAL_CAPACITY (1024*1024*1024)
//#define TOTAL_CAPACITY (PAGE_SIZE * 4)
//#define TOTAL_CAPACITY (1024*1024)
//#define TOTAL_CAPACITY (1024*1024*1024) // 1G
#define ITERATIONS (TOTAL_CAPACITY/PAGE_SIZE/THREAD_NUM)
//#define ITERATIONS 512

void*** vpages;
atomic_t failedSearch;
struct completion* comp;
void** return_page;
uint64_t** keys;

struct thread_data{
	int tid;
	struct completion *comp;
};

struct task_struct** write_threads;
struct task_struct** read_threads;

int write_test(void* arg){
	struct thread_data* my_data = (struct thread_data*)arg;
	int tid = my_data->tid;
	int i, ret;
	uint64_t key[1];

	for(i=0; i<ITERATIONS; i++){
		key[0] = keys[tid][i];
		ret = generate_write_request((void**)&vpages[tid][i], key, 1);
	}

	complete(my_data->comp);
	return 0;
}

int read_test(void* arg){
	struct thread_data* my_data = (struct thread_data*)arg;
	int i, ret;
	int tid = my_data->tid;
	//char temp[PAGE_SIZE];
	//void* temp = (void*)kmalloc(PAGE_SIZE, GFP_KERNEL);
	//void* temp[PAGE_SIZE];
	//char temp[PAGE_SIZE];
	int result = 0;
	uint64_t key[1];

	for(i=0; i<ITERATIONS; i++){
		//memcpy(&key[tid][i], vpages[tid][i], sizeof(uint64_t));
		key[0] = keys[tid][i];
		ret = generate_read_request((void**)&return_page[tid], key, 1);

		if(memcmp(return_page[tid], vpages[tid][i], PAGE_SIZE) != 0){
			printk("failed Searching for key %llu\n", key[0]);
			result++;
			//atomic_inc(&failedSearch);
		}
	}
	printk("failedSearch: %d\n", result);
	complete(my_data->comp);
	return 0;
}
/*
   int write_page(void* arg){
   struct thread_data* my_data = (struct thread_data*)arg;
   int from, to;
   int i, ret;

   from = (ITERATIONS / THREAD_NUM) * (my_data->tid);
   to = (ITERATIONS / THREAD_NUM) * (my_data->tid+1);
   for(i=from; i<to; i++){
   ret = generate_write_request((void**)&pages[i], 1);
   }

   complete(my_data->comp);
   return 0;
   }

   int read_page(void* arg){
   struct thread_data* my_data = (struct thread_data*)arg;
   int from, to;
   int i, ret;
   void* page = (void*)kmalloc(PAGE_SIZE, GFP_KERNEL);
   uint64_t key[1];

   from = (ITERATIONS / THREAD_NUM) * (my_data->tid);
   to = (ITERATIONS / THREAD_NUM) * (my_data->tid+1);

   for(i=from; i<to; i++){
   key[0] = pages[i]->index;
   ret = generate_read_request(&page, key, 1);
   if(((struct page*)page)->index != key[0])
   atomic_inc(&failedSearch);
   }
   complete(my_data->comp);
   return 0;
   }*/


int main(void){
	int i;
	ktime_t start, end;
	uint64_t elapsed;
	struct thread_data** args = (struct thread_data**)kmalloc(sizeof(struct thread_data*)*THREAD_NUM, GFP_KERNEL);

	for(i=0; i<THREAD_NUM; i++){
		args[i] = (struct thread_data*)kmalloc(sizeof(struct thread_data), GFP_KERNEL);
		args[i]->tid = i;
		args[i]->comp = &comp[i];
	}

	pr_info("************************************************");
	pr_info("   running write thread functions               ");
	pr_info("************************************************");
	start = ktime_get();
	for(i=0; i<THREAD_NUM; i++){
		write_threads[i] = kthread_create((void*)&write_test, (void*)args[i], "page_writer");
		//write_threads[i] = kthread_create((void*)&write_page, (void*)args[i], "page_writer");
		wake_up_process(write_threads[i]);
	}

	for(i=0; i<THREAD_NUM; i++){
		wait_for_completion(&comp[i]);
	}
	end = ktime_get();
	elapsed = ((u64)ktime_to_ns(ktime_sub(end, start)) / 1000);
	pr_info("************************************************");
	pr_info("   complete write thread functions: time( %llu ) usec", elapsed);
	pr_info("************************************************\n\n");
	ssleep(5);

	for(i=0; i<THREAD_NUM; i++){
		reinit_completion(&comp[i]);
		args[i]->comp = &comp[i];
	}

	pr_info("************************************************");
	pr_info("   running read thread functions               ");
	pr_info("************************************************");
	start = ktime_get();

	for(i=0; i<THREAD_NUM; i++){
		read_threads[i] = kthread_create((void*)&read_test, (void*)args[i], "page_reader");
		//read_threads[i] = kthread_create((void*)&read_page, (void*)args[i], "page_reader");
		wake_up_process(read_threads[i]);
	}

	for(i=0; i<THREAD_NUM; i++){
		wait_for_completion(&comp[i]);
	}

	end = ktime_get();
	elapsed = ((u64)ktime_to_ns(ktime_sub(end, start)) / 1000);
	pr_info("************************************************");
	pr_info("   complete read thread functions: time( %llu ) usec", elapsed);
	pr_info("************************************************");

	for(i=0; i<THREAD_NUM; i++){
		kfree(args[i]);
	}
	kfree(args);

	return 0;
}

int init_pages(void){
	int i, j;
	uint64_t key = 0;
	u64 rand;

	pr_info("pmrdma test: initialize pages\n");

	write_threads = (struct task_struct**)kmalloc(sizeof(struct task_struct*)*THREAD_NUM, GFP_KERNEL);
	if(!write_threads){
		printk(KERN_ALERT "write_threads allocation failed\n");
		goto ALLOC_ERR;
	}

	read_threads = (struct task_struct**)kmalloc(sizeof(struct task_struct*)*THREAD_NUM, GFP_KERNEL);
	if(!read_threads){
		printk(KERN_ALERT "read_threads allocation failed\n");
		goto ALLOC_ERR;
	}

	return_page = (void**)kmalloc(sizeof(void*)*THREAD_NUM, GFP_KERNEL);
	if(!return_page){
		printk(KERN_ALERT "return page allocation failed\n");
		goto ALLOC_ERR;
	}

	for(i=0; i<THREAD_NUM; i++){
		return_page[i] = (void*)kmalloc(PAGE_SIZE, GFP_KERNEL);
		if(!return_page[i]){
			printk(KERN_ALERT "return page[%d] allocation failed\n", i);
			goto ALLOC_ERR;
		}
	}

	vpages = (void***)kmalloc(sizeof(void**)*THREAD_NUM, GFP_KERNEL);
	if(!vpages){
		printk(KERN_ALERT "vpages allocation failed\n");
		goto ALLOC_ERR;
	}
	for(i=0; i<THREAD_NUM; i++){
		vpages[i] = (void**)vmalloc(sizeof(void*)*ITERATIONS);
		if(!vpages[i]){
			printk(KERN_ALERT "vpages[%d] allocation failed\n", i);
			goto ALLOC_ERR;
		}
		for(j=0; j<ITERATIONS; j++){
			vpages[i][j] = (void*)kmalloc(sizeof(char)*PAGE_SIZE, GFP_KERNEL);
			if(!vpages[i][j]){
				printk(KERN_ALERT "vpages[%d][%d] allocation failed\n", i, j);
				goto ALLOC_ERR;
			}
			get_random_bytes(&rand, sizeof(u64));
			memcpy(vpages[i][j], &rand, sizeof(u64));
		}
	}

	keys = (uint64_t**)kmalloc(sizeof(uint64_t*)*THREAD_NUM, GFP_KERNEL);
	if(!keys){
		printk(KERN_ALERT "keys allocation failed\n");
		goto ALLOC_ERR;
	}
	for(i=0; i<THREAD_NUM; i++){
		keys[i] = (uint64_t*)vmalloc(sizeof(uint64_t)*ITERATIONS);
		for(j=0; j<ITERATIONS; j++){
			keys[i][j] = ++key;
		}
	}

	comp = (struct completion*)kmalloc(sizeof(struct completion)*THREAD_NUM, GFP_KERNEL);
	if(!comp){
		printk(KERN_ALERT "Completion allocation failed\n");
		goto ALLOC_ERR;
	}

	for(i=0; i<THREAD_NUM; i++){
		init_completion(&comp[i]);
	}

	atomic_set(&failedSearch, 0);
	return 0;


ALLOC_ERR:
	if(write_threads) kfree(write_threads);
	if(read_threads) kfree(read_threads);
	if(comp) kfree(comp);
	for(i=0; i<THREAD_NUM; i++){
		if(return_page[i]) kfree(return_page[i]);
		for(j=0; j<ITERATIONS; j++){
			if(vpages[i][j]) kfree(vpages[i][j]);
		}
		if(vpages[i]) vfree(vpages[i]);
	}
	if(return_page) kfree(return_page);
	if(vpages) kfree(vpages);
	return 1;
}


static int __init init_test_module(void){
	int ret = 0;;

	ret = init_pages();
	if(ret){
		printk(KERN_ALERT "module initialization failed\n");
		return -1;
	}

	ret = main();
	if(ret){
		printk(KERN_ALERT "module main function failed\n");
		return -1;
	}
	printk(KERN_INFO "run all the module functions\n");

	return 0;
}


static void __exit exit_test_module(void){
	int i, j;
	pr_info("[%s]: cleaning up resources", __func__);

	if(read_threads){
		kfree(read_threads);
		pr_info("[%s]: freed read threads", __func__);
	}
	if(write_threads){
		kfree(write_threads);
		pr_info("[%s]: freed write threads", __func__);
	}

	if(comp){
		kfree(comp);
		pr_info("[%s]: freed completion", __func__);
	}
	if(vpages){
		for(i=0; i<THREAD_NUM; i++){
			for(j=0; j<ITERATIONS; j++)
				if(vpages[i][j]) kfree(vpages[i][j]);
			if(vpages[i]) vfree(vpages[i]);
		}
		kfree(vpages);
		pr_info("[%s]: freed vpages", __func__);
	}
	if(return_page){
		for(i=0; i<THREAD_NUM; i++)
			if(return_page[i]) kfree(return_page[i]);
		kfree(return_page);
		pr_info("[%s]: freed return pages", __func__);
	}

	pr_info("[%s]: exiting test module", __func__);
}

module_init(init_test_module);
module_exit(exit_test_module);

MODULE_AUTHOR("Hokeun");
MODULE_LICENSE("GPL");
