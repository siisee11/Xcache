#include "rdpma.h"
#include "ib.h"
#include <rdma/ib_cache.h>

static const int RDMA_BUFFER_SIZE = 4096;
static const int RDMA_PAGE_SIZE = 4096;
struct task_struct* thread_recv_poll_cq;
struct task_struct* thread_send_poll_cq;
struct task_struct* thread_handler;
struct client_context* ctx = NULL;
int ib_port = 1;
enum ib_mtu mtu;

static int enable;

struct kmem_cache* request_cache;

/* struct workqueue */
static struct workqueue_struct *rdpma_wq;

/* RDPMA nodes */
struct rdpma_node rdpma_nodes[RDPMA_MAX_NODES];

static int rdpma_sys_err_translations[RDPMA_ERR_MAX] =
		{[RDPMA_ERR_NONE]	= 0,
		 [RDPMA_ERR_NO_HNDLR]	= -ENOPROTOOPT,
		 [RDPMA_ERR_OVERFLOW]	= -EOVERFLOW,
		 [RDPMA_ERR_DIED]	= -EHOSTDOWN,};


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

int rdpma_post_recv(void);

static inline int rdpma_sys_err_to_errno(enum rdpma_system_error err)
{
	int trans;
	BUG_ON(err >= RDPMA_ERR_MAX);
	trans = rdpma_sys_err_translations[err];

	/* Just in case we mess up the translation table above */
	BUG_ON(err != RDPMA_ERR_NONE && trans == 0);
	return trans;
}

/* get rdpma_node by number */
struct rdpma_node * rdpma_nn_from_num(u8 node_num)
{
	BUG_ON(node_num >= ARRAY_SIZE(rdpma_nodes));
	return &rdpma_nodes[node_num];
}

static u8 rdpma_num_from_nn(struct rdpma_node *nn)
{
	BUG_ON(nn == NULL);
	return nn - rdpma_nodes;
}

/* ------------------------------------------------------------ */

static int rdpma_prep_nsw(struct rdpma_node *nn, struct rdpma_status_wait *nsw)
{
	int ret;

	spin_lock(&nn->nn_lock);
	ret = idr_alloc(&nn->nn_status_idr, nsw, 0, 0, GFP_ATOMIC);
	if (ret >= 0) {
		nsw->ns_id = ret;
		list_add_tail(&nsw->ns_node_item, &nn->nn_status_list);
	}
	spin_unlock(&nn->nn_lock);
	if (ret < 0)
		return ret;

	init_waitqueue_head(&nsw->ns_wq);
	nsw->ns_sys_status = RDPMA_ERR_NONE;
	nsw->ns_status = 0;
	return 0;
}

static void rdpma_complete_nsw_locked(struct rdpma_node *nn,
				      struct rdpma_status_wait *nsw,
				      enum rdpma_system_error sys_status,
				      s32 status)
{
	assert_spin_locked(&nn->nn_lock);

	if (!list_empty(&nsw->ns_node_item)) {
		list_del_init(&nsw->ns_node_item);
		nsw->ns_sys_status = sys_status;
		nsw->ns_status = status;
//		idr_remove(&nn->nn_status_idr, nsw->ns_id);
		wake_up(&nsw->ns_wq);
	}
}

void rdpma_complete_nsw(struct rdpma_node *nn,
			       struct rdpma_status_wait *nsw,
			       u64 id, enum rdpma_system_error sys_status,
			       s32 status)
{
	spin_lock(&nn->nn_lock);
	if (nsw == NULL) {
		if (id > INT_MAX)
			goto out;

		nsw = idr_find(&nn->nn_status_idr, id);
		if (nsw == NULL)
			goto out;
	}

	rdpma_complete_nsw_locked(nn, nsw, sys_status, status);
	spin_unlock(&nn->nn_lock);
	return;

out:
	pr_err("%s: idr_find cannot find nsw (%llx)\n", __func__, id);
	spin_unlock(&nn->nn_lock);
	return;
}

static void rdpma_complete_nodes_nsw(struct rdpma_node *nn)
{
	struct rdpma_status_wait *nsw, *tmp;
	unsigned int num_kills = 0;

	assert_spin_locked(&nn->nn_lock);

	list_for_each_entry_safe(nsw, tmp, &nn->nn_status_list, ns_node_item) {
		rdpma_complete_nsw_locked(nn, nsw, RDPMA_ERR_DIED, 0);
		num_kills++;
	}
}

static int rdpma_nsw_completed(struct rdpma_node *nn,
			       struct rdpma_status_wait *nsw)
{
	int completed;
	spin_lock(&nn->nn_lock);
	completed = list_empty(&nsw->ns_node_item);
	spin_unlock(&nn->nn_lock);
	return completed;
}

/* ------------------------------------------------------------ */

/**
 * ib_reg_mr_addr - map DMA mapping to addr and validate it.
 *
 * @addr: base address of target area.
 * @size: size from base address.
 *
 * If succeeds, return DMA-able address.
 */
uint64_t ib_reg_mr_addr(void* addr, uint64_t size){
	uint64_t ret;	
	ret = ib_dma_map_single(ctx->dev, addr, size, DMA_BIDIRECTIONAL);
	if (unlikely(ib_dma_mapping_error(ctx->dev, ret))) {
		ib_dma_unmap_single(ctx->dev,
				ret, size, DMA_BIDIRECTIONAL);
		return -ENOMEM;
	}
	return ret;
}

void ib_dereg_mr_addr(uint64_t addr, uint64_t size){
	ib_dma_unmap_single(ctx->dev, addr, size, DMA_BIDIRECTIONAL);
}

uint32_t bit_mask(int node_id, int msg_num, int type, int state, uint32_t num){
	uint32_t target = (((uint32_t)node_id << 28) | ((uint32_t)msg_num << 16) | ((uint32_t)type << 12) | ((uint32_t)state << 8) | ((uint32_t)num & 0x000000ff));
	return target;
}

void bit_unmask(uint32_t target, int* node_id, int* msg_num, int* type, int* state, uint32_t* num){
	*num = (uint32_t)(target & 0x000000ff);
	*state = (int)((target >> 8) & 0x0000000f);
	*type = (int)((target >> 12) & 0x0000000f);
	*msg_num = (int)((target >> 16) & 0x00000fff);
	*node_id = (int)((target >> 28) & 0x0000000f);
}

/**
 * rdpma_write_message- post write request and write message.
 * @msg_type: Type of message.
 * @key: Unique key.
 * @index: index from mapping.
 * @bit: bit of ps_storage.
 * @data: data address.
 * @len: data length.
 * @target_node: target node number (server is 0).
 * @status: status return from communication.
 *
 * If generate_single_write_request succeeds, then return 0.
 */
int rdpma_write_message(u32 msg_type, u32 key, u32 index, u32 bit, void *data, 
			u32 len, u8 target_node, int *status){
	uint64_t* addr;
	int ret;
	long longkey = (u64)key << 32 | index;
	void* dma_addr;
	uintptr_t dma_page_addr[REQUEST_MAX_BATCH];
	void* pages[REQUEST_MAX_BATCH];
	int i;
	long offset;
	uint64_t* remote_mm;
	int num = 1; 	/* batching size */
	struct rdpma_node *nn = rdpma_nn_from_num(target_node);
	struct rdpma_status_wait nsw = {
		.ns_node_item = LIST_HEAD_INIT(nsw.ns_node_item),
	};

	ret = rdpma_prep_nsw(nn, &nsw);

	if (nsw.ns_id >= MAX_PROCESS)
		return -1;

	dma_addr = (void*)GET_LOCAL_META_REGION(ctx->local_dma_addr, nsw.ns_id);
	offset = nsw.ns_id * METADATA_SIZE * NUM_ENTRY;
	addr = (uint64_t*)GET_LOCAL_META_REGION(ctx->local_mm, nsw.ns_id);
	*(addr) = longkey;

	/* TODO batching or buffering */
	ret = post_meta_request_batch(nsw.ns_id, msg_type, num, TX_WRITE_BEGIN, sizeof(uint64_t), dma_addr, offset, num);

	wait_event(nsw.ns_wq, rdpma_nsw_completed(nn, &nsw));
	ret = rdpma_sys_err_to_errno(nsw.ns_sys_status);
	if (status && !ret)
		*status = nsw.ns_status;

	remote_mm = (uint64_t*)(GET_LOCAL_META_REGION(ctx->local_mm, nsw.ns_id) + sizeof(uint64_t));

	pages[0] = data;

	/* write page content to remote_mm */
	for(i = 0; i < num; i++){
		dma_page_addr[i] = ib_reg_mr_addr(pages[i], PAGE_SIZE);
	}

	/* add nsw to nn_status_list again */
	list_add_tail(&nsw.ns_node_item, &nn->nn_status_list);

	post_write_request_batch(nsw.ns_id, MSG_WRITE, num, dma_page_addr, *remote_mm, num);

	wait_event(nsw.ns_wq, rdpma_nsw_completed(nn, &nsw));
	ret = rdpma_sys_err_to_errno(nsw.ns_sys_status);
	if (status && !ret)
		*status = nsw.ns_status;

	for(i = 0; i < num; i++){
		ib_dereg_mr_addr(dma_page_addr[i], PAGE_SIZE);
	}

	/* free nsw.ns_id, after this line msg_num can be reused */
	rdpma_complete_nsw(nn, &nsw, 0, 0, 0);
	idr_remove(&nn->nn_status_idr, nsw.ns_id);


	return ret;
}
EXPORT_SYMBOL(rdpma_write_message);


/**
 * rdpma_read_message - post read request and read from remote memory.
 * @msg_type: Type of message.
 * @key: Unique key.
 * @index: index from mapping.
 * @data: data address.
 * @len: data length.
 * @target_node: target node number (server is 0).
 * @status: status return from communication.
 *
 * If get succeeds, then return 0.
 * else return -1.
 */
int rdpma_read_message(u32 msg_type, u32 key, u32 index, void *data, 
			u32 len, u8 target_node, int *status){
	int ret;
	long longkey = (u64)key << 32 | index;
	uint64_t* addr;
	void* dma_addr;
	uintptr_t dma_page_addr[REQUEST_MAX_BATCH];
	void* pages[REQUEST_MAX_BATCH];
	int i;
	long offset;
	uint64_t* remote_mm;
	int page_exist = 0;
	int num = 1; 	/* batching size */
	struct rdpma_node *nn = rdpma_nn_from_num(target_node);
	struct rdpma_status_wait nsw = {
		.ns_node_item = LIST_HEAD_INIT(nsw.ns_node_item),
	};

	ret = rdpma_prep_nsw(nn, &nsw);

	if (nsw.ns_id >= MAX_PROCESS)
		return -1;

	dma_addr = (void*)GET_LOCAL_META_REGION(ctx->local_dma_addr, nsw.ns_id);
	offset = nsw.ns_id * METADATA_SIZE * NUM_ENTRY;
	addr = (uint64_t*)GET_LOCAL_META_REGION(ctx->local_mm, nsw.ns_id);
	*(addr) = longkey;
//	pr_info("MSG_READ_REQUEST key=%lx, msg_num=%x\n", *addr, nsw.ns_id);

	post_meta_request_batch(nsw.ns_id, MSG_READ_REQUEST, num, TX_READ_BEGIN, sizeof(uint64_t), dma_addr, offset, num);

	wait_event(nsw.ns_wq, rdpma_nsw_completed(nn, &nsw));
	ret = rdpma_sys_err_to_errno(nsw.ns_sys_status);
	if (status && !ret)
		*status = nsw.ns_status;

//	pr_info("woken, returning user status %d, msg_num %x, longkey %llx \n", \
			nsw.ns_status, nsw.ns_id, longkey);

	/* PAGE_NOT_EXIST */
	if (*status == -1) {
		ret = -1;
		goto out;
	}

	remote_mm = (uint64_t*)GET_REMOTE_ADDRESS_BASE(ctx->local_mm, nsw.ns_id);
	pages[0] = data;

	/* write page content to remote_mm */
	for(i = 0; i < num; i++){
		dma_page_addr[i] = ib_reg_mr_addr(pages[i], len);
	}

	post_read_request_batch(dma_page_addr, *remote_mm, num);

	/* ACK to server */
//	post_meta_request_batch(nsw.ns_id, MSG_READ_REPLY, num, TX_READ_COMMITTED, 0, NULL, offset, num);

	for(i = 0; i < num; i++){
		ib_dereg_mr_addr(dma_page_addr[i], len);
	}

out:
	/* free nsw.ns_id, after this line msg_num can be reused */
	rdpma_complete_nsw(nn, &nsw, 0, 0, 0);
	idr_remove(&nn->nn_status_idr, nsw.ns_id);

	return ret;
}
EXPORT_SYMBOL(rdpma_read_message);

/**
 * generate_single_write_request - Add new write request to request list.
 * @page: The address of page content.
 * @key: Unique key.
 *
 * This function write key to local metadata region and 
 * copy page content to newly allocated request_page and write its
 * address to ctx->temp_log.
 * Finally, add new_request to list to be handled by event_handler.
 *
 * If generate_single_write_request succeeds, then return 0.
 */
int generate_single_write_request(void* page, uint64_t key){
	struct request_struct* new_request = kmem_cache_alloc(request_cache, GFP_KERNEL);
	void* request_page;
	uint64_t* addr;
	int pid = find_and_set_nextbit();
	unsigned long flags;

	addr = (uint64_t*)GET_LOCAL_META_REGION(ctx->local_mm, pid);
	new_request->type = MSG_WRITE_REQUEST;
	new_request->msg_num = pid;
	new_request->num = 1;

	/* add key to metadata region */
	*(addr) = key;

	/* request_page pointer saved in temp_log */
	request_page = kmalloc(PAGE_SIZE, GFP_ATOMIC);
	if (!request_page) {
		pr_err("[%s]: cannot kmalloc request_pages\n", __func__);
		BUG_ON(request_page == NULL);
	}

	memcpy(request_page, page, PAGE_SIZE);
	ctx->temp_log[pid][0] = (uint64_t)request_page;

	spin_lock_irqsave(&ctx->lock, flags);
	list_add_tail(&new_request->entry, &ctx->req_list);
	wake_up(&ctx->req_wq);
	spin_unlock_irqrestore(&ctx->lock, flags);

//	dprintk("[%s]: added write request (key=%llx, pid=%x, num=%x)\n", __func__, *addr, pid, 1);
	return 0;
}
EXPORT_SYMBOL(generate_single_write_request);

int generate_write_request(void** pages, uint64_t* keys, int num){
	struct request_struct* new_request = kmem_cache_alloc(request_cache, GFP_KERNEL);
	void* request_pages[REQUEST_MAX_BATCH];
	uint64_t* addr;
	int pid = find_and_set_nextbit();
	int i;

	addr = (uint64_t*)GET_LOCAL_META_REGION(ctx->local_mm, pid);
	new_request->type = MSG_WRITE_REQUEST;
	new_request->msg_num = pid;
	new_request->num = num;
	for(i = 0 ; i < num ; i++){
		*(addr + i * METADATA_SIZE) = keys[i];
		dprintk("[%s]: generate write request with keys[%d]=%llx\n", __func__, i, keys[i]);
		request_pages[i] = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (!request_pages[i]) {
			pr_err("[%s]: cannot kmalloc request_pages\n", __func__);
			BUG_ON(request_pages[i] == NULL);
		}
		BUG_ON(pages[i] == NULL);

		memcpy(request_pages[i], pages[i], PAGE_SIZE);
		ctx->temp_log[pid][i] = (uint64_t)request_pages[i];
	}

	spin_lock(&ctx->lock);
	list_add_tail(&new_request->entry, &ctx->req_list);
	wake_up(&ctx->req_wq);
	spin_unlock(&ctx->lock);

	dprintk("[%s]: added write request (key=%llx, pid=%x, num=%x\n", __func__, *addr, pid, num);

	return 0;
}
EXPORT_SYMBOL(generate_write_request);

int generate_single_read_request(void* page, uint64_t key){
	struct request_struct* new_request = kmem_cache_alloc(request_cache, GFP_KERNEL);
//	void* request_page;
	uint64_t* addr;
	int pid = find_and_set_nextbit();
	volatile int* process_state = &ctx->process_state[pid];

	BUG_ON(page == NULL);

	addr = (uint64_t*)GET_LOCAL_META_REGION(ctx->local_mm, pid);
	new_request->type = MSG_READ_REQUEST;
	new_request->msg_num = pid;
	new_request->num = 1;

	*(addr) = key;
#if 0
//	dprintk("[%s]: generate read request with key=%llx\n", __func__, key);
	request_page = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!request_page) {
		pr_err("[%s]: cannot kmalloc request_pages\n", __func__);
		BUG_ON(request_page == NULL);
	}
#endif

	ctx->temp_log[pid][0] = (uint64_t)page;

	spin_lock(&ctx->lock);
	list_add_tail(&new_request->entry, &ctx->req_list);
	wake_up(&ctx->req_wq);
	spin_unlock(&ctx->lock);

	while(*process_state != PROCESS_STATE_WAIT){
		cpu_relax();
	}

	if(*process_state == PROCESS_STATE_ABORT){
//		kfree(request_page);
		page = NULL;
		unset_bit(pid);
		return 1;
	}

//	memcpy(page, request_page, PAGE_SIZE);

	*process_state = PROCESS_STATE_DONE;

//	dprintk("[%s]: completed read request with key %llu\n\n", __func__, key);
	return 0;
}
EXPORT_SYMBOL(generate_single_read_request);


int generate_read_request(void** pages, uint64_t* keys, int num){
	struct request_struct* new_request = kmem_cache_alloc(request_cache, GFP_KERNEL);
	int pid = find_and_set_nextbit();
	void* request_pages[REQUEST_MAX_BATCH];
	uint64_t* addr;
	volatile int* process_state = &ctx->process_state[pid];
	int i;

	addr = (uint64_t*)GET_LOCAL_META_REGION(ctx->local_mm, pid);
	new_request->type = MSG_READ_REQUEST;
	new_request->msg_num = pid;
	new_request->num = num;
	for(i = 0; i < num; i++){
		*(addr + i*METADATA_SIZE) = keys[i];
		request_pages[i] = kmalloc(PAGE_SIZE, GFP_KERNEL);
		ctx->temp_log[pid][i] = (uint64_t)request_pages[i];
		//new_request->keys[i] = keys[i];
	}

	spin_lock(&ctx->lock);
	list_add_tail(&new_request->entry, &ctx->req_list);
	wake_up(&ctx->req_wq);
	spin_unlock(&ctx->lock);
	//dprintk("[%s]: added read request with key %llu into request_list\n", __func__, *addr);

	while(*process_state != PROCESS_STATE_WAIT){
		cpu_relax();
	}

	if(*process_state == PROCESS_STATE_ABORT){
		for(i=0; i<num; i++){
			kfree(request_pages[i]);
			pages[i] = NULL;
		}
		unset_bit(pid);
		return 1;
	}

	for(i=0; i<num; i++){
		memcpy(pages[i], (void*)ctx->temp_log[pid][i], PAGE_SIZE);
		//pages[i] = (void*)ctx->temp_log[pid][i];
	}
	*process_state = PROCESS_STATE_DONE;

	return 0;
}
EXPORT_SYMBOL(generate_read_request);

/**
 * find_and_set_nextbit - Find first zero bit and set PROCESS_STATE_ACTIVE.
 * 
 * return first zero bit.
 */
int find_and_set_nextbit(void){
	int bit = 1;
	bit = find_first_zero_bit(ctx->bitmap, MAX_PROCESS);
	if(test_and_set_bit(bit, ctx->bitmap) == 0){
		ctx->process_state[bit] = PROCESS_STATE_ACTIVE;
		//atomic_set(&ctx->process_state[bit], PROCESS_STATE_ACTIVE);
		return bit;
	}
	printk(KERN_ALERT "[%s]: bitmap find err\n", __func__);
	return -1;
}

void unset_bit(int idx){
	ctx->process_state[idx] = PROCESS_STATE_IDLE;
	if(test_and_clear_bit(idx, ctx->bitmap) != 1){
		printk(KERN_ALERT "[%s]: bitmap setting has gone something wrong!!\n", __func__);
	}
}

void handle_write_request(int pid, int num){
	void* dma_addr = (void*)GET_LOCAL_META_REGION(ctx->local_dma_addr, pid);
	long offset = pid * METADATA_SIZE * NUM_ENTRY;

	post_meta_request_batch(pid, MSG_WRITE_REQUEST, num, TX_WRITE_BEGIN, sizeof(uint64_t), dma_addr, offset, num);
}

void handle_write(int pid, int num){
	uint64_t* remote_mm = (uint64_t*)(GET_LOCAL_META_REGION(ctx->local_mm, pid) + sizeof(uint64_t));
	uintptr_t addr[REQUEST_MAX_BATCH];
	void* pages[REQUEST_MAX_BATCH];
	int i;

	/* write page content to remote_mm */
	for(i = 0; i < num; i++){
		pages[i] = (void*)ctx->temp_log[pid][i];
		/* TODO: avoid register mr on critical path */
		addr[i] = ib_reg_mr_addr(pages[i], PAGE_SIZE);
	}
//	dprintk("[%s]: target addr= %llx\n", __func__, *remote_mm);

	post_write_request_batch(pid, MSG_WRITE, num, addr, *remote_mm, num);

	for(i = 0; i < num; i++){
		ib_dereg_mr_addr(addr[i], PAGE_SIZE);
		kfree(pages[i]);
	}
}

void handle_read_request(int pid, int num){
	void* dma_addr = (void*)GET_LOCAL_META_REGION(ctx->local_dma_addr, pid);
	uint64_t offset = pid * METADATA_SIZE * NUM_ENTRY;
	post_meta_request_batch(pid, MSG_READ_REQUEST, num, TX_READ_BEGIN, sizeof(uint64_t), dma_addr, offset, num);
}

void handle_read(int pid, int num){
	uint64_t* remote_mm = (uint64_t*)GET_REMOTE_ADDRESS_BASE(ctx->local_mm, pid);
	uintptr_t addr[REQUEST_MAX_BATCH];
	void* pages[REQUEST_MAX_BATCH];
	uint64_t offset = pid * METADATA_SIZE * NUM_ENTRY;
	volatile int* process_state = &ctx->process_state[pid];
	int i;

//	dprintk("[%s]: target addr= %llx\n", __func__, *remote_mm);

	for(i = 0; i < num; i++){
		pages[i] = (void*)ctx->temp_log[pid][i];
		addr[i] = ib_reg_mr_addr(pages[i], PAGE_SIZE);
	}
	post_read_request_batch(addr, *remote_mm, num);

//	dprintk("[%s]: returned page %s\n", __func__, (char *)pages[0]);

	post_meta_request_batch(pid, MSG_READ_REPLY, num, TX_READ_COMMITTED, 0, NULL, offset, num);

	*process_state = PROCESS_STATE_WAIT;
	while(*process_state != PROCESS_STATE_DONE){
		cpu_relax();
	}

	/* TODO: returning read pages to requested process */

	/*
	for(i=0; i<num; i++){
		ib_dereg_mr_addr(addr[i], PAGE_SIZE);
		kfree(pages[i]);
	}
	*/

	unset_bit(pid);
}

int event_handler(void){
	struct request_struct* new_request;
	allow_signal(SIGKILL);
	while(1){
		wait_event(ctx->req_wq, !list_empty(&ctx->req_list));
		spin_lock(&ctx->lock);

		if (list_empty(&ctx->req_list)){
			spin_unlock(&ctx->lock);
			continue;
		}
		new_request = list_first_entry(&ctx->req_list, struct request_struct, entry);
		BUG_ON(new_request == NULL);

		list_del_init(&new_request->entry);
		spin_unlock(&ctx->lock);

		dprintk("[%s]: handle new request(type=%s, msg_num=%d, num=%lld)\n", 
				__func__, TYPE_STR(new_request->type),
				new_request->msg_num, new_request->num);

		switch(new_request->type) {
			case MSG_WRITE_REQUEST: 
				handle_write_request(new_request->msg_num, new_request->num);
				break;
			
			case MSG_WRITE: 
				handle_write(new_request->msg_num, new_request->num);
				break;

			case MSG_READ_REQUEST:
				handle_read_request(new_request->msg_num, new_request->num);
				break;

			case MSG_READ:
				handle_read(new_request->msg_num, new_request->num);
				break;

			default:
				printk(KERN_ALERT "[%s]: weired request type (%d)\n", __func__, new_request->type);

		}
		kmem_cache_free(request_cache, new_request);
	}
	return 0;
}

/**
 * post_meta_request_batch - post metadata request to target
 * @pid: Progress identifier.
 * @type: Message type (i.e. MSG_READ_REQUEST, MSG_WRITE_REQUEST, ...)
 * @num: Same with batch_size
 * @tx_state: Transaction state (i.e. TX_READ_BEGIN, TX_READ_COMMITTED, ...) 
 * @dma_addr: DMA-able address.
 * @offset: offset to metadata region for pid
 * @batch_size: 
 *
 * This function post send in batch manner.
 * Note that only last work request to be signaled.
 *
 * If generate_single_write_request succeeds, then return 0
 * if not return negative value.
 */
int post_meta_request_batch(int msg_num, int type, int num, int tx_state, int len, 
		void* dma_addr, uint64_t offset, int batch_size){
	struct ib_rdma_wr wr[REQUEST_MAX_BATCH];
	const struct ib_send_wr* bad_wr;
	struct ib_sge sge[REQUEST_MAX_BATCH];
	struct ib_wc wc;
	int ret, ne, i;

	memset(sge, 0, sizeof(struct ib_sge) * REQUEST_MAX_BATCH);
	memset(wr, 0, sizeof(struct ib_rdma_wr) * REQUEST_MAX_BATCH);

	for(i = 0; i < batch_size; i++){
		sge[i].addr = (u64)(dma_addr + i*len);
		sge[i].length = len;
		sge[i].lkey = ctx->mr->lkey;

		/* TODO: wr_id and msg_num */
		//wr[i].wr.wr_id = bit_mask(ctx->node_id, pid, type, size);
		wr[i].wr.opcode = IB_WR_RDMA_WRITE_WITH_IMM;
		wr[i].wr.sg_list = &sge[i];
		wr[i].wr.num_sge = 1;
		wr[i].wr.next = (i == batch_size-1) ? NULL : (struct ib_send_wr*)&wr[i+1];
		wr[i].wr.send_flags = (i == batch_size-1) ? IB_SEND_SIGNALED : 0;
		wr[i].wr.ex.imm_data = htonl(bit_mask(ctx->node_id, msg_num, type, tx_state, num));
		wr[i].remote_addr = (uintptr_t)(ctx->remote_mm + offset + i*len);
		wr[i].rkey = ctx->rkey;

		dprintk("[%s]: target addr: %llx, target rkey %x\n", __func__, wr[i].remote_addr, wr[i].rkey);
	}

	ret = ib_post_send(ctx->qp, &wr[0].wr, &bad_wr);
	if(ret){
		printk(KERN_ALERT "[%s]: ib_post_send failed\n", __func__);
		return -1;
	}

	do{
		ne = ib_poll_cq(ctx->ic->i_send_cq, 1, &wc);
		if(ne < 0){
			printk(KERN_ALERT "[%s]: ib_poll_cq failed\n", __func__);
			return -1;
		}
	}while(ne < 1);

	if(wc.status != IB_WC_SUCCESS){
		printk(KERN_ALERT "[%s]: sending request failed status %s(%d) for wr_id %d\n", __func__, ib_wc_status_msg(wc.status), wc.status, (int)wc.wr_id);
		return -1;
	}

	return 0;
}

/**
 * post_read_request_batch - post read request to target
 * @addr: DMA-able address.
 * @offset: Base address of remote memory address. 
 * @batch_size: 
 *
 * This function post read request in batched manner.
 * Note that only last work request to be signaled.
 *
 * If generate_single_write_request succeeds, then return 0
 * if not return negative value.
 */

int post_read_request_batch(uintptr_t* addr, uint64_t offset, int batch_size){
	struct ib_rdma_wr wr[REQUEST_MAX_BATCH];
	const struct ib_send_wr* bad_wr;
	struct ib_sge sge[REQUEST_MAX_BATCH];
	struct ib_wc wc;
	int ret, ne, i;

	memset(sge, 0, sizeof(struct ib_sge)*batch_size);
	memset(wr, 0, sizeof(struct ib_rdma_wr)*batch_size);

	for(i = 0; i < batch_size; i++){
		sge[i].addr = addr[i];
		sge[i].length = PAGE_SIZE;
		sge[i].lkey = ctx->mr->lkey;

		//wr[i].wr.wr_id = bit_mask(ctx->node_id, pid, type, size);
		wr[i].wr.opcode = IB_WR_RDMA_READ;
		wr[i].wr.sg_list = &sge[i];
		wr[i].wr.num_sge = 1;
		wr[i].wr.next = (i == batch_size-1) ? NULL : (struct ib_send_wr*)&wr[i+1];
		wr[i].wr.send_flags = (i == batch_size-1) ? IB_SEND_SIGNALED : 0;
		wr[i].remote_addr = (uintptr_t)(offset + i * PAGE_SIZE);
		wr[i].rkey = ctx->rkey;
//		dprintk("[%s]: target addr: %llx\n", __func__, wr[i].remote_addr);
//		pr_info("[%s]: target addr: %llx\n", __func__, wr[i].remote_addr);
	}

	ret = ib_post_send(ctx->qp, &wr[0].wr, &bad_wr);
	if(ret){
		printk(KERN_ALERT "[%s]: ib_post_send failed\n", __func__);
		return 1;
	}

	do{
		ne = ib_poll_cq(ctx->ic->i_send_cq, 1, &wc);
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

/**
 * post_write_request_batch - post write request to target
 * @pid: Progress identifier.
 * @type: Message type (i.e. MSG_READ_REQUEST, MSG_WRITE_REQUEST, ...)
 * @num: Same with batch_size
 * @dma_addr: DMA-able local address.
 * @remote_mm: Remote memory address.
 * @batch_size: 
 *
 * This function post write request in batched manner.
 * Note that only last work request to be signaled.
 *
 * If generate_single_write_request succeeds, then return 0
 * if not return negative value.
 */
int post_write_request_batch(int pid, int type, int num, 
		uintptr_t* dma_addr, uint64_t remote_mm, int batch_size){
	struct ib_rdma_wr wr[REQUEST_MAX_BATCH];
	const struct ib_send_wr* bad_wr;
	struct ib_sge sge[REQUEST_MAX_BATCH];
	struct ib_wc wc;
	int ret, ne, i;

	memset(sge, 0, sizeof(struct ib_sge) * batch_size);
	memset(wr, 0, sizeof(struct ib_rdma_wr) * batch_size);

	for(i = 0; i < batch_size; i++) {
		sge[i].addr = dma_addr[i];
		sge[i].length = PAGE_SIZE;
		sge[i].lkey = ctx->mr->lkey;

		//	wr[i].wr.wr_id = bit_mask(ctx->node_id, pid, type, size);
		wr[i].wr.opcode = IB_WR_RDMA_WRITE_WITH_IMM;
		wr[i].wr.sg_list = &sge[i];
		wr[i].wr.num_sge = 1;
		wr[i].wr.next = (i == batch_size-1) ? NULL : (struct ib_send_wr*)&wr[i+1];
		wr[i].wr.send_flags = (i == batch_size-1) ? IB_SEND_SIGNALED : 0;
		wr[i].wr.ex.imm_data = htonl(bit_mask(ctx->node_id, pid, type, TX_WRITE_BEGIN, num));
		wr[i].remote_addr = (uintptr_t)(remote_mm + i * PAGE_SIZE);
//		wr[i].remote_addr = remote_mm;
		wr[i].rkey = ctx->rkey;
	}
	dprintk("[%s]: target addr: %llx, target rkey %x\n", __func__, (uint64_t)wr[0].remote_addr, ctx->rkey);

	ret = ib_post_send(ctx->qp, &wr[0].wr, &bad_wr);
	if(ret){
		printk(KERN_ALERT "[%s]: ib_post_send failed\n", __func__);
		return 1;
	}

	do{
		ne = ib_poll_cq(ctx->ic->i_send_cq, 1, &wc);
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

int rdpma_post_recv(void){
	struct ib_recv_wr wr;
	const struct ib_recv_wr* bad_wr;
	struct ib_sge sge;
	int ret;

	memset(&wr, 0, sizeof(struct ib_recv_wr));
	memset(&sge, 0, sizeof(struct ib_sge));

	sge.addr = (uintptr_t)NULL;
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
 * modify_qp
 * Modify queue pair state from Reset to RTS
 * @my_psn:  A 24 bits value of the Packet Sequence Number of the sent packets for any QP
 * @sl: 4 bits. The Service Level to be used
 * @remote: remote node information
 *
 * ret: 0 on success
 */
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
//	dprintk("[%s] ib_modify_qp to RTS succeeded\n", __func__);

	dprintk("[  OK  ] ib_modify_qp to RTS succeeded\n");

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
	//	dprintk("[TCP] socket has been created\n");

	ret = sock->ops->connect(sock, (struct sockaddr*)&addr, sizeof(addr), O_RDWR);
	if(ret){
		printk(KERN_ALERT "[TCP] socket connection failed (%d)\n", ret);
		ksys_close(fd);
		return 1;
	}
//	dprintk("[TCP] socket has been connected to server\n");

	ret = tcp_recv(sock, (char*)&remote, sizeof(struct node_info));
	if(ret){
		printk(KERN_ALERT "[TCP] recv failed\n");
		ksys_close(fd);
		return 1;
	}
//	dprintk("[TCP] received node_id(%d), lid(%d), qpn(%d), psn(%d), mm(%llx), rkey(%x)\n", remote.node_id, remote.lid, remote.qpn, remote.psn, remote.mm, remote.rkey);
	ctx->node_id = remote.node_id;
	ctx->remote_mm = remote.mm;
	ctx->rkey = remote.rkey;

	ret = rdma_query_gid(ctx->dev, ib_port, gid_idx, &gid);
	if(ret){
		printk(KERN_ALERT "[%s] rdma_query_gid failed\n", __func__);
		return 1;
	} 
//	dprintk("[%s] sizeof(struct node_info) : %ld\n", __func__, sizeof(struct node_info));
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
	//	dprintk("[TCP] sent local data to server\n");
//	dprintk("[TCP] sent local data: node_id(%d), lid(%d), qpn(%d), psn(%d), mm(%llx), rkey(%x)\n", local.node_id, local.lid, local.qpn, local.psn, local.mm, local.rkey);

	ret = rdma_is_port_valid(ctx->dev, ib_port);
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


	dprintk("[  OK  ] RDMA connection has been connected to server\n");

	if(fd)
		ksys_close(fd);
	return 0;

}

/**
 * client_init_ctx - Initialize struct client_context.
 *
 * temp_log, process_state, req_list
 * dev, rdpma_ib_connection, mr, qp, cq
 *
 * ctx is global variable.
 */
static struct client_context* client_init_ctx(void){
	int ret = 0, i, flags;

	unsigned long bitmap_size = BITS_TO_LONGS(BITMAP_SIZE) * sizeof(unsigned long);
	unsigned long *bitmap = kzalloc(bitmap_size, GFP_KERNEL);

	ctx = (struct client_context*)kmalloc(sizeof(struct client_context), GFP_KERNEL);
	ctx->temp_log = (uint64_t**)kmalloc(sizeof(uint64_t*)*MAX_PROCESS, GFP_KERNEL);
	ctx->process_state = (volatile int*)kmalloc(sizeof(volatile int)*MAX_PROCESS, GFP_KERNEL);
	//ctx->process_state = (atomic_t*)kmalloc(sizeof(atomic_t)*MAX_PROCESS, GFP_KERNEL);
	for(i = 0; i < MAX_PROCESS; i++){
		ctx->temp_log[i] = (uint64_t*)kmalloc(sizeof(uint64_t)*NUM_ENTRY, GFP_KERNEL);
		if (!ctx->temp_log[i]) {
			printk(KERN_ALERT "[%s] failed to initialize temp_log\n", __func__);
			return NULL;
		}
		//atomic_set(&ctx->process_state[i], PROCESS_STATE_IDLE);
		ctx->process_state[i] = PROCESS_STATE_IDLE;
	}
	
	kref_init(&ctx->kref);
	spin_lock_init(&ctx->lock);
	INIT_LIST_HEAD(&ctx->req_list);
	init_waitqueue_head(&ctx->req_wq);

	atomic_set(&ctx->connected, 0);
	ctx->node_id = -1;
	ctx->bitmap = bitmap;
	ctx->bitmap_size = bitmap_size;
	bitmap_zero(ctx->bitmap, ctx->bitmap_size);
	ctx->send_flags = IB_SEND_SIGNALED;
	ctx->depth = DEPTH;
	ctx->channel = NULL;
	ctx->dev= ibdev;    /* ibdev is assigned in ib.c */
	if(!ctx->dev){
		printk(KERN_ALERT "[%s] failed to initialize ibdev\n", __func__);
		return NULL;
	}

	/* alloc ctx->ic (ib_cm.c) */
	rdpma_ib_conn_alloc(ctx, GFP_KERNEL);

	/* ic->i_pd, ctx->qp here (ib_cm.c) */
	ret = rdpma_ib_setup_qp(ctx);
	if(ret){
		printk(KERN_ALERT "[%s] ib_create_qp failed\n", __func__);
		return NULL;
	}

	flags = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_READ;
	ctx->mr = ctx->ic->i_pd->device->ops.get_dma_mr(ctx->ic->i_pd, flags);
	if (IS_ERR(ctx->mr)) {
		return NULL;
	}
	ctx->mr->device = ctx->ic->i_pd->device;
	ctx->mr->pd = ctx->ic->i_pd;
	ctx->mr->uobject = NULL;
	ctx->mr->need_inval = false;
	ctx->ic->i_pd->__internal_mr = ctx->mr;  /* FIXME : is this right? */
	if(!ctx->mr){
		printk(KERN_ALERT "[%s] ib_allocate_mr failed\n", __func__);
		return NULL;
	}
	//dprintk("[%s] ib_get_dma_mr succeeded\n", __func__);

	return ctx;

err_create_qp:	
	ib_destroy_qp(ctx->qp);
err_ib_create_cq_send:	
	ib_destroy_cq(ctx->ic->i_send_cq);
err_ib_create_cq_recv:    
	ib_destroy_cq(ctx->ic->i_recv_cq);
err_ib_alloc_pd:
	ib_dealloc_pd(ctx->ic->i_pd);

	return NULL;
}

int client_init_interface(struct rdpma_node *nn){
	int ret, x;

	BUG_ON(ibdev == NULL);
	x = rdma_port_get_link_layer(ibdev, ib_port);
	BUG_ON(x != IB_LINK_LAYER_INFINIBAND);

	nn->nn_ctx = client_init_ctx();
	ctx = nn->nn_ctx;
	if(!ctx){
		printk(KERN_ALERT "Failed to initialize client_init_ctx\n");
		return -1;
	}
	pr_info("[  OK  ] client context successfully initailized");

	ret = ib_query_port(ibdev, ib_port, &ctx->port_attr);
	if(ret < 0){
		printk(KERN_ALERT "Failed to query ib_port\n");
		return -1;
	}
//	dprintk("[%s] ib_query_port succeeded\n", __func__);

	ret = establish_conn();
	if(ret){
		printk(KERN_ALERT "Failed to establish connection\n");
		goto cleanup_resources;
	}
	pr_info("[  OK  ] connection successfully established");

	rdpma_post_recv();

	atomic_set(&ctx->connected, 1);
	nn->nn_ctx_valid = 1;

	request_cache = kmem_cache_create("request_cache", sizeof(struct request_struct), 64, SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD, NULL);

	thread_handler = kthread_create((void*)&event_handler, NULL, "event_handler");
	if(IS_ERR(thread_handler)){
		printk(KERN_ALERT "event_handler thread creation failed\n");
		return -1;
	}
	wake_up_process(thread_handler);

	return 0;

cleanup_resources:
	if(ctx->qp) 
		ib_destroy_qp(ctx->qp);
	if(ctx->ic->i_send_cq) 
		ib_destroy_cq(ctx->ic->i_send_cq);
	if(ctx->ic->i_recv_cq) 
		ib_destroy_cq(ctx->ic->i_recv_cq);
	if(ctx->ic->i_pd) 
		ib_dealloc_pd(ctx->ic->i_pd);

	return -1;
}

void cleanup_resource(void){
	if(ctx->qp)
		ib_destroy_qp(ctx->qp);
	if(ctx->ic->i_send_cq) 
		ib_destroy_cq(ctx->ic->i_send_cq);
	if(ctx->ic->i_recv_cq) 
		ib_destroy_cq(ctx->ic->i_recv_cq);
	if(ctx->ic->i_pd)
		ib_dealloc_pd(ctx->ic->i_pd);

	atomic_set(&ctx->connected, 0);
}

static struct class client_class = {
	.name = "rdpma_client_class"
};

/* rdma connection start here */
static void rdpma_start_connect(struct work_struct *work)
{
	struct rdpma_node *nn = 
		container_of(work, struct rdpma_node, nn_connect_work.work);
	int ret = 0;

	nn->nn_last_connect_attempt = jiffies;

	ret = client_init_interface(nn);
	if(ret){
		pr_err("client_init_interface failed\n");
	}

	pr_info("[  OK  ] PMDFC rdma module successfully installed");
	return;
}



static int __init init_net_module(void){
	int i;
	int ret = 0;

	/* if rdma is enabled else do nothing */
	if (enable)
	{
		ret = class_register(&client_class);
		if(ret){
			pr_err("class_register failed\n");
			return -1;
		}

		ret = rdpma_ib_init();
		if(ret){
			pr_err("rdpma_ib_init failed\n");
			goto err_class_register;
		}
		pr_info("[  OK  ] rdpma_ib_device successfully registered\n");

		/* Codes from o2net_start_listening */
		BUG_ON(rdpma_wq != NULL);
		/* perpapre work queue */
		rdpma_wq = alloc_ordered_workqueue("rdpma", WQ_MEM_RECLAIM);
		if (rdpma_wq == NULL) {
			return -ENOMEM; /* ? */
		}

		for (i = 0; i < ARRAY_SIZE(rdpma_nodes); i++) {
			struct rdpma_node *nn = rdpma_nn_from_num(i);
			atomic_set(&nn->nn_timeout, 0);
			spin_lock_init(&nn->nn_lock);

			INIT_DELAYED_WORK(&nn->nn_connect_work, rdpma_start_connect);
//			INIT_DELAYED_WORK(&nn->nn_connect_expired, pmnet_connect_expired);
//			INIT_DELAYED_WORK(&nn->nn_still_up, pmnet_still_up);
			/* until we see hb from a node we'll return einval */
			nn->nn_persistent_error = -ENOTCONN;
			init_waitqueue_head(&nn->nn_ctx_wq);
			idr_init(&nn->nn_status_idr);
			INIT_LIST_HEAD(&nn->nn_status_list);

			queue_delayed_work(rdpma_wq, &nn->nn_connect_work, 0);
		}
		/* follow 0/-E semantic */
		return 0;

err_rdpma_ib_init:
		rdpma_ib_exit();
err_class_register:
		class_unregister(&client_class);
	}

	return ret;
}

static void __exit exit_net_module(void){

	if(thread_recv_poll_cq){
		kthread_stop(thread_recv_poll_cq);
		thread_recv_poll_cq = NULL;
		printk(KERN_INFO "Stopped thread_recv_poll_cq\n");
	}

	if(thread_handler){
		kthread_stop(thread_handler);
		thread_handler = NULL;
		printk(KERN_INFO "Stopped thread_handler\n");
	}
	cleanup_resource();

	if(request_cache) 
		kmem_cache_destroy(request_cache);

	atomic_set(&ctx->connected, 0);

	rdpma_ib_exit();
	class_unregister(&client_class);
	pr_info("[  OK  ] PMDFC rdma module successfully removed ");
}

module_param(enable, int, 0);

module_init(init_net_module);
module_exit(exit_net_module);

MODULE_AUTHOR("Hokeun & Jaeyoun");
MODULE_LICENSE("GPL");
