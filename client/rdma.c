#include "rdma.h"
#include <rdma/ib_cache.h>

static const int RDMA_BUFFER_SIZE = 4096;
static const int RDMA_PAGE_SIZE = 4096;
struct task_struct* thread_poll_cq;
struct task_struct* thread_handler;
struct client_context* ctx = NULL;
int ib_port = 1;
enum ib_mtu mtu;

#define HASH_TABLE_SIZE_BIT 10
DEFINE_HASHTABLE(MR_HASHTABLE, HASH_TABLE_SIZE_BIT);
spinlock_t MR_HASHTABLE_LOCK[1 << HASH_TABLE_SIZE_BIT];

enum ib_mtu client_mtu_to_enum(int max_transfer_unit){
	switch(max_transfer_unit){
		case 256:	return IB_MTU_256;
		case 512:	return IB_MTU_512;
		case 1024:	return IB_MTU_1024;
		case 2048:	return IB_MTU_2048;
		case 4096:	return IB_MTU_4096;
		default:	return -1;
	}
}

struct ib_device* ib_dev;
struct ib_pd* ctx_pd;
static struct ib_client client = {
	.name = "PMDFC_Client",
	.add = add_one
};

static struct class client_class = {
	.name = "PMDFC_Client_Class"
};


uintptr_t ib_reg_mr_addr(void* addr, uint64_t size){
	//dprintk("[%s]: registering virtual address to dma address\n", __func__);
	return (uintptr_t)ib_dma_map_single((struct ib_device*)ctx->context, addr, size, DMA_BIDIRECTIONAL);
}

void ib_dereg_mr_addr(uint64_t addr, uint64_t size){
	//dprintk("[%s]: deregistering virtual addres from dma address\n", __func__);
	ib_dma_unmap_single((struct ib_device*)ctx->context, addr, size, DMA_BIDIRECTIONAL);
}

struct mr_info* ib_reg_mr(void* addr, uint64_t length, enum ib_access_flags flags){
	struct mr_info* ret;
	struct ib_mr* mr;
	mr = ctx->pd->device->ops.get_dma_mr(ctx->pd, flags);
	ret = (struct mr_info*)kmalloc(sizeof(struct mr_info), GFP_KERNEL);
	ret->addr = (uint64_t)ib_dma_map_single((struct ib_device*)ctx->context, addr, length, DMA_BIDIRECTIONAL);
	ret->length = length;
	ret->lkey = mr->lkey;
	ret->rkey = mr->rkey;
	ret->node_id = ctx->node_id;
	return ret;
}

int poll_cq_test(struct ib_cq* cq, void* cq_txt){
	int cnt = 0;
	ktime_t start, end;
	struct ib_wc wc;
	start = ktime_get();
	do{
		cnt += ib_poll_cq(cq, 1, &wc);
		end = ktime_get();
		if(((long long)ktime_to_ns(ktime_sub(end, start))) > 1000000000){
			printk(KERN_ALERT "[%s]: timeout, clock expired, poll_cq failed\n", __func__);
			return 0;
		}
	}while(cnt < 1);

	if(cnt < 0){
		printk(KERN_ALERT "[%s]: poll_cq failed\n", __func__);
		return 0;
	}

	if(wc.status != IB_WC_SUCCESS){
		printk(KERN_ALERT "[%s]: failed status %s(%d)\n", __func__, ib_wc_status_msg(wc.status), wc.status);

		switch(wc.status){
			case IB_WC_LOC_LEN_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_LOC_LEN_ERR\n", __func__);
				return 0;
			case IB_WC_LOC_QP_OP_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_LOC_QP_OP_ERR\n", __func__);
				return 0;
			case IB_WC_LOC_EEC_OP_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_LOC_EEC_OP_ERR\n", __func__);
				return 0;
			case IB_WC_LOC_PROT_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_LOC_PROT_ERR\n", __func__);
				return 0;
			case IB_WC_WR_FLUSH_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_WR_FLUSH_ERR\n", __func__);
				return 0;
			case IB_WC_MW_BIND_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_MW_BIND_ERR\n", __func__);
				return 0;
			case IB_WC_BAD_RESP_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_BAD_RESP_ERR\n", __func__);
				return 0;
			case IB_WC_LOC_ACCESS_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_LOCA_ACCESS_ERR\n", __func__);
				return 0;
			case IB_WC_REM_INV_REQ_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_REM_INV_REQ_ERR\n", __func__);
				return 0;
			case IB_WC_REM_OP_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_REM_OP_ERR\n", __func__);
				return 0;
			case IB_WC_RETRY_EXC_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_RETRY_EXC_ERR\n", __func__);
				return 0;
			case IB_WC_RNR_RETRY_EXC_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_RNR_RETRY_EXC_ERR\n", __func__);
				return 0;
			case IB_WC_LOC_RDD_VIOL_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_LOC_RDD_VIOL_ERR\n", __func__);
				return 0;
			case IB_WC_REM_INV_RD_REQ_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_REM_INV_RD_REQ_ERR\n", __func__);
				return 0;
			case IB_WC_REM_ABORT_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_REM_ABORT_ERR\n", __func__);
				return 0;
			case IB_WC_INV_EECN_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_INV_EECN_ERR\n", __func__);
				return 0;
			case IB_WC_INV_EEC_STATE_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_INV_EEC_STATE_ERR\n", __func__);
				return 0;
			case IB_WC_FATAL_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_FATAL_ERR\n", __func__);
				return 0;
			case IB_WC_RESP_TIMEOUT_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_RESP_TIMEOUT_ERR\n", __func__);
				return 0;
			case IB_WC_GENERAL_ERR:
				printk(KERN_ALERT "[%s]: IB_WC_GENERAL_ERR\n", __func__);
				return 0;
			default:
				printk(KERN_ALERT "[%s]: unknown\n", __func__);
		}
		return 0;
	}
	else
		printk("[%s]: polling succeeded IB_WC_SUCCESS\n", __func__);
	return cnt;
}


static void poll_cq(struct ib_cq* cq, void* cq_ctx){
	struct ib_wc wc;
	int ret;
	while(1){
		ret = ib_req_notify_cq(cq, 0);
		if(ret){
			printk("[%s]: ib_req_notify_cq failed %d\n", __func__, ret);
			return;
		}
		while(ib_poll_cq(cq, 1, &wc)){}
	}
}

static int client_poll_cq(struct ib_cq* cq){
	struct ib_wc wc;
	int ne, i, ret;
	uint32_t size;
	allow_signal(SIGKILL);
	while(1){
		do{
			ne = ib_poll_cq(cq, 1, &wc);
			if(ne < 0){
				printk(KERN_ALERT "[%s]: ib_poll_cq failed (%d)\n", __func__, ne);
				return 1;
			}
#ifdef DEBUG
			schedule();
			if(kthread_should_stop()){
				printk("[%s]: stop and return \n", __func__);
				return 0;
			}
#endif
		}while(ne < 1);

		if(wc.status != IB_WC_SUCCESS){
			printk(KERN_ALERT "[%s]: ib_poll_cq returned failure status (%d)\n", __func__, wc.status);
			return 1;
		}
		//dprintk("[%s]: polled a work request\n", __func__);
		post_recv();

		if((int)wc.opcode == IB_WC_RECV_RDMA_WITH_IMM){
			int node_id, pid, type, tx_state;
			uint32_t num;
			//bit_unmask(htonl(wc.ex.imm_data), &node_id, &pid, &type, &tx_state, &num);
			bit_unmask(ntohl(wc.ex.imm_data), &node_id, &pid, &type, &tx_state, &num);
			//dprintk("[%s]: node_id(%d), pid(%d), type(%d), tx_state(%d), num(%d)\n", __func__, node_id, pid, type, tx_state, num);
			if(type == MSG_WRITE_REQUEST_REPLY){
				uint64_t* addr = GET_LOCAL_META_REGION(ctx->local_mm, pid);
				struct request_struct* new_request = kmem_cache_alloc(request_cache, GFP_KERNEL);
				dprintk("[%s]: [MSG_WRITE_REQUEST_REPLY] received from server with num pages %d, pid %d\n", __func__, num, pid);
				new_request->type = MSG_WRITE;
				new_request->pid = pid;
				new_request->num = num;
				new_request->remote_mm = *addr;

				spin_lock(&list_lock);
				list_add_tail(&(new_request->list), &request_list.list);
				spin_unlock(&list_lock);
			}
			else if(type == MSG_WRITE_REPLY){
				dprintk("[%s]: [MSG_WRITE_REPLY] received from server with num pages %d, pid %d\n", __func__, num, pid);
				/* TODO: how to notify committed requests?? */
				if(tx_state == TX_WRITE_COMMITTED){
					dprintk("[%s]: remote server committed write request\n", __func__);
					unset_bit(pid);
				}
				else{
					dprintk("[%s]: remote server aborted write request\n", __func__);
					unset_bit(pid);
				}
			}
			else if(type == MSG_READ_REQUEST_REPLY){
				dprintk("[%s]: [MSG_READ_REQUEST_REPLY] received from server with num pages %d, pid %d\n", __func__, num, pid);
				if(tx_state == TX_READ_READY){
					uint64_t* addr = GET_LOCAL_META_REGION(ctx->local_mm, pid);
					struct request_struct* new_request = kmem_cache_alloc(request_cache, GFP_KERNEL);
					new_request->type = MSG_READ;
					new_request->pid = pid;
					new_request->num = num;
					new_request->remote_mm = *addr;

					spin_lock(&list_lock);
					list_add_tail(&(new_request->list), &request_list.list);
					spin_unlock(&list_lock);
				}
				else{
					dprintk("[%s]: remote server aborted read request\n", __func__);
					unset_bit(pid);
				}
			}
			else{
				printk(KERN_ALERT "[%s]: received weired type msg from remote server (%d)\n", __func__, type);
			}
		}
		else{
			printk(KERN_ALERT "[%s]: received weired opcode from remote server (%d)\n", __func__, (int)wc.opcode);
		}
	}
}


static void add_one(struct ib_device* dev){
	ib_dev = dev;
	ctx_pd = ib_alloc_pd(dev, 0);
	if(!ctx_pd)
		printk(KERN_ALERT "[%s]: ib_alloc_pd failed\n", __func__);
}

//uint64_t bit_mask(int node_id, int pid, int type, uint32_t size){
uint32_t bit_mask(int node_id, int pid, int type, int state, uint32_t num){
	uint32_t target = (((uint32_t)node_id << 24) | ((uint32_t)pid << 16) | ((uint32_t)type << 12) | ((uint32_t)state << 8) | ((uint32_t)num & 0x000000ff));
	//    uint64_t target = (((uint64_t)node_id << 56) | ((uint64_t)pid << 48) | ((uint64_t)type << 32) | ((uint32_t)size));
	//    uint64_t target = (((uint64_t)node_id << 56) | ((uint64_t)pid << 48) | ((uint64_t)type << 32) | ((uint64_t)size & 0xffffffff));
	return target;
}

//int bit_unmask(uint64_t target, int* node_id, int* pid, int* type, uint32_t* size){
void bit_unmask(uint32_t target, int* node_id, int* pid, int* type, int* state, uint32_t* num){
	*num = (uint32_t)(target & 0x000000ff);
	*state = (int)((target >> 8) & 0x0000000f);
	*type = (int)((target >> 12) & 0x0000000f);
	*pid = (int)((target >> 16) & 0x000000ff);
	*node_id = (int)((target >> 24) & 0x000000ff);
	/*
	 *size = (uint32_t)(target & 0xffffffff);
	 *type = (int)((target >> 32) & 0xffff);
	 *pid = (int)((target >> 48) & 0xff);
	 *node_id = (int)((target >> 56) & 0xff);*/
}

//int generate_write_request(void** pages, int num){
int generate_write_request(void** pages, u32 key, u32 index, int num){
	struct request_struct* new_request;
	int pid, i, ret;

	new_request = kmem_cache_alloc(request_cache, GFP_KERNEL);
	pid = find_and_set_nextbit();

	/*
	   struct request_struct* new_request = kmem_cache_alloc(request_cache, GFP_KERNEL);
	   int pid = find_and_set_nextbit();
	   int i;
	   int num = size / PAGE_SIZE;*/

	new_request->type = MSG_WRITE_REQUEST;
	new_request->pid = pid;
	new_request->num = num;
	new_request->key = key;
	new_request->index = index;
	for(i=0; i<num; i++){
		new_request->pages[i] = pages[i];
	}

	spin_lock(&list_lock);
	list_add_tail(&(new_request->list), &request_list.list);
	spin_unlock(&list_lock);
	//dprintk("[%s]: added write request into request_list\n", __func__);
	/* TODO: we need some indexes or message queues 
	   to notify current tx state
	   */

	while(atomic_read(&ctx->process_state[pid]) == PROCESS_STATE_ACTIVE){
		cpu_relax();
	}
	/*
	   ret = ctx->process_state[pid];
	   while(ret == PROCESS_STATE_ACTIVE){
	   ret = ctx->process_state[pid];
	   }*/
	//dprintk("[%s]: completed request processing\n", __func__);
	return 0;
}
EXPORT_SYMBOL(generate_write_request);

//int generate_read_request(void** pages, uint64_t* keys, int num){
int generate_read_request(void** pages, u32 key, u32 index, int num){
	struct request_struct* new_request = kmem_cache_alloc(request_cache, GFP_KERNEL);
	int pid = find_and_set_nextbit();
	int i, ret;

	new_request->type = MSG_READ_REQUEST;
	new_request->pid = pid;
	new_request->num = num;
	new_request->index = index;
	new_request->key = key;
#if 0
	for(i=0; i<num; i++){
		new_request->keys[i] = keys[i];
	}
#endif

	spin_lock(&list_lock);
	list_add_tail(&(new_request->list), &request_list.list);
	spin_unlock(&list_lock);
	//dprintk("[%s]: added read request with key %llu into request_list\n", __func__, new_request->keys[0]);
	/* TODO: we need some indexes or message queues 
	   to notify current tx state
	   */

	while(atomic_read(&ctx->process_state[pid]) == PROCESS_STATE_ACTIVE){
		cpu_relax();
	}
	for(i=0; i<num; i++){
		memcpy(pages[i], ctx->temp_log[pid][i], PAGE_SIZE);
		//pages[i] = (void*)ctx->temp_log[pid][i];
	}
	atomic_set(&ctx->process_state[pid], PROCESS_STATE_DONE);
	/*
	   ret = ctx->process_state[pid];
	   while(ret == PROCESS_STATE_ACTIVE){
	   ret = ctx->process_state[pid];
	   }*/
	//dprintk("[%s]: completed request processing\n", __func__);
	return 0;
}
EXPORT_SYMBOL(generate_read_request);

int find_and_set_nextbit(void){
	//uint64_t* bitmap = &ctx->bitmap;
	//int ret = 0;
	int bit = 1;
	//uint64_t bit = 1;
	while(1){
		bit = find_first_zero_bit(ctx->bitmap, MAX_PROCESS);
		if(test_and_set_bit(bit, ctx->bitmap) == 0){
			atomic_set(&ctx->process_state[bit], PROCESS_STATE_ACTIVE);
			return bit;
		}
	}
	/*
	   if((*bitmap & bit) == 0){
//dprintk("[%s]: setting bitmap and returning id %d\n", __func__, ret);
	 *bitmap |= bit;
	 atomic_set(&ctx->process_state[ret], PROCESS_STATE_ACTIVE);
//atomic_inc(&ctx->process_state[ret]);
//ctx->process_state[ret] = PROCESS_STATE_ACTIVE;
return ret;
}
bit <<= 1;
ret++;
if(ret == 63){
//if(ret/BITS_PER_LONG){
//dprintk("[%s]: bitmap full\n", __func__);
ret = 0;
bit = 1;
}
}*/
	printk(KERN_ALERT "[%s]: bitmap find err\n", __func__);
	return -1;
}

void unset_bit(int idx){
	//    uint64_t* bitmap = &ctx->bitmap;
	//    uint64_t bit = (uint64_t)~(1 << idx);
	//    clear_bit(idx, ctx->bitmap);
	if(test_and_clear_bit(idx, ctx->bitmap) != 1){
		printk(KERN_ALERT "[%s]: bitmap setting has gone something wrong!!\n", __func__);
	}

	/*    if((*bitmap & ~bit) == 0){
		  printk(KERN_ALERT "[%s]: bitmap setting has gone something wrong!!\n", __func__);
		  }
	 *bitmap &= bit;*/
	//dprintk("[%s]: unsetting bitmap with id %d\n", __func__, idx);
	atomic_set(&ctx->process_state[idx], PROCESS_STATE_IDLE);
	//atomic_dec(&ctx->process_state[idx]);
	//ctx->process_state[idx] = PROCESS_STATE_IDLE;
}

void handle_write_request(int pid, int num, void** request_pages){
	uint64_t offset = pid * REQUEST_SIZE * NUM_ENTRY;
	void* pages[num];
	int i;
	//uint64_t* addr = ctx->local_mm + pid*REQUEST_SIZE*NUM_ENTRY;
	//void* dma_addr = (void*)ctx->local_dma_addr + pid*REQUEST_SIZE*NUM_ENTRY;
	//*addr = 1111;
	//dprintk("[%s]: start posting meta request \n", __func__);

	//post_meta_request(pid, MSG_WRITE_REQUEST, num, TX_WRITE_BEGIN, 8, dma_addr, offset);
	post_meta_request(pid, MSG_WRITE_REQUEST, num, TX_WRITE_BEGIN, 0, NULL, offset);

	/* eager pre-allocation */

	for(i=0; i<num; i++){
		pages[i] = kmem_cache_alloc(page_cache, GFP_KERNEL);
		ctx->temp_log[pid][i] = (uint64_t)pages[i];
		memcpy(pages[i], request_pages[i], PAGE_SIZE);
	}
	//dprintk("[%s]: pre-allocated pages\n", __func__);
}

void handle_write(int pid, int num, uint64_t remote_mm){
	uintptr_t addr[num];
	void* pages[num];
	int i;

	for(i=0; i<num; i++){
		pages[i] = (void*)ctx->temp_log[pid][i];
		addr[i] = ib_reg_mr_addr(pages[i], PAGE_SIZE);
	}

	post_write_request_batch(pid, MSG_WRITE, num, addr, remote_mm, num);

	for(i=0; i<num; i++){
		ib_dereg_mr_addr(addr[i], PAGE_SIZE);
		kmem_cache_free(page_cache, pages[i]);
	}
}

void handle_read_request(int pid, int num, uint64_t* keys){
	uint64_t* addr = GET_LOCAL_META_REGION(ctx->local_mm, pid);
	uint64_t dma_addr = GET_LOCAL_META_REGION(ctx->local_dma_addr, pid);
	uint64_t offset = pid * REQUEST_SIZE * NUM_ENTRY;
	void* pages[num];
	int i;

	for(i=0; i<num; i++){
		*(addr + i*sizeof(uint64_t)) = keys[i];
	}

	post_meta_request_batch(pid, MSG_READ_REQUEST, num, TX_READ_BEGIN, sizeof(uint64_t), dma_addr, offset, num);

	/* eager pre-allocation */
	for(i=0; i<num; i++){
		pages[i] = kmem_cache_alloc(page_cache, GFP_KERNEL);
		ctx->temp_log[pid][i] = (uint64_t)pages[i];
	}
}

void handle_read(int pid, int num, uint64_t remote_mm){
	uintptr_t addr[num];
	void* pages[num];
	uint64_t offset = pid * REQUEST_SIZE * NUM_ENTRY;
	int i;

	for(i=0; i<num; i++){
		pages[i] = (void*)ctx->temp_log[pid][i];
		addr[i] = ib_reg_mr_addr(pages[i], PAGE_SIZE);
	}
	post_read_request_batch(addr, remote_mm, num);

	atomic_set(&ctx->process_state[pid], PROCESS_STATE_WAIT);
	while(atomic_read(&ctx->process_state[pid]) == PROCESS_STATE_WAIT){
		cpu_relax();
	}

	/* TODO: returning read pages to requested process */
	for(i=0; i<num; i++){
		ib_dereg_mr_addr(addr[i], PAGE_SIZE);
		kmem_cache_free(page_cache, pages[i]);
	}
	/* if read correct pages */
	post_meta_request(pid, MSG_READ_REPLY, num, TX_READ_COMMITTED, 0, NULL, offset);
	/* if read wrong pages */
	//    post_meta_request(pid, MSG_READ_REPLY, size, TX_READ_ABORTED, 0, NULL, offset);

	unset_bit(pid);
}



int event_handler(void){
	struct request_struct* new_request;
	allow_signal(SIGKILL);
	while(1){
		while(list_empty(&(request_list.list))){
			schedule();
			if(kthread_should_stop()){
				printk("[%s]: stopping event_handler\n", __func__);
				return 0;
			}
		}
		spin_lock(&list_lock);
		new_request = list_entry(request_list.list.next, struct request_struct, list);
		spin_unlock(&list_lock);
		//dprintk("[%s]: New request detected with type %d, num_pages %d, pid %d\n", __func__, new_request->type, new_request->num, new_request->pid);

		/* TODO: how about delegating those request process to another thread? */

		if(new_request->type == MSG_WRITE_REQUEST){
			dprintk("[%s]: Processing [MSG_WRITE_REQUEST] with num %d, pid %d\n", __func__, new_request->num, new_request->pid);
			handle_write_request(new_request->pid, new_request->num, new_request->pages);
			dprintk("[%s]: Processed [MSG_WRITE_REQUEST] with num %d, pid %d\n", __func__, new_request->num, new_request->pid);
		}

		else if(new_request->type == MSG_WRITE){
			dprintk("[%s]: Processing [MSG_WRITE] with num %d, pid %d\n", __func__, new_request->num, new_request->pid);
			handle_write(new_request->pid, new_request->num, new_request->remote_mm);
			dprintk("[%s]: Processed [MSG_WRITE] with num %d, pid %d\n", __func__, new_request->num, new_request->pid);
		}
		else if(new_request->type == MSG_READ_REQUEST){
			dprintk("[%s]: Processing [MSG_READ_REQUEST] with num %d, pid %d\n", __func__, new_request->num, new_request->pid);
			handle_read_request(new_request->pid, new_request->num, new_request->keys);
			dprintk("[%s]: Processed [MSG_READ_REQUEST] with num %d, pid %d\n", __func__, new_request->num, new_request->pid);
		}
		else if(new_request->type == MSG_READ){
			dprintk("[%s]: Processing [MSG_READ] with num %d, pid %d\n", __func__, new_request->num, new_request->pid);
			handle_read(new_request->pid, new_request->num, new_request->remote_mm);
			dprintk("[%s]: Processed [MSG_READ] with num %d, pid %d\n", __func__, new_request->num, new_request->pid);
		}
		else{
			printk(KERN_ALERT "[%s]: weired request type (%d)\n", __func__, new_request->type);
		}
		spin_lock(&list_lock);
		list_del(&new_request->list);
		spin_unlock(&list_lock);
		//dprintk(KERN_ALERT "[%s]: removed a list entry\n", __func__);
		kmem_cache_free(request_cache, new_request);
		//dprintk(KERN_ALERT "[%s]: freed the list entry\n", __func__);
	}
	return 0;
}

int rdma_cas(void* addr, uint64_t dest, uint64_t compare, uint64_t swap){
	struct ib_atomic_wr wr;
	const struct ib_send_wr* bad_wr;
	struct ib_sge sge;
	struct ib_wc wc;
	int ret, ne;

	memset(&wr, 0, sizeof(struct ib_atomic_wr));
	memset(&sge, 0, sizeof(struct ib_sge));

	sge.addr = (uintptr_t)addr;
	sge.length = sizeof(uint64_t);
	sge.lkey = ctx->mr->lkey;

	wr.wr.wr_id = 0;
	wr.wr.opcode = IB_WR_ATOMIC_CMP_AND_SWP;
	wr.wr.sg_list = &sge;
	wr.wr.num_sge = 1;
	wr.wr.send_flags = IB_SEND_SIGNALED;
	wr.remote_addr = (uintptr_t)dest;
	wr.rkey = ctx->rkey;
	wr.compare_add = compare;
	wr.swap = swap;

	ret = ib_post_send(ctx->qp, (struct ib_send_wr*)&wr, &bad_wr);
	if(ret){
		printk(KERN_ALERT "[%s]: ib_post_send failed\n", __func__);
		return 1;
	}

	do{
		ne = ib_poll_cq(ctx->send_cq, 1, &wc);
		if(ne < 0){
			printk(KERN_ALERT "[%s]: ib_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);

	if(wc.status != IB_WC_SUCCESS){
		printk(KERN_ALERT "[%s]: sending request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);
		return 1;
	}
	return 0;
}
/*
   int rdma_read_batch(void* addr, uint64_t dest, int size, int batch_size){
   struct ib_rdma_wr wr[batch_size];
   struct ib_rdma_wr wr[batch_size];
   const struct ib_send_wr* bad_wr;
   struct ib_sge sge[batch_size];
   struct ib_wc wc;
   int ret, ne, i;

   memset(sge, 0, sizeof(struct ib_sge)*batch_size);
   memset(wr, 0, sizeof(struct ib_rdma_wr)*batch_size);
   for(i=0; i<batch_size; i++){
   sge[i].addr = (uintptr_t)addr;
   sge[i].length = size;
   sge[i].lkey = ctx->mr->lkey;

   wr[i].wr.wr_id = 0;
   wr[i].wr.opcode = IB_WR_RDMA_READ;
   wr[i].wr.sg_list = &sge[i];
   wr[i].wr.num_sge = 1;
   wr[i].wr.next = (i == batch_size-1) ? NULL : &wr[i+1];
   wr[i].wr.send_flags = IB_SEND_SIGNALED;

   wr[i].remote_addr = (uintptr_t)dest + i*RDMA_BUFFER_SIZE;
   wr[i].rkey = ctx->rkey;
   }

   ret = ib_post_send(ctx->qp, (struct ib_send_wr*)&wr[0], &bad_wr);
   if(ret){
   printk(KERN_ALERT "[%s] ib_post_send failed\n", __func__);
   return 1;
   }

   do{
   ne = ib_poll_cq(ctx->send_cq, 1, &wc);
   if(ne < 0){
   printk(KERN_ALERT "[%s] ib_poll_cq failed\n", __func__);
   return 1;
   }
   }while(ne < 1);

   if(wc.status != IB_WC_SUCCESS){
   printk(KERN_ALERT "[%s] sending request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);
   return 1;
   }
   return 0;
   }*/

int rdma_read(int node_id, int pid, int type, uint32_t size, uintptr_t addr, uint64_t offset){
	struct ib_rdma_wr wr;
	const struct ib_send_wr* bad_wr;
	struct ib_sge sge;
	struct ib_wc wc;
	int ret, ne;

	memset(&sge, 0, sizeof(struct ib_sge));
	memset(&wr, 0, sizeof(struct ib_rdma_wr));

	sge.addr = addr;
	sge.length = size;
	sge.lkey = ctx->mr->lkey;

	//wr.wr.wr_id = bit_mask(node_id, pid, type, size);
	wr.wr.opcode = IB_WR_RDMA_READ;
	wr.wr.sg_list = &sge;
	wr.wr.num_sge = 1;
	wr.wr.send_flags = IB_SEND_SIGNALED;
	wr.remote_addr = (uintptr_t)(ctx->remote_mm + offset);
	wr.rkey = ctx->rkey;

	ret = ib_post_send(ctx->qp, (struct ib_send_wr*)&wr, &bad_wr);
	if(ret){
		printk(KERN_ALERT "[%s] ib_post_send failed\n", __func__);
		return 1;
	}

	do{
		ne = ib_poll_cq(ctx->send_cq, 1, &wc);
		if(ne < 0){
			printk(KERN_ALERT "[%s] ib_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);

	if(wc.status != IB_WC_SUCCESS){
		printk(KERN_ALERT "[%s] sending request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);
		return 1;
	}
	return 0;
}
/*
   int rdma_write_batch(void* addr, uint64_t dest, int size, int batch_size){
   struct ib_rdma_wr wr[batch_size];
   const struct ib_send_wr* bad_wr;
   struct ib_sge sge[batch_size];
   struct ib_wc wc;
   int ret, ne, i;

   memset(sge, 0, sizeof(struct ib_sge)*batch_size);
   memset(wr, 0, sizeof(struct ib_rdma_wr)*batch_size);
   for(i=0; i<batch_size; i++){
   sge[i].addr = (uintptr_t)addr;
   sge[i].length = size;
   sge[i].lkey = ctx->mr->lkey;

   wr[i].wr.wr_id = 0;
   wr[i].wr.opcode = IB_WR_RDMA_WRITE;
   wr[i].wr.sg_list = &sge[i];
   wr[i].wr.num_sge = 1;
   wr[i].wr.next = (i == batch_size-1) ? NULL : wr[i+1];
   wr[i].wr.send_flags = IB_SEND_SIGNALED;

   wr[i].remote_addr = (uintptr_t)dest + i*RDMA_BUFFER_SIZE;
   wr[i].rkey = ctx->rkey;
   }

   ret = ib_post_send(ctx->qp, (struct ib_send_wr*)&wr[0], &bad_wr);
   if(ret){
   printk(KERN_ALERT "[%s] ib_post_send failed\n", __func__);
   return 1;
   }

   do{
   ne = ib_poll_cq(ctx->send_cq, 1, &wc);
   if(ne < 0){
   printk(KERN_ALERT "[%s] ib_poll_cq failed\n", __func__);
   return 1;
   }
   }while(ne < 1);

   if(wc.status != IB_WC_SUCCESS){
   printk(KERN_ALERT "[%s] sending request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);
   return 1;
   }
   return 0;
   }*/

int post_meta_request_batch(int pid, int type, int num, int tx_state, int len, void* addr, uint64_t offset, int batch_size){
	struct ib_rdma_wr wr[batch_size];
	const struct ib_send_wr* bad_wr;
	struct ib_sge sge[batch_size];
	struct ib_wc wc;
	int ret, ne, i;
	//dprintk("[%s]: start posting meta request in batch\n", __func__);

	memset(sge, 0, sizeof(struct ib_sge)*batch_size);
	memset(wr, 0, sizeof(struct ib_rdma_wr)*batch_size);

	for(i=0; i<batch_size; i++){
		sge[i].addr = (uintptr_t)(addr + i*len);
		sge[i].length = len;
		sge[i].lkey = ctx->mr->lkey;

		//wr[i].wr.wr_id = bit_mask(ctx->node_id, pid, type, size);
		wr[i].wr.opcode = IB_WR_RDMA_WRITE_WITH_IMM;
		wr[i].wr.sg_list = &sge[i];
		wr[i].wr.num_sge = 1;
		wr[i].wr.next = (i == batch_size-1) ? NULL : (struct ib_send_wr*)&wr[i+1];
		wr[i].wr.send_flags = (i == batch_size-1) ? IB_SEND_SIGNALED : 0;
		wr[i].wr.ex.imm_data = htonl(bit_mask(ctx->node_id, pid, type, tx_state, num));
		wr[i].remote_addr = (uintptr_t)(ctx->remote_mm + offset + i*len);
		wr[i].rkey = ctx->rkey;

		//dprintk("[%s]: sending imm_data: %lu \thtonl(imm_data): %lu\n", __func__, wr[i].wr.ex.imm_data, htonl(wr[i].wr.ex.imm_data));
	}


	ret = ib_post_send(ctx->qp, (struct ib_send_wr*)&wr[0], &bad_wr);
	if(ret){
		printk(KERN_ALERT "[%s]: ib_post_send failed\n", __func__);
		return 1;
	}

	do{
		ne = ib_poll_cq(ctx->send_cq, 1, &wc);
		if(ne < 0){
			printk(KERN_ALERT "[%s]: ib_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);

	if(wc.status != IB_WC_SUCCESS){
		printk(KERN_ALERT "[%s]: sending request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);
		return 1;
	}
	return 0;
}


int post_meta_request(int pid, int type, int num, int tx_state, int len, void* addr, uint64_t offset){
	ktime_t start, end;

	struct ib_rdma_wr wr;
	const struct ib_send_wr* bad_wr;
	struct ib_sge sge;
	struct ib_wc wc;
	int ret, ne = 0;

	memset(&sge, 0, sizeof(struct ib_sge));
	memset(&wr, 0, sizeof(struct ib_rdma_wr));

	//dprintk("[%s]: start posting meta request\n", __func__);

	sge.addr = (uintptr_t)addr;
	//sge.length = sizeof(uint64_t);
	sge.length = len;
	sge.lkey = ctx->mr->lkey;

	//wr.wr.wr_id = bit_mask(ctx->node_id, pid, type, size);
	//wr.wr.opcode = IB_WR_RDMA_WRITE;
	wr.wr.opcode = IB_WR_RDMA_WRITE_WITH_IMM;
	wr.wr.sg_list = &sge;
	wr.wr.num_sge = 1;
	wr.wr.send_flags = IB_SEND_SIGNALED;
	//wr.wr.ex.imm_data = 1111;
	//wr.wr.ex.imm_data = htonl(tx_state);
	wr.wr.ex.imm_data = htonl(bit_mask(ctx->node_id, pid, type, tx_state, num));
	wr.remote_addr = (uintptr_t)(ctx->remote_mm + offset);
	wr.rkey = ctx->rkey;

	//dprintk("[%s]: target addr: %x, target rkey %lu\n", __func__, wr.remote_addr, wr.rkey);

	//dprintk("[%s]: sending imm_data: %lu \thtonl(imm_data): %lu\n", __func__, wr.wr.ex.imm_data, htonl(wr.wr.ex.imm_data));

	ret = ib_post_send(ctx->qp, (struct ib_send_wr*)&wr.wr, &bad_wr);
	if(ret){
		printk(KERN_ALERT "[%s]: ib_post_send failed\n", __func__);
		return 1;
	}
	//dprintk("[%s]: ib_post_send succeeded\n", __func__);

	do{
		ne = ib_poll_cq(ctx->send_cq, 1, &wc);
		if(ne < 0){
			printk(KERN_ALERT "[%s] ib_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);
	//dprintk("[%s] ib_poll_cq succeeded\n", __func__);

	if(wc.status != IB_WC_SUCCESS){
		printk(KERN_ALERT "[%s] sending request failed status %s(%d)\n", __func__, ib_wc_status_msg(wc.status), wc.status);
		return 1;
	}
	//dprintk("[%s]: wc statis is success\n", __func__);
	return 0;
}

int post_read_request_batch(uintptr_t* addr, uint64_t offset, int batch_size){
	struct ib_rdma_wr wr[batch_size];
	const struct ib_send_wr* bad_wr;
	struct ib_sge sge[batch_size];
	struct ib_wc wc;
	int ret, ne, i;

	memset(sge, 0, sizeof(struct ib_sge)*batch_size);
	memset(wr, 0, sizeof(struct ib_rdma_wr)*batch_size);

	for(i=0; i<batch_size; i++){
		sge[i].addr = addr[i];
		sge[i].length = PAGE_SIZE;
		sge[i].lkey = ctx->mr->lkey;

		//wr[i].wr.wr_id = bit_mask(ctx->node_id, pid, type, size);
		wr[i].wr.opcode = IB_WR_RDMA_READ;
		wr[i].wr.sg_list = &sge[i];
		wr[i].wr.num_sge = 1;
		wr[i].wr.next = (i == batch_size-1) ? NULL : (struct ib_send_wr*)&wr[i+1];
		wr[i].wr.send_flags = (i == batch_size-1) ? IB_SEND_SIGNALED : 0;
		wr[i].remote_addr = (uintptr_t)(offset + i*PAGE_SIZE);
		wr[i].rkey = ctx->rkey;
		//dprintk("[%s]: target addr: %llx\n", __func__, wr[i].remote_addr);
	}

	ret = ib_post_send(ctx->qp, (struct ib_send_wr*)&wr[0].wr, &bad_wr);
	if(ret){
		printk(KERN_ALERT "[%s]: ib_post_send failed\n", __func__);
		return 1;
	}

	do{
		ne = ib_poll_cq(ctx->send_cq, 1, &wc);
		if(ne < 0){
			printk(KERN_ALERT "[%s]: ib_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);

	if(wc.status != IB_WC_SUCCESS){
		printk(KERN_ALERT "[%s]: sending request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);
		return 1;
	}
	return 0;
}

int post_read_request(uintptr_t addr, uint64_t offset){
	struct ib_rdma_wr wr;
	const struct ib_send_wr* bad_wr;
	struct ib_sge sge;
	struct ib_wc wc;
	int ret, ne;

	memset(&sge, 0, sizeof(struct ib_sge));
	memset(&wr, 0, sizeof(struct ib_rdma_wr));

	sge.addr = addr;
	sge.length = PAGE_SIZE;
	sge.lkey = ctx->mr->lkey;

	//wr.wr.wr_id = bit_mask(ctx->node_id, pid, type, size);
	wr.wr.opcode = IB_WR_RDMA_READ;
	wr.wr.sg_list = &sge;
	wr.wr.num_sge = 1;
	wr.wr.send_flags = IB_SEND_SIGNALED;
	wr.remote_addr = (uintptr_t)offset;
	wr.rkey = ctx->rkey;

	ret = ib_post_send(ctx->qp, (struct ib_send_wr*)&wr.wr, &bad_wr);
	if(ret){
		printk(KERN_ALERT "[%s] ib_post_send failed\n", __func__);
		return 1;
	}

	do{
		ne = ib_poll_cq(ctx->send_cq, 1, &wc);
		if(ne < 0){
			printk(KERN_ALERT "[%s] ib_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);

	if(wc.status != IB_WC_SUCCESS){
		printk(KERN_ALERT "[%s] sending request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);
		return 1;
	}
	return 0;
}


int post_write_request(int pid, int type, int num, uintptr_t addr, uint64_t offset){
	struct ib_rdma_wr wr;
	const struct ib_send_wr* bad_wr;
	struct ib_sge sge;
	struct ib_wc wc;
	int ret, ne;

	memset(&sge, 0, sizeof(struct ib_sge));
	memset(&wr, 0, sizeof(struct ib_rdma_wr));

	sge.addr = addr;
	sge.length = PAGE_SIZE;
	sge.lkey = ctx->mr->lkey;

	//    wr.wr.wr_id = bit_mask(ctx->node_id, pid, type, size);
	wr.wr.opcode = IB_WR_RDMA_WRITE_WITH_IMM;
	wr.wr.sg_list = &sge;
	wr.wr.num_sge = 1;
	wr.wr.send_flags = IB_SEND_SIGNALED;
	wr.wr.ex.imm_data = htonl(bit_mask(ctx->node_id, pid, type, TX_WRITE_BEGIN, num));
	//    wr.wr.ex.imm_data = htonl(0);
	wr.remote_addr = (uintptr_t)offset;
	wr.rkey = ctx->rkey;
	//dprintk("[%s]: sending imm_data: %lu \thtonl(imm_data): %lu\n", __func__, wr.wr.ex.imm_data, htonl(wr.wr.ex.imm_data));
	//dprintk("[%s] target addr: %llx, target rkey %lu\n", __func__, (uint64_t)wr.remote_addr, ctx->rkey);

	ret = ib_post_send(ctx->qp, (struct ib_send_wr*)&wr.wr, &bad_wr);
	if(ret){
		printk(KERN_ALERT "[%s] ib_post_send failed\n", __func__);
		return 1;
	}
	//dprintk("[%s] ib_post_send succeeded\n", __func__);

	do{
		ne = ib_poll_cq(ctx->send_cq, 1, &wc);
		if(ne < 0){
			printk(KERN_ALERT "[%s] ib_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);
	//dprintk("[%s] ib_poll_cq succeeded\n", __func__);

	if(wc.status != IB_WC_SUCCESS){
		printk(KERN_ALERT "[%s] sending request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);
		return 1;
	}
	return 0;
}

int post_write_request_batch(int pid, int type, int num, uintptr_t* addr, uint64_t offset, int batch_size){
	struct ib_rdma_wr wr[batch_size];
	const struct ib_send_wr* bad_wr;
	struct ib_sge sge[batch_size];
	struct ib_wc wc;
	int ret, ne, i;

	memset(sge, 0, sizeof(struct ib_sge)*batch_size);
	memset(wr, 0, sizeof(struct ib_rdma_wr)*batch_size);

	for(i=0; i<batch_size; i++){
		sge[i].addr = addr[i];
		sge[i].length = PAGE_SIZE;
		sge[i].lkey = ctx->mr->lkey;

		//	wr[i].wr.wr_id = bit_mask(ctx->node_id, pid, type, size);
		wr[i].wr.opcode = IB_WR_RDMA_WRITE_WITH_IMM;
		wr[i].wr.sg_list = &sge[i];
		wr[i].wr.num_sge = 1;
		wr[i].wr.next = (i == batch_size-1) ? NULL : (struct ib_send_wr*)&wr[i+1];
		wr[i].wr.send_flags = (i == batch_size-1) ? IB_SEND_SIGNALED : 0;
		wr[i].wr.ex.imm_data = htonl(bit_mask(ctx->node_id, pid, type, TX_WRITE_BEGIN, num));
		//wr[i].wr.ex.imm_data = htonl(0);
		wr[i].remote_addr = (uintptr_t)(offset + i*PAGE_SIZE);
		//dprintk("[%s]: vaddr = %llx\n", __func__, wr[i].remote_addr);
		wr[i].rkey = ctx->rkey;
		//dprintk("[%s]: sending imm_data: %lu \thtonl(imm_data): %lu\n", __func__, wr[i].wr.ex.imm_data, htonl(wr[i].wr.ex.imm_data));
	}

	//dprintk("[%s]: target addr: %llx, target rkey %lu\n", __func__, (uint64_t)wr[0].remote_addr, ctx->rkey);

	ret = ib_post_send(ctx->qp, &wr[0].wr, &bad_wr);
	if(ret){
		printk(KERN_ALERT "[%s]: ib_post_send failed\n", __func__);
		return 1;
	}
	//dprintk("[%s]: ib_post_send succeeded\n", __func__);

	do{
		ne = ib_poll_cq(ctx->send_cq, 1, &wc);
		if(ne < 0){
			printk(KERN_ALERT "[%s]: ib_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);
	//dprintk("[%s]: ib_poll_cq succeeded\n", __func__);

	if(wc.status != IB_WC_SUCCESS){
		printk(KERN_ALERT "[%s]: sending request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);
		return 1;
	}
	return 0;
}

int post_recv(void){
	struct ib_recv_wr wr;
	const struct ib_recv_wr* bad_wr;
	struct ib_sge sge;
	int ret;

	memset(&wr, 0, sizeof(struct ib_recv_wr));
	memset(&sge, 0, sizeof(struct ib_sge));

	sge.addr = NULL;
	sge.length = 0;
	sge.lkey = ctx->mr->lkey;

	wr.wr_id = 0;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.next = NULL;

	ret = ib_post_recv(ctx->qp, &wr, &bad_wr);
	if(ret){
		printk(KERN_ALERT "[%s] ib_post_recv failed\n", __func__);
		return 1;
	}
	return 0;
}
/*
   int rdma_write(int node_id, int type, uint32_t size, void* addr, uint64_t offset){
   struct ib_rdma_wr wr;
   const struct ib_send_wr* bad_wr;
   struct ib_sge sge;
   struct ib_wc wc;
   int ret, ne;

   memset(&sge, 0, sizeof(struct ib_sge));
   memset(&wr, 0, sizeof(struct ib_rdma_wr));

   sge.addr = (uintptr_t)addr;
   sge.length = size;
   sge.lkey = ctx->mr->lkey;

   wr.wr.wr_id = bit_mask(node_id, type, size);
   wr.wr.opcode = IB_WR_RDMA_WRITE;
   wr.wr.sg_list = &sge;
   wr.wr.num_sge = 1;
   wr.wr.send_flags = IB_SEND_SIGNALED;
   wr.remote_addr = (uintptr_t)ctx->remote_mm + offset;
   wr.rkey = ctx->rkey;

   dprintk("[%s] target addr: %llu, target rkey %lu\n", (uint64_t)wr.remote_addr, ctx->rkey);

   ret = ib_post_send(ctx->qp, (struct ib_send_wr*)&wr, &bad_wr);
   if(ret){
   printk(KERN_ALERT "[%s] ib_post_send failed\n", __func__);
   return 1;
   }
   dprintk("[%s] ib_post_send succeeded\n", __func__);

   do{
   ne = ib_poll_cq(ctx->send_cq, 1, &wc);
   if(ne < 0){
   printk(KERN_ALERT "[%s] ib_poll_cq failed\n", __func__);
   return 1;
   }
   }while(ne < 1);
   dprintk("[%s] ib_poll_cq succeeded\n", __func__);

   if(wc.status != IB_WC_SUCCESS){
   printk(KERN_ALERT "[%s] sending request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);
   return 1;
   }
   return 0;
   }*/

int rdma_send(void* addr, int size){
	struct ib_send_wr wr;
	const struct ib_send_wr* bad_wr;
	struct ib_sge sge;
	struct ib_wc wc;
	int ret, ne;

	memset(&sge, 0, sizeof(struct ib_sge));
	memset(&wr, 0, sizeof(struct ib_send_wr));

	sge.addr = (uintptr_t)addr;
	sge.length = size;
	sge.lkey = ctx->mr->lkey;

	wr.wr_id = 0;
	wr.opcode = IB_WR_SEND;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	//    wr.send_flags = IB_SEND_SIGNALED;

	ret = ib_post_send(ctx->qp, (struct ib_send_wr*)&wr, &bad_wr);
	if(ret){
		printk(KERN_ALERT "[%s] ib_post_send failed\n", __func__);
		return 1;
	}
	//dprintk("[%s] posted ib_post_send request\n", __func__);

	do{
		ne = ib_poll_cq(ctx->send_cq, 1, &wc);
		if(ne < 0){
			printk(KERN_ALERT "[%s] ib_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);

	if(wc.status != IB_WC_SUCCESS){
		printk(KERN_ALERT "[%s] sending request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);
		return 1;
	}
	return 0;
}

int rdma_recv(void* addr, int size){
	struct ib_recv_wr wr;
	const struct ib_recv_wr* bad_wr;
	struct ib_sge sge;
	int ret, ne;

	memset(&sge, 0, sizeof(struct ib_sge));
	memset(&wr, 0, sizeof(struct ib_recv_wr));

	sge.addr = (uintptr_t)addr;
	sge.length = size;
	sge.lkey = ctx->mr->lkey;

	wr.wr_id = 0;
	wr.sg_list = &sge;
	wr.num_sge = 1;

	ret = ib_post_recv(ctx->qp, (struct ib_recv_wr*)&wr, &bad_wr);
	if(ret){
		printk(KERN_ALERT "[%s] ib_post_recv failed\n", __func__);
		return 1;
	}

	return 0;
}


int modify_qp(int my_psn, int sl, struct node_info* remote){
	int ret;
	struct ib_qp_attr attr;

	memset(&attr, 0, sizeof(struct ib_qp_attr));
	attr.qp_state = IB_QPS_INIT;
	attr.port_num = ib_port;
	attr.pkey_index = 0;
	//    attr.qp_access_flags = 0;
	//    attr.qp_access_flags = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ | IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_ATOMIC;
	attr.qp_access_flags = IB_ACCESS_REMOTE_READ | IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_ATOMIC;

	ret = ib_modify_qp(ctx->qp, &attr,
			IB_QP_STATE	|
			IB_QP_PORT	|
			IB_QP_PKEY_INDEX |
			IB_QP_ACCESS_FLAGS);
	if(ret){
		printk(KERN_ALERT "[%s] ib_modify_qp to INIT failed\n", __func__);
		return 1;
	}
	//dprintk("[%s] ib_modify_qp to INIT succeeded\n", __func__);

	memset(&attr, 0, sizeof(struct ib_qp_attr));
	attr.qp_state = IB_QPS_RTR;
	//    attr.path_mtu = mtu;
	attr.path_mtu = IB_MTU_4096;
	attr.dest_qp_num = remote->qpn;
	//    attr.rq_psn = 0;
	attr.rq_psn = remote->psn;
	attr.max_dest_rd_atomic = 16;
	//    attr.max_dest_rd_atomic = 10;
	attr.min_rnr_timer = 12;
	attr.ah_attr.type = RDMA_AH_ATTR_TYPE_IB;
	attr.ah_attr.ib.dlid = remote->lid;
	attr.ah_attr.ib.src_path_bits = 0;
	attr.ah_attr.sl = 0;
	//    attr.ah_attr.sl = sl;
	attr.ah_attr.port_num = ib_port;
	//    attr.ah_attr.grh = 0;

	if(remote->gid.global.interface_id){
		//	attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = remote->gid;
		attr.ah_attr.grh.sgid_index = -1;
	}

	ret = ib_modify_qp(ctx->qp, &attr, 
			IB_QP_STATE		|
			IB_QP_PATH_MTU	|
			IB_QP_DEST_QPN	|
			IB_QP_RQ_PSN	|
			IB_QP_MAX_DEST_RD_ATOMIC |
			IB_QP_MIN_RNR_TIMER	|
			IB_QP_AV);
	if(ret){
		printk(KERN_ALERT "[%s] ib_modify_qp to RTR failed\n", __func__);
		if(ret == -EINVAL)
			printk("returned -EINVAL\n");
		else if(ret == -ENOMEM)
			printk("returned -ENOMEM\n");
		else
			printk("returned unknown error\n");

		if(ctx->qp->counter)
			printk("ctx->qp->counter!!\n");
		else
			printk("ctx->qp->counter is not set\n");

		return 1;
	}
	//dprintk("[%s] ib_modify_qp to RTR succeeded\n", __func__);

	memset(&attr, 0, sizeof(struct ib_qp_attr));
	attr.qp_state = IB_QPS_RTS;
	//    attr.sq_psn = 0;
	attr.timeout = 14;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	attr.sq_psn = my_psn;
	attr.max_rd_atomic = 16;
	attr.max_dest_rd_atomic = 16;
	//    attr.max_rd_atomic = 10;

	ret = ib_modify_qp(ctx->qp, &attr,
			IB_QP_STATE		|
			IB_QP_SQ_PSN	|
			IB_QP_TIMEOUT	|
			IB_QP_RETRY_CNT	|
			IB_QP_RNR_RETRY	|
			IB_QP_SQ_PSN	|
			IB_QP_MAX_QP_RD_ATOMIC);
	if(ret){
		printk(KERN_ALERT "[%s] ib_modify_qp to RTS failed\n", __func__);
		return 1;
	}
	//dprintk("[%s] ib_modify_qp to RTS succeeded\n", __func__);


	return 0;
}

int query_qp(struct ib_qp* qp){
	struct ib_qp_attr qp_attr;
	struct ib_qp_init_attr init_attr;
	int ret;

	ret = ib_query_qp(qp, &qp_attr, IB_QP_STATE, &init_attr);
	switch(qp_attr.qp_state){
		case IB_QPS_INIT:
			printk(KERN_ALERT "[%s] current qp state is ib_qps_init\n", __func__);
			break;
		case IB_QPS_RTR:
			printk(KERN_ALERT "[%s] current qp state is ib_qps_rtr\n", __func__);
			break;
		case IB_QPS_RTS:
			printk(KERN_ALERT "[%s] current qp state is ib_qps_rts\n", __func__);
			break;
		case IB_QPS_RESET:
			printk(KERN_ALERT "[%s] current qp state is ib_qps_reset\n", __func__);
			break;
		case IB_QPS_SQD:
			printk(KERN_ALERT "[%s] current qp state is ib_qps_sqd\n", __func__);
			break;
		case IB_QPS_SQE:
			printk(KERN_ALERT "[%s] current qp state is ib_qps_sqe\n", __func__);
			break;
		case IB_QPS_ERR:
			printk(KERN_ALERT "[%s] current qp state is ib_qps_err\n", __func__);
			break;
		default:
			printk(KERN_ALERT "[%s] current qp state is unknown\n", __func__);
	}

	return 1;
}



int tcp_send(struct socket* sock, char* buf, int len){
	struct msghdr msg;
	struct kvec iov;

	if(!sock->sk){
		printk(KERN_ALERT "[TCP] socket is NULL\n");
		return 1;
	}

	memset(&msg, 0, sizeof(struct msghdr));
	iov.iov_base = buf;
	iov.iov_len = len;
	msg.msg_flags = MSG_WAITALL;
	msg.msg_iter.iov = (struct iovec*)&iov;

	kernel_sendmsg(sock, &msg, &iov, 1, iov.iov_len);
	return 0;
}

int tcp_recv(struct socket* sock, char* buf, int len){
	struct msghdr msg;
	struct kvec iov;

	if(!sock->sk){
		printk(KERN_ALERT "[TCP] socket is NULL\n");
		return 1;
	}

	memset(&msg, 0, sizeof(struct msghdr));
	iov.iov_base = buf;
	iov.iov_len = len;
	msg.msg_flags = MSG_WAITALL;
	msg.msg_iter.iov = (struct iovec*)&iov;

	kernel_recvmsg(sock, &msg, &iov, 1, iov.iov_len, msg.msg_flags);
	return 0;
}

static int param_port = 0;
static char param_ip[64];
module_param_named(port, param_port, int, 0444);
module_param_string(ip, param_ip, sizeof(param_ip), 0444);

int establish_conn(void){
	struct node_info local, remote;
	int ret;
	struct sockaddr_in addr;
	struct socket* sock;
	int fd;
	int gid_idx = 0;
	union ib_gid gid;
	char ip[64];
	int port = param_port;
	strncpy(ip, param_ip, strlen(param_ip));

	ctx->size = LOCAL_META_REGION_SIZE;
	ctx->local_mm = (uint64_t)kmalloc(ctx->size, GFP_KERNEL);
	ctx->local_dma_addr = ib_reg_mr_addr((void*)ctx->local_mm, ctx->size);

	memset(&local, 0, sizeof(struct node_info));
	memset(&remote, 0, sizeof(struct node_info));
	memset(&addr, 0, sizeof(struct sockaddr_in));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = inet_addr(ip);

	fd = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if(fd < 0){
		printk(KERN_ALERT "Failed to establish tcp connection (%d)\n", fd);
		return 1;
	}
	//dprintk("[TCP] socket has been created\n");

	ret = sock->ops->connect(sock, (struct sockaddr*)&addr, sizeof(addr), O_RDWR);
	if(ret){
		printk(KERN_ALERT "[TCP] socket connection failed (%d)\n", ret);
		ksys_close(fd);
		return 1;
	}
	//dprintk("[TCP] socket has been connected to server\n");

	/* TODO: establishing QP connections
	   with metadata exchange through TCP sockets */

	ret = tcp_recv(sock, (char*)&remote, sizeof(struct node_info));
	if(ret){
		printk(KERN_ALERT "[TCP] recv failed\n");
		ksys_close(fd);
		return 1;
	}
	//dprintk("[TCP] received node_id(%d), lid(%d), qpn(%d), psn(%d), mm(%llu), rkey(%u)\n", remote.node_id, remote.lid, remote.qpn, remote.psn, remote.mm, remote.rkey);
	ctx->node_id = remote.node_id;
	ctx->remote_mm = remote.mm;
	ctx->rkey = remote.rkey;

	ret = rdma_query_gid((struct ib_device*)ctx->context, ib_port, gid_idx, &gid);
	if(ret){
		printk(KERN_ALERT "[%s] rdma_query_gid failed\n", __func__);
		return 1;
	} 
	//dprintk("[%s] sizeof(struct node_info) : %ld\n", __func__, sizeof(struct node_info));
	local.node_id = ctx->node_id;
	local.lid = ctx->port_attr.lid;
	local.qpn = ctx->qp->qp_num;
	local.psn = 0;
	local.mm = (uint64_t)ctx->local_dma_addr;
	local.rkey = ctx->mr->rkey;
	local.gid = gid;
	ret = tcp_send(sock, (char*)&local, sizeof(struct node_info));
	if(ret){
		printk(KERN_ALERT "[TCP] send failed\n");
		ksys_close(fd);
		return 1;
	}
	//dprintk("[TCP] sent local data to server\n");
	//dprintk("[TCP] sent data: node_id(%d), lid(%d), qpn(%d), psn(%d), mm(%llu), rkey(%u)\n", local.node_id, local.lid, local.qpn, local.psn, local.mm, local.rkey);

	ret = rdma_is_port_valid((struct ib_device*) ctx->context, ib_port);
	if(ret != 1){
		printk("rdma_is_port_valid returned error\n");
		if(ret == -EINVAL)
			printk("returned -EINVAL\n");
		else
			printk("returned unknown error\n");
	}

	ret = modify_qp(local.psn, 0, &remote);
	if(ret){
		printk(KERN_ALERT "[%s] ib_modify_qp failed\n", __func__);
		ksys_close(fd);
		return 1;
	}

	post_recv();

	if(fd)
		ksys_close(fd);
	return 0;

}

int create_qp(void){
	struct ib_device_attr dev_attr;
	struct ib_qp_init_attr qp_attr;
	struct ib_udata uhw = {.outlen = 0, .inlen = 0};

	memset(&dev_attr, 0, sizeof(struct ib_device_attr));
	if(((struct ib_device*)ctx->context)->ops.query_device((struct ib_device*)ctx->context, &dev_attr, &uhw)){
		printk(KERN_ALERT "[%s] ib_query_device failed\n", __func__);
		return 1;
	}
	//printk("[%s] ib_query_device succeeded \n\
	max_qp_wr(%d)\n\
		max_send_sge(%d)\n\
		max_recv_sge(%d)\n", __func__, dev_attr.max_qp_wr, dev_attr.max_send_sge, dev_attr.max_recv_sge);

	memset(&qp_attr, 0, sizeof(struct ib_qp_init_attr));
	//    ctx->depth = min(dev_attr.max_qp_wr, 1 << 13);
	ctx->depth = 64;
	qp_attr.cap.max_inline_data = 0;
	qp_attr.cap.max_send_wr = ctx->depth;;
	qp_attr.cap.max_recv_wr = ctx->depth;
	qp_attr.cap.max_send_sge = min(dev_attr.max_send_sge, 1 << 2);
	qp_attr.cap.max_recv_sge = min(dev_attr.max_recv_sge, 1 << 2);
	/*    qp_attr.cap.max_send_wr = ctx->depth;
		  qp_attr.cap.max_recv_wr = ctx->depth;
		  qp_attr.cap.max_send_sge = 2;
		  qp_attr.cap.max_recv_sge = 2;*/
	qp_attr.qp_type = IB_QPT_RC;
	qp_attr.send_cq = ctx->send_cq;
	qp_attr.recv_cq = ctx->recv_cq;

	ctx->qp = ib_create_qp(ctx->pd, &qp_attr);
	if(!ctx->qp)
		return 1;
	return 0;
}

static struct client_context* client_init_ctx(void){
	int ret, flags, i;
	struct ib_cq_init_attr attr;
	struct ib_device_attr dev_attr;
	struct ib_udata uhw = {.outlen = 0, .inlen = 0};

	ctx = (struct client_context*)kmalloc(sizeof(struct client_context), GFP_KERNEL);
	ctx->temp_log = (uint64_t**)kmalloc(sizeof(uint64_t*)*MAX_PROCESS, GFP_KERNEL);
	ctx->process_state = (atomic_t*)kmalloc(sizeof(atomic_t)*MAX_PROCESS, GFP_KERNEL);
	for(i=0; i<MAX_PROCESS; i++){
		ctx->temp_log[i] = (uint64_t*)kmalloc(sizeof(uint64_t)*NUM_ENTRY, GFP_KERNEL);
		atomic_set(&ctx->process_state[i], PROCESS_STATE_IDLE);
		//ctx->process_state[i] = PROCESS_STATE_IDLE;
	}

	atomic_set(&ctx->connected, 0);
	ctx->node_id = -1;
	//ctx->bitmap = 0;
	ctx->send_flags = IB_SEND_SIGNALED;
	ctx->depth = DEPTH;
	ctx->channel = NULL;
	ctx->context = (struct ib_context*)ib_dev;
	if(!ctx->context){
		printk(KERN_ALERT "[%s] failed to initialize ib_dev\n", __func__);
		return NULL;
	}

	flags = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_READ;
	//    ctx->pd = ib_alloc_pd((struct ib_device*)ctx->context, flags);
	ctx->pd = ib_alloc_pd((struct ib_device*)ctx->context, 0);
	if(!ctx->pd){
		printk(KERN_ALERT "[%s] ib_alloc_pd failed\n", __func__);
		return NULL;
	}
	//dprintk("[%s] ib_alloc_pd succeeded\n", __func__);

	ctx->mr = ctx->pd->device->ops.get_dma_mr(ctx->pd, flags);
	ctx->mr->device = ctx->pd->device;
	ctx->mr->pd = ctx->pd;
	ctx->mr->uobject = NULL;
	ctx->mr->need_inval = false;
	ctx->pd->__internal_mr = ctx->mr;
	//    ctx->mr = ctx->pd->__internal_mr;
	if(!ctx->mr){
		printk(KERN_ALERT "[%s] ib_allocate_mr failed\n", __func__);
		goto dealloc_pd;
	}
	//dprintk("[%s] ib_get_dma_mr succeeded\n", __func__);

	memset(&dev_attr, 0, sizeof(struct ib_device_attr));
	if(((struct ib_device*)ctx->context)->ops.query_device((struct ib_device*)ctx->context, &dev_attr, &uhw)){
		printk(KERN_ALERT "[%s] ib_query_device failed\n", __func__);
		goto dealloc_pd;
	}
	//dprintk("max_cqe: %d\n", dev_attr.max_cqe);
	memset(&attr, 0, sizeof(struct ib_cq_init_attr));
	attr.cqe = min(dev_attr.max_cqe, min(dev_attr.max_qp_wr, 1 << 13));
	//    attr.cqe = ctx->depth*4 + 1;
	attr.comp_vector = 0;
	//  ctx->recv_cq = ib_create_cq((struct ib_device*)ctx->context, poll_cq, NULL, ctx->channel, &attr);
	ctx->recv_cq = ib_create_cq((struct ib_device*)ctx->context, NULL, NULL, NULL, &attr);
	//    ctx->recv_cq = ib_create_cq((struct ib_device*)ctx->context, poll_cq, NULL, NULL, &attr);
	if(!ctx->recv_cq){
		printk(KERN_ALERT "[%s] ib_create_cq failed for recv_cq\n", __func__);
		goto dealloc_pd;
	}
	//dprintk("[%s] ib_create_cq succeeded for recv_cq\n", __func__);

	/*    ret = ib_req_notify_cq(ctx->recv_cq, 0);
		  if(ret){
		  printk(KERN_ALERT "[%s] ib_req_notify_cq failed %d\n", __func__, ret);
		  goto destroy_cq1;
		  }
		  dprintk("[%s] ib_req_notify_cq succeeded for recv_cq\n", __func__);
		  */
	memset(&attr, 0, sizeof(struct ib_cq_init_attr));
	attr.cqe = min(dev_attr.max_cqe, min(dev_attr.max_qp_wr, 1 << 13));
	//    attr.cqe = ctx->depth * 4;
	attr.comp_vector = 0;
	//    ctx->send_cq = ib_create_cq((struct ib_device*)ctx->context, poll_cq, NULL, ctx->channel, &attr);
	//    ctx->send_cq = ib_create_cq((struct ib_device*)ctx->context, poll_cq, NULL, NULL, &attr);
	ctx->send_cq = ib_create_cq((struct ib_device*)ctx->context, NULL, NULL, NULL, &attr);
	//    ctx->send_cq = ib_create_cq((struct ib_device*)ctx->context, poll_cq_test, NULL, NULL, &attr);

	if(!ctx->send_cq){
		printk(KERN_ALERT "[%s] ib_create_cq failed for send_cq\n", __func__);
		goto destroy_cq2;
	}
	//dprintk("[%s] ib_create_cq succeeded for send_cq\n", __func__);
	/*
	   ret = ib_req_notify_cq(ctx->send_cq, 0);
	   if(ret){
	   printk(KERN_ALERT "[%s] ib_req_notify_cq failed %d\n", __func__, ret);
	   goto destroy_cq1;
	   }
	   dprintk("[%s] ib_req_notify_cq succeeded for send_cq\n", __func__);
	   */
	ret = create_qp();
	if(ret){
		printk(KERN_ALERT "[%s] ib_create_qp failed\n", __func__);
		goto destroy_cq1;
	}
	//dprintk("[%s] ib_create_qp succeeded\n", __func__);

	return ctx;

destroy_qp:	ib_destroy_qp(ctx->qp);
destroy_cq1:	ib_destroy_cq(ctx->send_cq);
destroy_cq2:    ib_destroy_cq(ctx->recv_cq);
dealloc_pd:     ib_dealloc_pd(ctx->pd);
				return NULL;
}

int client_init_interface(void){
	int ret, x, i;

	x = rdma_port_get_link_layer(ib_dev, ib_port);
	ctx = client_init_ctx();
	if(!ctx){
		printk(KERN_ALERT "Failed to initialize client_init_ctx\n");
		return 1;
	}
	//dprintk("[%s] clinet_init_ctx succeeded\n", __func__);

	ret = ib_query_port(ib_dev, ib_port, &ctx->port_attr);
	if(ret < 0){
		printk(KERN_ALERT "Failed to query ib_port\n");
		return 1;
	}
	//dprintk("[%s] ib_query_port succeeded\n", __func__);

	ret = establish_conn();
	if(ret){
		printk(KERN_ALERT "Failed to establish connection\n");
		goto cleanup_resources;
	}
	dprintk("[%s] establish_conn succeeded\n", __func__);

	atomic_set(&ctx->connected, 1);

	spin_lock_init(&list_lock);
	INIT_LIST_HEAD(&(request_list.list));
	request_cache = kmem_cache_create("request_cache", sizeof(struct request_struct), 64, SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD, NULL);
	page_cache = kmem_cache_create("page_cache", PAGE_SIZE, PAGE_SIZE, SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD, NULL);

	thread_poll_cq = kthread_create((void*)&client_poll_cq, ctx->recv_cq, "cq_poller");
	if(IS_ERR(thread_poll_cq)){
		printk(KERN_ALERT "cq_poller thread creation failed\n");
		return 1;
	}
	wake_up_process(thread_poll_cq);

	thread_handler = kthread_create((void*)&event_handler, NULL, "event_handler");
	if(IS_ERR(thread_handler)){
		printk(KERN_ALERT "event_handler thread creation failed\n");
		return 1;
	}
	wake_up_process(thread_handler);

	return 0;

cleanup_resources:
	if(ctx->qp) ib_destroy_qp(ctx->qp);
	if(ctx->send_cq) ib_destroy_cq(ctx->send_cq);
	if(ctx->recv_cq) ib_destroy_cq(ctx->recv_cq);
	if(ctx->pd) ib_dealloc_pd(ctx->pd);
	return 1;
}

void cleanup_resource(void){
	if(ctx->qp)
		ib_destroy_qp(ctx->qp);
	if(ctx->send_cq)
		ib_destroy_cq(ctx->send_cq);
	if(ctx->recv_cq)
		ib_destroy_cq(ctx->recv_cq);
	if(ctx->pd)
		ib_dealloc_pd(ctx->pd);
	atomic_set(&ctx->connected, 0);
}

int test_func3(void){
	uintptr_t addr = ib_reg_mr_addr((void*)ctx->local_mm, 64);
	char* test[64];
	int i, ret;

	for(i=0; i<64; i++){
		test[i] = (char*)(ctx->local_mm+i);
	}

	ret = rdma_recv((void*)addr, 64);
	if(ret){
		printk(KERN_ALERT "[%s] posting rdma_recv failed\n", __func__);
		return 1;
	}
	//dprintk("[%s] posting rdma_recv succeeded\n", __func__);
	//ret = poll_cq_test(ctx->recv_cq, 1);
	if(ret != 1){
		printk(KERN_ALERT "[%s] timeout, clock expired, send_poll_cq failed\n", __func__);
		return 1;
	}
	dprintk("[%s] recv_poll_cq succeeded\n", __func__);

	printk("[%s]: testmsg: %s\n", *test);

	return 0;
}




int test_func2(void){
	//    uintptr_t addr = ib_reg_mr_addr((void*)ctx->local_mm, 64);
	char msg[64] = "this is apache3";
	char* test[64];
	int i, ret;
	uintptr_t addr = ctx->local_dma_addr;

	for(i=0; i<64; i++){
		test[i] = (char*)(ctx->local_mm+i);
	}

	strcpy(*test, msg);
	printk("[%s] test reading: %s\n", __func__, *test);

	ret = rdma_send((void*)addr, 64);
	//    ret = rdma_send((void*)test[0], 64);
	if(ret){
		printk(KERN_ALERT "[%s] posting rdma_send failed\n", __func__);
		return 1;
	}
	dprintk("[%s] posting rdma_send succeeded\n", __func__);
	poll_cq_test(ctx->send_cq, 1);
	if(ret != 1){
		printk(KERN_ALERT "[%s] send_poll_cq failed\n", __func__);
		return 1;
	}
	dprintk("[%s] send_poll_cq succeeded\n", __func__);

	return 0;
}


int test_func(void){
	int ret, i;
	uintptr_t addr = test_ptr;
	//    uintptr_t addr = ib_reg_mr_addr((void*)ctx->local_mm, 4096);
	//    char* ptr = (char*)addr;
	//    char* ptr = (char*)ctx->local_mm;
	char msg[64] = "this is apache3\0";
	char* test[64];

	//    strcpy(ptr, msg);
	//    printk("[%s] test reading: %s\n", __func__, *ptr);

	for(i=0; i<64; i++){
		test[i] = (char*)(ctx->local_mm + i);
		//	test[i] = (char*)(addr + i);
	}
	strcpy(*test, msg);
	printk("[%s] test reading: %s\n", __func__, *test);
	printk("local_mm(%llu), reg_mr(%llu)\n", ctx->local_mm, (uint64_t)addr);
	//    ret = rdma_write((uint64_t)ctx->local_mm, 0, sizeof(char)*64);
	//    ret = rdma_write((void*)test[0], 0, strlen(msg));
	//    ret = rdma_write((void*)addr, 0, strlen(msg));
	dprintk("[%s] rdma_write succeeded\n", __func__);
	return 0;
}

static int __init init_net_module(void){
	int ret = 0;
	pr_info("*****************************************");
	pr_info(" PMDFC network module is being installed ");
	pr_info("*****************************************\n");

	ret = class_register(&client_class);
	if(ret){
		pr_err("class_register failed\n");
		return -1;
	}

	ret = ib_register_client(&client);
	if(ret){
		pr_err("ib_register_client failed\n");
		class_unregister(&client_class);
		return -1;
	}

	ret = client_init_interface();
	if(ret){
		pr_err("client_init_interface failed\n");
		ib_unregister_client(&client);
		class_unregister(&client_class);
		return -1;
	}

	ret = query_qp(ctx->qp);

	pr_info("*****************************************");
	pr_info(" PMDFC network module has benn installed ");
	pr_info("*****************************************\n");

	//    ret = single_write_test();
	//    ret = test_func2();
	/*
	   ssleep(1);
	   ib_unregister_client(&client);
	   class_unregister(&client_class);*/
	return 1;
}

static void __exit exit_net_module(void){

	pr_info("****************************************");
	pr_info(" PMDFC network module is being removed ");
	pr_info("****************************************\n");

	if(thread_poll_cq){
		kthread_stop(thread_poll_cq);
		thread_poll_cq = NULL;
		printk(KERN_INFO "Stopped thread_poll_cq\n");
	}

	if(thread_handler){
		kthread_stop(thread_handler);
		thread_handler = NULL;
		printk(KERN_INFO "Stopped thread_handler\n");
	}
	cleanup_resource();

	if(page_cache) kmem_cache_destroy(page_cache);
	if(request_cache) kmem_cache_destroy(request_cache);

	atomic_set(&ctx->connected, 0);

	ib_unregister_client(&client);
	class_unregister(&client_class);
	pr_info("***************************************");
	pr_info(" PMDFC network module has been removed ");
	pr_info("***************************************\n");
}

module_init(init_net_module);
module_exit(exit_net_module);

MODULE_AUTHOR("Hokeun");
MODULE_LICENSE("GPL");
