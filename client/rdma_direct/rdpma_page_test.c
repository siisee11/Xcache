#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/memory.h>
#include <linux/mm_types.h>
#include <linux/random.h>
#include <linux/kthread.h>

#include "pmdfc.h"
#include "rdpma.h"
#include "timeperf.h"

#define THREAD_NUM 8
#define PAGE_ORDER 0
#define BATCH_SIZE (1 << PAGE_ORDER)
#define TOTAL_CAPACITY (PAGE_SIZE * BATCH_SIZE * THREAD_NUM * 1024 * 128)
#define ITERATIONS (TOTAL_CAPACITY/PAGE_SIZE/BATCH_SIZE/THREAD_NUM)

struct page*** vpages;
atomic_t failedSearch;
struct completion* comp;
struct page** return_page;
uint64_t** keys;

struct thread_data{
	int tid;
	struct completion *comp;
	int ret;
};

struct task_struct** write_threads;
struct task_struct** read_threads;

atomic_long_t mr_free_start;
extern long mr_free_end;

/* code from pmdfc.c */
static long get_longkey(long key, long index)
{
	long longkey;
	longkey = key << 32;
	longkey |= index;
	return longkey;
}

int rdpma_single_write_message_test(void* arg){
	struct thread_data* my_data = (struct thread_data*)arg;
	int ret;
	uint32_t key=4400, index=11111;
	char *test_string;
	struct page *test_page;
	int status;
	long longkey;

	test_page = alloc_pages(GFP_KERNEL, PAGE_ORDER);
	test_string = kzalloc(PAGE_SIZE, GFP_KERNEL);
	strcpy(test_string, "hi, dicl");

	memcpy(page_address(test_page), test_string, PAGE_SIZE * (1 << PAGE_ORDER));

	longkey = get_longkey(key, index);
	
	ret = rdpma_put(test_page, longkey, (1 << PAGE_ORDER));
	complete(my_data->comp);

	printk("[ PASS ] %s succeeds \n", __func__);

	return ret;
}


int rdpma_write_message_test(void* arg){
	struct thread_data* my_data = (struct thread_data*)arg;
	int tid = my_data->tid;
	int i, ret, status;
	uint32_t index = 0;
	uint32_t key;
	long longkey;
	int failed = 0;

	for(i = 0; i < ITERATIONS; i++){
		key = (uint32_t)keys[tid][i];
		longkey = get_longkey(key, index);
		ret = rdpma_put(vpages[tid][i], longkey, BATCH_SIZE);
		if (ret != 0)
			failed++;
	}

	complete(my_data->comp);

	if (failed == 0) {
		ret = 0;
	} else {
		ret = failed;
	}

	my_data->ret = ret;

	return ret;
}

int rdpma_single_read_message_test(void* arg){
	struct thread_data* my_data = (struct thread_data*)arg;
	int ret;
	uint32_t key=4400, index = 11111;
	char *test_string;
	int result = 0;
	struct page *test_page;
	long longkey;
	int status;

	test_page = alloc_pages(GFP_KERNEL, PAGE_ORDER);
	test_string = kzalloc(PAGE_SIZE * (1 << PAGE_ORDER), GFP_KERNEL);
	strcpy(test_string, "hi, dicl");

	longkey = get_longkey(key, index);
	
	ret = rdpma_get(test_page, longkey, (1 << PAGE_ORDER));

	if (ret == -1){
		printk("[ FAIL ] Searching for key (ret -1)\n");
		result++;
	}
	else if(memcmp(page_address(test_page), test_string, PAGE_SIZE * (1 << PAGE_ORDER)) != 0){
//		printk("[ FAIL ] Searching for key\n");
//		printk("[ FAIL ] returned: %s\n", (char *)page_address(test_page));
		result++;
	}

	if (result == 0) {
		ret = 0;
	} else {
		ret = result;
	}

	complete(my_data->comp);
	my_data->ret = ret;

	return ret;
}

int rdpma_read_message_test(void* arg){
	struct thread_data* my_data = (struct thread_data*)arg;
	int ret, i;
	uint32_t key, index = 0;
	int tid = my_data->tid;
	int nfailed = 0;
	long longkey;
	int status;

	for(i = 0; i < ITERATIONS; i++){
		key = keys[tid][i];

		longkey = get_longkey(key, index);
		ret = rdpma_get(return_page[tid], longkey, BATCH_SIZE);

		if(memcmp(page_address(return_page[tid]), page_address(vpages[tid][i]), PAGE_SIZE * BATCH_SIZE) != 0){
//			printk("failed Searching for key %x\nreturn: %s\nexpect: %s", key, (char *)page_address(return_page[tid]), (char *)page_address(vpages[tid][i]));
			nfailed++;
		}
	}

	if (nfailed == 0) {
//		printk("[ PASS ] %s succeeds\n", __func__);
		ret = 0;
	} else {
//		printk("[ FAIL ] failedSearch: %d\n", nfailed);
		ret = nfailed;
	}

	complete(my_data->comp);
	my_data->ret = ret;

	return ret;
}


int main(void){
	int i;
	ktime_t start, end;
	uint64_t elapsed;
	int ret = 0;
	struct thread_data** args = (struct thread_data**)kmalloc(sizeof(struct thread_data*)*THREAD_NUM, GFP_KERNEL);

	for(i = 0; i < THREAD_NUM; i++){
		args[i] = (struct thread_data*)kmalloc(sizeof(struct thread_data), GFP_KERNEL);
		args[i]->tid = i;
		args[i]->comp = &comp[i];
		args[i]->ret = 0;
	}
	pr_info("Start running write thread functions...\n");
	start = ktime_get();
	for(i=0; i<THREAD_NUM; i++){
//		write_threads[i] = kthread_create((void*)&rdpma_single_write_message_test, (void*)args[i], "page_writer");
		write_threads[i] = kthread_create((void*)&rdpma_write_message_test, (void*)args[i], "page_writer");
		wake_up_process(write_threads[i]);
	}

	for(i=0; i<THREAD_NUM; i++){
		wait_for_completion(&comp[i]);
	}
	end = ktime_get();
	elapsed = ((u64)ktime_to_ns(ktime_sub(end, start)) / 1000);

	for(i=0; i<THREAD_NUM; i++){
		ret += args[i]->ret;
	}

	if (ret == 0 )
		pr_info("[ PASS ] complete write thread functions: time( %llu ) usec, %d failed", elapsed, ret);
	else
		pr_info("[ FAIL ] complete write thread functions: time( %llu ) usec, %d failed ", elapsed, ret);

	if (elapsed/1000/1000 != 0)
		pr_info("[ PASS ] Throughput: %lld (MB/sec)\n", (TOTAL_CAPACITY/1024/1024)/(elapsed/1000/1000));
	else 
		pr_info("[ PASS ] Throughput: %lld (MB/usec)\n", (TOTAL_CAPACITY/1024/1024)/(elapsed/1000));
	
	ssleep(1);

	ret = 0;
	for(i=0; i<THREAD_NUM; i++){
		reinit_completion(&comp[i]);
		args[i]->comp = &comp[i];
		args[i]->ret = 0;
	}
	pr_info("Start running read thread functions...\n");
	start = ktime_get();

	for(i=0; i<THREAD_NUM; i++){
//		read_threads[i] = kthread_create((void*)&rdpma_single_read_message_test, (void*)args[i], "page_reader");
		read_threads[i] = kthread_create((void*)&rdpma_read_message_test, (void*)args[i], "page_reader");
		wake_up_process(read_threads[i]);
	}

	for(i=0; i<THREAD_NUM; i++){
		wait_for_completion(&comp[i]);
	}

	end = ktime_get();
	elapsed = ((u64)ktime_to_ns(ktime_sub(end, start)) / 1000);

	ret = 0;
	for(i=0; i<THREAD_NUM; i++){
		ret += args[i]->ret;
	}

	if (ret == 0 )
		pr_info("[ PASS ] complete read thread functions: time( %llu ) usec, %d failed\n", elapsed, ret);
	else
		pr_info("[ FAIL ] complete read thread functions: time( %llu ) usec, %d failed \n", elapsed, ret);

	if (elapsed/1000/1000 != 0)
		pr_info("[ PASS ] Throughput: %lld (MB/sec)\n", (TOTAL_CAPACITY/1024/1024)/(elapsed/1000/1000));
	else 
		pr_info("[ PASS ] Throughput: %lld (MB/usec)\n", (TOTAL_CAPACITY/1024/1024)/(elapsed/1000));

	for(i=0; i<THREAD_NUM; i++){
		reinit_completion(&comp[i]);
		args[i]->comp = &comp[i];
		args[i]->ret = 0;
	}

	pr_info("Start running mixed thread functions...\n");
	start = ktime_get();

	for(i=0; i<THREAD_NUM/2; i++){
		write_threads[i] = kthread_create((void*)&rdpma_write_message_test, (void*)args[i], "page_writer");
		wake_up_process(write_threads[i]);
	}
	for(i=THREAD_NUM/2; i<THREAD_NUM; i++){
		read_threads[i] = kthread_create((void*)&rdpma_read_message_test, (void*)args[i], "page_reader");
		wake_up_process(read_threads[i]);
	}

	for(i=0; i<THREAD_NUM; i++){
		wait_for_completion(&comp[i]);
	}

	end = ktime_get();
	elapsed = ((u64)ktime_to_ns(ktime_sub(end, start)) / 1000);

	ret = 0;
	for(i=0; i<THREAD_NUM; i++){
		ret += args[i]->ret;
	}

	if (ret == 0 )
		pr_info("[ PASS ] complete mixed thread functions: time( %llu ) usec, %d failed\n", elapsed, ret);
	else
		pr_info("[ FAIL ] complete mixed thread functions: time( %llu ) usec, %d failed\n", elapsed, ret);

	if ( elapsed / 1000/ 1000 != 0)
		pr_info("[ PASS ] Throughput: %lld (MB/sec)\n", (TOTAL_CAPACITY/1024/1024)/(elapsed/1000/1000));
	else 
		pr_info("[ PASS ] Throughput: %lld (MB/usec)\n", (TOTAL_CAPACITY/1024/1024)/(elapsed/1000));

//	pmdfc_rdma_print_stat();

	ssleep(1);

	for(i=0; i<THREAD_NUM; i++){
		kfree(args[i]);
	}
	kfree(args);

	return 0;
}

int init_pages(void){
	int i, j;
	uint64_t key = 0;
	char str[8];

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

	return_page = (struct page**)kmalloc(sizeof(void*)*THREAD_NUM, GFP_KERNEL);
	if(!return_page){
		printk(KERN_ALERT "return page allocation failed\n");
		goto ALLOC_ERR;
	}

	for(i=0; i<THREAD_NUM; i++){
		return_page[i] = alloc_pages(GFP_KERNEL, PAGE_ORDER);
		if(!return_page[i]){
			printk(KERN_ALERT "return page[%d] allocation failed\n", i);
			goto ALLOC_ERR;
		}
	}

	vpages = (struct page***)kmalloc(sizeof(struct page**)*THREAD_NUM, GFP_KERNEL);
	if(!vpages){
		printk(KERN_ALERT "vpages allocation failed\n");
		goto ALLOC_ERR;
	}
	for(i=0; i<THREAD_NUM; i++){
		vpages[i] = (struct page**)vmalloc(sizeof(void*)*ITERATIONS);
		if(!vpages[i]){
			printk(KERN_ALERT "vpages[%d] allocation failed\n", i);
			goto ALLOC_ERR;
		}
		for(j=0; j<ITERATIONS; j++){
			vpages[i][j] = alloc_pages(GFP_KERNEL, PAGE_ORDER);
			if(!vpages[i][j]){
				printk(KERN_ALERT "vpages[%d][%d] allocation failed\n", i, j);
				goto ALLOC_ERR;
			}
			sprintf(str, "%lld", key++ );
			strcpy(page_address(vpages[i][j]), str);
		}
	}

	keys = (uint64_t**)kmalloc(sizeof(uint64_t*)*THREAD_NUM, GFP_KERNEL);
	key = 0;
	if(!keys){
		printk(KERN_ALERT "keys allocation failed\n");
		goto ALLOC_ERR;
	}
	for(i=0; i<THREAD_NUM; i++){
		keys[i] = (uint64_t*)vmalloc(sizeof(uint64_t)*ITERATIONS);
		for(j=0; j<ITERATIONS; j++){
			keys[i][j] = key++;
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

	printk(KERN_INFO "[ PASS ] pmdfc initialization");
	return 0;


ALLOC_ERR:
	if(write_threads) 
		kfree(write_threads);
	if(read_threads) 
		kfree(read_threads);
	if(comp) 
		kfree(comp);

	for(i=0; i<THREAD_NUM; i++){
		if(return_page[i]) kfree(return_page[i]);
		for(j=0; j<ITERATIONS; j++){
			if(vpages[i][j]) kfree(vpages[i][j]);
		}
		if(vpages[i]) vfree(vpages[i]);
	}

	if(return_page) 
		kfree(return_page);
	if(vpages) 
		kfree(vpages);
	return 1;
}

void show_test_info(void){

	pr_info("+------------ PMDFC SERVER TEST INFO ------------+\n");
	pr_info("| NUMBER OF THREAD: %20d  \t|\n", THREAD_NUM);
	pr_info("| TOTAL CAPACITY  : %20ld \t|\n", TOTAL_CAPACITY);
	pr_info("| ITERATIONS      : %20ld \t|\n", ITERATIONS);
	pr_info("+------------------------------------------------+\n");

	return;
}


static int __init init_test_module(void){
	int ret = 0;

	show_test_info();
	ssleep(1);

	ret = init_pages();
	if(ret){
		printk(KERN_ALERT "module initialization failed\n");
		return -1;
	}
	ssleep(1);

	ret = main();
	if(ret){
		printk(KERN_ALERT "module main function failed\n");
		return -1;
	}
	printk(KERN_INFO "run all the module functions\n");

	return 0;
}


static void __exit exit_test_module(void){
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
#if 0
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
#endif

	pr_info("[%s]: exiting test module", __func__);
}

module_init(init_test_module);
module_exit(exit_test_module);

MODULE_AUTHOR("Jaeyoun");
MODULE_LICENSE("GPL");
