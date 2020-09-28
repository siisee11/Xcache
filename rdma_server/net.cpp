#include <signal.h>
#include <errno.h>

#include "server.h"
#include "net.h"

extern int ib_port;
extern int tcp_port;
#ifdef DEBUG
extern int errno;
#endif
struct server_context* ctx = NULL;
pthread_t connection_thread;
pthread_t thread_poll_cq;
pthread_t event_handler_thread;

static int running;

static void signal_handler(int signal){
	printf("SIGNAL occur\n");
	running = 0;
}

enum ibv_mtu server_mtu_to_enum(int max_transfer_unit){
	switch(max_transfer_unit){
		case 256:	return IBV_MTU_256;
		case 512:	return IBV_MTU_512;
		case 1024:	return IBV_MTU_1024;
		case 2048:	return IBV_MTU_2048;
		case 4096:	return IBV_MTU_4096;
		default:	return (enum ibv_mtu)0;
	}
}

int poll_cq(struct ibv_cq* cq){
	struct ibv_wc wc;
	int ne, i;

	printf("polling...............\n");

	do{
		ne = ibv_poll_cq(cq, 1, &wc);
		if(ne < 0){
			die("poll_cq failed\n");
		}
	}while(ne < 1);

	if(wc.status != IBV_WC_SUCCESS){
		fprintf(stderr, "Failed status %s[%d] for wr_id %d\n", ibv_wc_status_str(wc.status), wc.status, (int)wc.wr_id);
	}
	return 0;
}

struct ibv_mr* ibv_register_mr(void* addr, int size, int flags){
	struct ibv_mr* ret;
	ret = ibv_reg_mr(ctx->pd, addr, size, flags);
	if(!ret)
		die("ibv_reg_mr failed\n");
	return ret;
}
/*
int rdma_read(int node_id, int type, struct ibv_mr* input, uint64_t addr){
   struct ibv_send_wr wr;
   struct ibv_send_wr* bad_wr;
   struct ibv_sge sge[2];
   int ret, ne;

   memset(&wr, 0, sizeof(struct ibv_send_wr));
   memset(sge, 0, sizeof(struct ibv_sge)*2);
   struct ibv_header header;
   struct ibv_mr* output;

   wr.wr_id = type;
   wr.opcode = IBV_WR_RDMA_READ;
   wr.sg_list = sge;
   wr.num_sge = 1;
   wr.send_flags = IBV_SEND_SIGNALED;

   init_header(ctx->node_id, addr, input->length, type, &header);
   output = ibv_register_mr(&header, sizeof(struct ibv_header), IBV_ACCESS_LOCAL_WRITE);

   sge[0].addr = (uint64_t)output->addr;
   sge[0].length = output->length;
   sge[0].lkey = output->lkey;

   sge[1].addr = (uint64_t)input->addr;
   sge[1].length = input->length;
   sge[1].lkey = input->lkey;

   ret = ibv_post_send(ctx->qp[node_id], &wr, &bad_wr);
   if(ret){
   fprintf(stderr, "[%s] ibv_post_send failed to %d client\n", __func__, node_id);
   die("ibv_post_send failed");
   }

   struct ibv_wc wc[2];
   do{
   ne = ibv_poll_cq(ctx->send_cq, 1, wc);
   if(ne < 0){
   fprintf(stderr, "[%s] ibv_poll_cq failed\n", __func__);
   die("ibv_poll_cq failed");
   }
   }while(ne < 1);

   for(int i=0; i<ne; i++){
   if(wc[i].status != IBV_WC_SUCCESS){
   fprintf(stderr, "[%s] sending request failed status %s (%d) for wr_id %d\n", __func__, ibv_wc_status_str(wc[i].status), wc[i].status, (int)wc[i].wr_id);
   die("ib_poll_cq resulted unknown wc.status");
   }
   }
   ibv_dereg_mr(output);
   return 1;
}*/

int post_recv(int node_id){
	struct ibv_recv_wr wr;
	struct ibv_recv_wr* bad_wr;
	struct ibv_sge sge;

	memset(&wr, 0, sizeof(struct ibv_recv_wr));
	memset(&sge, 0, sizeof(struct ibv_sge));

	sge.addr = 0;
	sge.length = 0;
	sge.lkey = ctx->mr->lkey;

	wr.wr_id = 0;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.next = NULL;

	if(ibv_post_recv(ctx->qp[node_id], &wr, &bad_wr)){
		fprintf(stderr, "[%s] ibv_post_recv to node %d failed\n", __func__, node_id);
		return 1;
	}
	return 0;
}

int post_meta_request(int node_id, int pid, int type, uint32_t num, 
		int tx_state, int len, uint64_t* addr, uint64_t offset){
	struct ibv_send_wr wr;
	struct ibv_send_wr* bad_wr;
	struct ibv_sge sge;

	memset(&wr, 0, sizeof(struct ibv_send_wr));
	memset(&sge, 0, sizeof(struct ibv_sge));

	sge.addr = (uintptr_t)addr;
	sge.length = len;
	sge.lkey = ctx->mr->lkey;

	wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.imm_data = htonl(bit_mask(node_id, pid, type, tx_state, num));
	wr.wr.rdma.remote_addr = (uintptr_t)(ctx->remote_mm[node_id] + offset);
	wr.wr.rdma.rkey = ctx->rkey[node_id];

//	dprintf("[%s]: sending wr.imm_data: %u\t htonl(wr.imm_data): %u\n", __func__, wr.imm_data, htonl(wr.imm_data));
	dprintf("[%s]: node_id(%d), pid(%d), type(%d), tx_state(%d), num(%d)\n", __func__, node_id, pid, type, tx_state, num);
	if(ibv_post_send(ctx->qp[node_id], &wr, &bad_wr)){
		fprintf(stderr, "[%s] ibv_post_send to node %d failed\n", __func__, node_id);
		return 1;
	}

	struct ibv_wc wc;
	int ne;
	do{
		ne = ibv_poll_cq(ctx->send_cq, 1, &wc);
		if(ne < 0){
			fprintf(stderr, "[%s] ibv_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);

	if(wc.status != IBV_WC_SUCCESS){
		fprintf(stderr, "[%s] sending rdma_write failed status %s (%d)\n", __func__, ibv_wc_status_str(wc.status), wc.status);
		return 1;
	}

	return 0;
}



int rdma_write_imm(int node_id, int type, int imm_data, uint64_t offset){
	struct ibv_send_wr wr;
	struct ibv_send_wr* bad_wr;
	struct ibv_sge sge;
	int ne;

	memset(&wr, 0, sizeof(struct ibv_send_wr));
	memset(&sge, 0, sizeof(struct ibv_sge));

	sge.addr = (uintptr_t)NULL;
	sge.length = 0;
	sge.lkey = ctx->mr->lkey;

	//    wr.wr_id = bit_mask(node_id, type, 0);
	wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.imm_data = htonl(imm_data);
	wr.wr.rdma.remote_addr = (uintptr_t)(ctx->remote_mm[node_id] + offset);
	wr.wr.rdma.rkey = ctx->rkey[node_id];

	if(ibv_post_send(ctx->qp[node_id], &wr, &bad_wr)){
		fprintf(stderr, "[%s] ibv_post_send to node %d failed\n", __func__, node_id);
		return 1;
	}
	struct ibv_wc wc;
	do{
		ne = ibv_poll_cq(ctx->send_cq, 1, &wc);
		if(ne < 0){
			fprintf(stderr, "[%s] ibv_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);

	if(wc.status != IBV_WC_SUCCESS){
		fprintf(stderr, "[%s] sending rdma_write failed status %s (%d)\n", __func__, ibv_wc_status_str(wc.status), wc.status);
		return 1;
	}

	return 0;
}


int rdma_write(int node_id, int type, uint64_t addr, int len){
	struct ibv_send_wr wr;
	struct ibv_send_wr* bad_wr;
	struct ibv_sge sge;
	int ne;

	memset(&wr, 0, sizeof(struct ibv_send_wr));
	memset(&sge, 0, sizeof(struct ibv_sge));

	sge.addr = (uint64_t)addr;
	sge.length = len;
	sge.lkey = ctx->mr->lkey;

	//    wr.wr_id = bit_mask(node_id, type, 0);
	wr.opcode = IBV_WR_RDMA_WRITE;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.wr.rdma.remote_addr = ctx->remote_mm[node_id] + METADATA_SIZE; /*writing to reply region*/
	wr.wr.rdma.rkey = ctx->rkey[node_id];

	if(ibv_post_send(ctx->qp[node_id], &wr, &bad_wr)){
		fprintf(stderr, "[%s] ibv_post_send to node %d failed\n", __func__, node_id);
		return 1;
	}

	struct ibv_wc wc;
	do{
		ne = ibv_poll_cq(ctx->send_cq, 1, &wc);
		if(ne < 0){
			fprintf(stderr, "[%s] ibv_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);

	if(wc.status != IBV_WC_SUCCESS){
		fprintf(stderr, "[%s] sending rdma_write failed status %s (%d)\n", __func__, ibv_wc_status_str(wc.status), wc.status);
		return 1;
	}

	return 0;
}

/*
   int rdma_write(int node_id, int type, struct ibv_mr* input, uint64_t addr){
   struct ibv_send_wr wr;
   struct ibv_send_wr* bad_wr;
   struct ibv_sge sge[2];
   int ret, ne;

   memset(&wr, 0, sizeof(struct ibv_send_wr));
   memset(sge, 0, sizeof(struct ibv_sge)*2);
   struct ibv_header header;
   struct ibv_mr* output;

   wr.wr_id = type;
   wr.opcode = IBV_WR_RDMA_WRITE;
   wr.sg_list = sge;
   wr.num_sge = 1;
   wr.send_flags = IBV_SEND_SIGNALED;

   init_header(ctx->node_id, addr, input->length, type, &header);
   output = ibv_register_mr(&header, sizeof(struct ibv_header), IBV_ACCESS_LOCAL_WRITE);

   sge[0].addr = (uint64_t)output->addr;
   sge[0].length = output->length;
   sge[0].lkey = output->lkey;

   sge[1].addr = (uint64_t)input->addr;
   sge[1].length = input->length;
   sge[1].lkey = input->lkey;

   ret = ibv_post_send(ctx->qp[node_id], &wr, &bad_wr);
   if(ret){
   fprintf(stderr, "[%s] ibv_post_send failed to %d client\n", __func__, node_id);
   die("ibv_post_send failed");
   }

   struct ibv_wc wc[2];
   do{
   ne = ibv_poll_cq(ctx->send_cq, 1, wc);
   if(ne < 0){
   fprintf(stderr, "[%s] ibv_poll_cq failed\n", __func__);
   die("ibv_poll_cq failed");
   }
   }while(ne < 1);

   for(int i=0; i<ne; i++){
   if(wc[i].status != IBV_WC_SUCCESS){
   fprintf(stderr, "[%s] sending request failed status %s (%d) for wr_id %d\n", __func__, ibv_wc_status_str(wc[i].status), wc[i].status, (int)wc[i].wr_id);
   die("ib_poll_cq resulted unknown wc.status");
   }
   }
   ibv_dereg_mr(output);
   return 1;
   }*/

int rdma_send(int node_id, void* addr, int size){
	struct ibv_send_wr wr;
	struct ibv_send_wr* bad_wr;
	struct ibv_sge sge;
	int ret;

	memset(&wr, 0, sizeof(struct ibv_send_wr));
	memset(&sge, 0, sizeof(struct ibv_sge));

	sge.addr = (uintptr_t)addr;
	sge.length = size;
	sge.lkey = ctx->mr->lkey;

	wr.wr_id = 0;
	wr.sg_list = &sge;
	wr.num_sge = 1;

	if(ibv_post_send(ctx->qp[node_id], &wr, &bad_wr)){
		fprintf(stderr, "[%s] ibv_post_send failed\n", __func__);
		return 1;
	}
	return 0;
}




int rdma_recv(int node_id, void* addr, int size){
	struct ibv_recv_wr wr;
	struct ibv_recv_wr* bad_wr;
	struct ibv_sge sge;
	int ret;

	memset(&wr, 0, sizeof(struct ibv_recv_wr));
	memset(&sge, 0, sizeof(struct ibv_sge));

	sge.addr = (uintptr_t)addr;
	sge.length = size;
	sge.lkey = ctx->mr->lkey;

	wr.wr_id = 0;
	wr.sg_list = &sge;
	wr.num_sge = 1;

	if(ibv_post_recv(ctx->qp[node_id], &wr, &bad_wr)){
		fprintf(stderr, "[%s] ibv_post_recv failed\n", __func__);
		return 1;
	}
	return 0;
}


/*
   int recv_message(int node_id, int type){
   struct ibv_recv_wr wr;
   struct ibv_recv_wr* bad_wr;
   struct ibv_sge sge[2];

   char* temp_addr;
   char* temp_header;
   struct ibv_mr* temp_mr;
   struct ibv_mr* temp_header_mr;
   struct post_recv_struct* post_recv;

   int ret;

   temp_addr = (char*)malloc(sizeof(char)*RDMA_BUFFER_SIZE*2);
   temp_mr = ibv_register_mr(temp_addr, RDMA_BUFFER_SIZE*2, IBV_ACCESS_LOCAL_WRITE);
   temp_header = (char*)malloc(sizeof(struct ibv_header));
   temp_header_mr = ibv_register_mr(temp_header, sizeof(struct ibv_header), IBV_ACCESS_LOCAL_WRITE);

   post_recv = (struct post_recv_struct*)malloc(sizeof(struct post_recv_struct));
   post_recv->header = (uintptr_t)temp_header;
   post_recv->msg = (uintptr_t)temp_addr;
   post_recv->header_mr = temp_header_mr;
   post_recv->msg_mr = temp_mr;

   memset(&wr, 0, sizeof(struct ibv_recv_wr));
   memset(sge, 0, sizeof(struct ibv_sge)*2);
   sge[0].addr = (uintptr_t)temp_header_mr->addr;
   sge[0].length = temp_header_mr->length;
   sge[0].lkey = temp_header_mr->lkey;

   sge[1].addr = (uintptr_t)temp_mr->addr;
   sge[1].length = temp_mr->length;
   sge[1].lkey = temp_mr->lkey;

   wr.wr_id = (uint64_t)post_recv;
   wr.next = NULL;
   wr.sg_list = sge;
   wr.num_sge = 2;

   ret = ibv_post_recv(ctx->qp[node_id], &wr, &bad_wr);
   if(ret){
   fprintf(stderr, "[%s] ibv_post_recv failed to %d client\n", __func__, node_id);
   die("ibv_post_send failed");
   }
   return 0;
   }

   int send_message(int node_id, int type, struct ibv_mr* input, uint64_t addr){
   struct ibv_send_wr wr;
   struct ibv_send_wr* bad_wr;
   struct ibv_sge sge[2];
   struct ibv_mr* output;

   int ret, ne;

   memset(&wr, 0, sizeof(struct ibv_send_wr));
   memset(sge, 0, sizeof(struct ibv_sge)*2);

   wr.wr_id = (node_id << 8) | type;
   wr.opcode = IBV_WR_SEND;
   wr.sg_list = sge;
   wr.num_sge = 2;
   wr.send_flags = IBV_SEND_SIGNALED;

   sge[0].addr = (uint64_t)output->addr;
   sge[0].length = output->length;
   sge[0].lkey = output->lkey;

   sge[1].addr = (uint64_t)input->addr;
   sge[1].length = input->length;
sge[1].lkey = input->lkey;

ret = ibv_post_send(ctx->qp[node_id], &wr, &bad_wr);
if(ret){
	fprintf(stderr, "[%s] ibv_post_send failed to %d client\n", __func__, node_id);
	die("ibv_post_send failed");
}

struct ibv_wc wc[2];
do{
	ne = ibv_poll_cq(ctx->send_cq, 1, wc);
	if(ne < 0){
		fprintf(stderr, "[%s] ibv_poll_cq failed\n", __func__);
		die("ibv_poll_cq failed");
	}
}while(ne < 1);

for(int i=0; i<ne; i++){
	if(wc[i].status != IBV_WC_SUCCESS){
		fprintf(stderr, "[%s] sending request failed status %s (%d) for wr_id %d\n", __func__, ibv_wc_status_str(wc[i].status), wc[i].status, (int)wc[i].wr_id);
		die("ib_poll_cq resulted unknown wc.status");
	}
}
ibv_dereg_mr(output);
return 0;
}*/


uint32_t bit_mask(int node_id, int pid, int type, int state, uint32_t num){
	uint32_t target = (((uint32_t)node_id << 24) | ((uint32_t)pid << 16) | ((uint32_t)type << 12) | ((uint32_t)state << 8) | ((uint32_t)num & 0x000000ff));
	//  uint64_t target = ((uint64_t)node_id << 56) | ((uint64_t)pid << 48 ) | ((uint64_t)type << 32) | ((uint64_t)size & 0xffffffff);
	return target;
}

void bit_unmask(uint32_t target, int* node_id, int* pid, int* type, int* state, uint32_t* num){
	//int bit_unmask(uint64_t target, int* node_id, int* pid, int* type, uint32_t* size){
	*num = (uint32_t)(target & 0x000000ff);
	*state = (int)((target >> 8) & 0x0000000f);
	*type = (int)((target >> 12) & 0x0000000f);
	*pid = (int)((target >> 16) & 0x000000ff);
	*node_id = (int)((target >> 24) & 0x000000ff);
}

void* server_recv_poll_cq(void* cq_context){
	struct ibv_cq* cq = (struct ibv_cq*)cq_context;
	struct ibv_wc wc;
	int ne;
	static int num = 1;
	printf("[%s] polling ready...\n", __func__);
	while(1){
		ne = 0;
		do{
			ne += ibv_poll_cq(cq, 1, &wc);
			if(ne < 0){
				fprintf(stderr, "ibv_poll_cq failed %d\n", ne);
				die("ibv_poll_cq failed");
			}
		}while(ne < 1);

		if(wc.status != IBV_WC_SUCCESS){
			fprintf(stderr, "Failed status %s (%d)\n", ibv_wc_status_str(wc.status), wc.status);
			die("Failed status");
		}

		struct queue_t* request_queue = ctx->request_queue;
		int ret;
		if((int)wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM){
			int node_id, pid, type, tx_state;
			uint32_t num;
			//bit_unmask(htonl(wc.imm_data), &node_id, &pid, &type, &tx_state, &num);
			bit_unmask(ntohl(wc.imm_data), &node_id, &pid, &type, &tx_state, &num);
			dprintf("[%s]: node_id(%d), pid(%d), type(%d), tx_state(%d), num(%d)\n", __func__, node_id, pid, type, tx_state, num);
			post_recv(node_id);
			if(type == MSG_WRITE_REQUEST){
				dprintf("[%s]: received MSG_WRITE_REQUEST\n", __func__);
				struct request_struct* new_request = (struct request_struct*)malloc(sizeof(struct request_struct));
				new_request->type = type;
				new_request->node_id = node_id;
				new_request->pid = pid;
				new_request->num = num;
				enqueue(request_queue, (void*)new_request);
			}
			else if(type == MSG_WRITE){
				dprintf("[%s]: received MSG_WRITE\n", __func__);
				struct request_struct* new_request = (struct request_struct*)malloc(sizeof(struct request_struct));
				new_request->type = type;
				new_request->node_id = node_id;
				new_request->pid = pid;
				new_request->num = num;
				enqueue(request_queue, (void*)new_request);
			}
			else if(type == MSG_READ_REQUEST){
				dprintf("[%s]: received MSG_READ_REQUEST\n", __func__);
				struct request_struct* new_request = (struct request_struct*)malloc(sizeof(struct request_struct));
				new_request->type = type;
				new_request->node_id = node_id;
				new_request->pid = pid;
				new_request->num = num;
				enqueue(request_queue, (void*)new_request);
			}
			else if(type == MSG_READ_REPLY){
				dprintf("[%s]: received MSG_READ_REPLY\n", __func__);
				free((void*)ctx->temp_log[node_id][pid]);
				//munmap((void*)ctx->temp_log[node_id][pid], num*PAGE_SIZE);
			}
		}
		else if((int)wc.opcode == IBV_WC_RDMA_READ){
			dprintf("[%s]: received WC_RDMA_READ\n", __func__);
			/* the client is reading data from read region*/
		}
		else{
			fprintf(stderr, "Received a weired opcode (%d)\n", (int)wc.opcode);
		}
	}
}


void* event_handler(void*){
	struct request_struct* new_request;
	struct queue_t* request_queue = ctx->request_queue;
	//TOID(CCEH) hashtable = ctx->hashtable;
	int insert_cnt = 0;
	int search_cnt = 0;

	printf("[%s] event_handler ready...\n", __func__);

	while(1){
		new_request = (struct request_struct*)dequeue(request_queue);

		if(new_request->type == MSG_WRITE_REQUEST){
			uint64_t* key = (uint64_t*)GET_CLIENT_META_REGION(ctx->local_mm, new_request->node_id, new_request->pid);
			dprintf("Processing [MSG_WRITE_REQUEST] %d num pages (node=%x, pid=%x, key=%lx)\n", 
					new_request->num, new_request->node_id, new_request->pid, *key);
			//TOID(char) page;
			//POBJ_ALLOC(ctx->pop, &page, char, sizeof(char)*PAGE_SIZE, NULL, NULL);
			void* page = (void*)malloc(new_request->num * PAGE_SIZE);
			//memset(ptr, 0, PAGE_SIZE);
			//uint64_t page_addr = (uint64_t)ctx->pop + page.oid.off;
			//ctx->temp_log[new_request->node_id][new_request->pid] = page_addr;
			ctx->temp_log[new_request->node_id][new_request->pid] = (uint64_t)page;
			//ctx->temp_log[new_request->node_id][new_request->pid] = (uint64_t)&ptr;
			uint64_t offset = NUM_ENTRY * METADATA_SIZE * new_request->pid + sizeof(uint64_t);
			uint64_t* addr = (uint64_t*)(GET_CLIENT_META_REGION(ctx->local_mm, new_request->node_id, new_request->pid) + sizeof(uint64_t));
			*addr = (uint64_t)page;
			//*addr = (uint64_t)&ptr;
			post_meta_request(new_request->node_id, new_request->pid, MSG_WRITE_REQUEST_REPLY, new_request->num, TX_WRITE_READY, sizeof(uint64_t), addr, offset);
			dprintf("Processed  [MSG_WRITE_REQUEST] %d num pages (node=%x pid=%x)\n", new_request->num, new_request->node_id, new_request->pid);
		}
		else if(new_request->type == MSG_WRITE){
			void* ptr = (void*)ctx->temp_log[new_request->node_id][new_request->pid];
			uint64_t* key = (uint64_t*)GET_CLIENT_META_REGION(ctx->local_mm, new_request->node_id, new_request->pid);
			dprintf("Processing [MSG_WRITE] %d num pages (node=%x, pid=%x, key=%lx)\n", 
					new_request->num, new_request->node_id, new_request->pid, *key);
			for(int i = 0; i < new_request->num; i++){
				TOID(char) temp;
				POBJ_ALLOC(ctx->log_pop, &temp, char, sizeof(char)*PAGE_SIZE, NULL, NULL);
				uint64_t temp_addr = (uint64_t)ctx->log_pop + temp.oid.off;
				memcpy((void*)temp_addr, (void *)((uint8_t*)ptr+i*PAGE_SIZE), PAGE_SIZE);
				pmemobj_persist(ctx->log_pop, (char*)temp_addr, sizeof(char)*PAGE_SIZE);

				D_RW(ctx->hashtable)->Insert(ctx->index_pop, *key, (Value_t)temp_addr);
				void* check = (void*)D_RW(ctx->hashtable)->Get(*key);
				fprintf(stderr, "Inserted value for key %lu (%lx)\n", *key, *key);

				key += METADATA_SIZE;
			}
			uint64_t offset = NUM_ENTRY * METADATA_SIZE * new_request->pid + sizeof(uint64_t);
			/* if successfully inserted */
			post_meta_request(new_request->node_id, new_request->pid, MSG_WRITE_REPLY, new_request->num, TX_WRITE_COMMITTED, 0, NULL, offset);
			free(ptr);
			dprintf("Processed  [MSG_WRITE] %d num pages (node=%d pid=%d)\n", new_request->num, new_request->node_id, new_request->pid);
		}
		else if(new_request->type == MSG_READ_REQUEST){
			dprintf("Processing [MSG_READ_REQUEST] %d num pages from node %d with %d pid\n", new_request->num, new_request->node_id, new_request->pid);
			void* page = (void*)malloc(new_request->num * PAGE_SIZE);
			uint64_t offset = NUM_ENTRY * METADATA_SIZE * new_request->pid + sizeof(uint64_t);
			void* values[new_request->num];
			bool abort = false;
			for(int i=0; i<new_request->num; i++){
				uint64_t* key = (uint64_t*)(GET_CLIENT_META_REGION(ctx->local_mm, new_request->node_id, new_request->pid) + i*METADATA_SIZE); 
				dprintf("Target Key is %ld (%lx)\n", *key, *key);
				values[i] = (void*)D_RW(ctx->hashtable)->Get(*key);
				//search_cnt++;
				if(!values[i]){
					dprintf("Value for key[%lx] not found\n", *key);
//					printf("Value is not found!!\n");
					abort = true;
				}
			}

			if(!abort){
				memcpy(page, values[0], PAGE_SIZE * new_request->num);
				ctx->temp_log[new_request->node_id][new_request->pid] = (uint64_t)page;
				uint64_t* addr = (uint64_t*)(GET_CLIENT_META_REGION(ctx->local_mm, new_request->node_id, new_request->pid) + sizeof(uint64_t));
				*addr = (uint64_t)page;
				dprintf("allocated page addr: %lx\n", *addr);
				dprintf("[%s]: addr: %lx, page: %p\n", __func__, *addr, page);
				dprintf("[%s]: msg double check: %s\n", __func__, (char*)page);
				post_meta_request(new_request->node_id, new_request->pid, MSG_READ_REQUEST_REPLY, new_request->num, TX_READ_READY, sizeof(uint64_t), addr, offset);
			}
			else{
				post_meta_request(new_request->node_id, new_request->pid, MSG_READ_REQUEST_REPLY, new_request->num, TX_READ_ABORTED, 0, NULL, offset);
				dprintf("Aborted [MSG_READ_REQUEST] %d num pages from node %d with %d pid\n", new_request->num, new_request->node_id, new_request->pid);
			}
			dprintf("Processing [MSG_READ_REQUEST] %d num pages from node %d with %d pid\n", new_request->num, new_request->node_id, new_request->pid);
		}
		else{
			fprintf(stderr, "Received weired request type %d from node %d\n", new_request->type, new_request->node_id);
		}
	}
	return NULL;
}


static int modify_qp(struct ibv_qp* qp, int my_psn, int sl, struct node_info* dest){
	struct ibv_qp_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_INIT;
	attr.pkey_index = 0;
	attr.port_num = ib_port;
	//    attr.qp_access_flags = 0;
	/*    attr.qp_access_flags =
		  IBV_ACCESS_REMOTE_READ |
		  IBV_ACCESS_REMOTE_WRITE|
		  IBV_ACCESS_REMOTE_ATOMIC;*/

	attr.qp_access_flags =
		IBV_ACCESS_LOCAL_WRITE |
		IBV_ACCESS_REMOTE_READ |
		IBV_ACCESS_REMOTE_WRITE|
		IBV_ACCESS_REMOTE_ATOMIC;
	if(ibv_modify_qp(qp, &attr,
				IBV_QP_STATE |
				IBV_QP_PKEY_INDEX |
				IBV_QP_PORT |
				IBV_QP_ACCESS_FLAGS)){
		die("ibv_modify_qp to INIT failed\n");
		return 1;
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = IBV_MTU_4096;
	attr.dest_qp_num = dest->qpn;
	//    attr.rq_psn = 0;
	attr.rq_psn = dest->psn;
	attr.max_dest_rd_atomic = 16;
	//    attr.max_dest_rd_atomic = 10;
	attr.min_rnr_timer = 12;
	attr.ah_attr.is_global = 0;
	attr.ah_attr.dlid = dest->lid;
	attr.ah_attr.sl = 0;
	//    attr.ah_attr.sl = sl;
	attr.ah_attr.src_path_bits = 1;
	//    attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num = ib_port;
	attr.ah_attr.is_global = 0;

	if(dest->gid.global.interface_id){
		//	attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = dest->gid;
		attr.ah_attr.grh.sgid_index = -1;
	}

	if(ibv_modify_qp(qp, &attr, 
				IBV_QP_STATE |
				IBV_QP_PATH_MTU |
				IBV_QP_DEST_QPN |
				IBV_QP_RQ_PSN |
				IBV_QP_MAX_DEST_RD_ATOMIC |
				IBV_QP_MIN_RNR_TIMER |
				IBV_QP_AV )){
		die("ibv_modify_qp to RTR failed\n");
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = 14;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	//    attr.sq_psn = 0;
	attr.sq_psn = my_psn;
	attr.max_rd_atomic = 16;
	attr.max_dest_rd_atomic = 16;
	if(ibv_modify_qp(qp, &attr,
				IBV_QP_STATE |
				IBV_QP_TIMEOUT |
				IBV_QP_RETRY_CNT |
				IBV_QP_RNR_RETRY |
				IBV_QP_SQ_PSN |
				IBV_QP_MAX_QP_RD_ATOMIC)){
		die("ibv_modify_qp to RTS failed\n");
	}

	printf("[%s] modify_qp to RTS succeeded\n", __func__);
	return 0;
}

/* make PM file and global context */
static struct server_context* server_init_ctx(struct ibv_device* dev, int size, int rx_depth){
	int flags;
	void* ptr;
	char index_path[32] = "/mnt/pmem0/jy/pmem";
	char log_path[32] = "/mnt/pmem0/jy/log";
	const size_t hashtable_initialSize = 1024*16*4; 

	ctx = (struct server_context*)malloc(sizeof(struct server_context));
	ctx->node_id = SERVER_NODE_ID;
	ctx->size = size;
	ctx->send_flags = IBV_SEND_SIGNALED;
	ctx->rx_depth = rx_depth;
	ctx->local_mm = (uint64_t)malloc(LOCAL_META_REGION_SIZE);
	ctx->hashtable = OID_NULL;

	printf("create request queue...\n");
	ctx->request_queue = create_queue();
	ctx->temp_log = (uint64_t**)malloc(sizeof(uint64_t*)*MAX_NODE);
	for(int i=0; i<MAX_NODE; i++){
		ctx->temp_log[i] = (uint64_t*)malloc(sizeof(uint64_t)*MAX_PROCESS);
	}

	if(access(log_path, 0) != 0){
		ctx->log_pop = pmemobj_create(log_path, "log", LOG_SIZE, 0666);
		if(!ctx->log_pop){
			perror("pmemobj_create");
			exit(0);
		}
	}
	else{
		ctx->log_pop = pmemobj_open(log_path, "log");
		if(!ctx->log_pop){
			perror("pmemobj_open");
			exit(0);
		}
	}
	printf("log initialized\n");

	if(access(index_path, 0) != 0){
		ctx->index_pop = pmemobj_create(index_path, "index", INDEX_SIZE, 0666);
		if(!ctx->index_pop){
			perror("pmemobj_create");
			exit(0);
		}
		ctx->hashtable = POBJ_ROOT(ctx->index_pop, CCEH);
		D_RW(ctx->hashtable)->initCCEH(ctx->index_pop, hashtable_initialSize);
	}
	else{
		ctx->index_pop = pmemobj_open(index_path, "index");
		if(!ctx->index_pop){
			perror("pmemobj_open");
			exit(0);
		}
		ctx->hashtable = POBJ_ROOT(ctx->index_pop, CCEH);
	}
	printf("hashtable initialized\n");

	ctx->context = ibv_open_device(dev);
	if(!ctx->context){
		fprintf(stderr, "ibv_open_device failed for %s\n", ibv_get_device_name(dev));
		return NULL;
	}

	ctx->channel = NULL;
	ctx->pd = ibv_alloc_pd(ctx->context);
	if(!ctx->pd)
		die("ibv_alloc_pd failed\n");

	flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_ON_DEMAND;
	//    flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
	//    ctx->mr = ibv_reg_mr(ctx->pd, (void*)ctx->mm, sizeof(char)*4096 * 4, flags);
	//    ctx->mr = ibv_reg_mr(ctx->pd, (void*)ctx->local_mm, LOCAL_TOTAL_REGION_SIZE, flags);
	ctx->mr = ibv_reg_mr(ctx->pd, NULL, -1, flags);
	if(!ctx->mr){
		fprintf(stderr, "ibv_reg_mr failed\n");
		goto dealloc_pd;
	}

	//ptr = mmap(NULL, LOCAL_META_REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, -1, 0);
	//ptr = mmap((void*)ctx->local_mm, LOCAL_META_REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, 0, 0);

	/*
	   if((int)ptr == MAP_FAILED){
	   fprintf(stderr, "mmap failed\n");
	   exit(0);
	   }*/
	//    ctx->local_mm = &ptr;
	memset((void*)ctx->local_mm, 0, LOCAL_META_REGION_SIZE);

	//    ctx->local_ptr = mmap(NULL, LOCAL_TOTAL_SIZE,//here 

	//    ctx->recv_cq = ibv_create_cq(ctx->context, rx_depth+1, NULL, NULL, 0);
	//    ctx->recv_cq = ibv_create_cq(ctx->context, rx_depth+1, NULL, ctx->channel, 0);
	QP_DEPTH = rx_depth+1;
	ctx->recv_cq = ibv_create_cq(ctx->context, QP_DEPTH, NULL, NULL, 0);
	if(!ctx->recv_cq){
		fprintf(stderr, "ibv_create_cq for recv_cq failed\n");
		goto dereg_mr;
	}

	//    ctx->send_cq = ibv_create_cq(ctx->context, rx_depth+1, NULL, NULL, 0);
//	ctx->send_cq = ibv_create_cq(ctx->context, QP_DEPTH, NULL, ctx->channel, 0);
	ctx->send_cq = ibv_create_cq(ctx->context, QP_DEPTH, NULL, NULL, 0);
	if(!ctx->send_cq){
		fprintf(stderr, "ibv_create_cq for send_cq failed\n");
		goto destroy_qp;
	}

//	printf("[%s] Allocate queue pair region\n", __func__);
	ctx->qp = (struct ibv_qp**)malloc(MAX_NODE * sizeof(struct ibv_qp*));
	for(int i=0; i<MAX_NODE; i++){
		struct ibv_qp_init_attr init_attr;
		memset(&init_attr, 0, sizeof(struct ibv_qp_init_attr));

		init_attr.send_cq = ctx->send_cq;
		init_attr.recv_cq = ctx->recv_cq;
		init_attr.cap.max_send_wr = 64;
		init_attr.cap.max_recv_wr = 64;
		init_attr.cap.max_send_sge = 1;
		init_attr.cap.max_recv_sge = 1;

		init_attr.sq_sig_all = 0;
		init_attr.cap.max_inline_data = 0;

		init_attr.qp_type = IBV_QPT_RC;

		ctx->qp[i] = ibv_create_qp(ctx->pd, &init_attr);
		if(!ctx->qp[i]){
			fprintf(stderr, "ibv_create_qp[%d] failed\n", i);
			goto destroy_qp;
		}
		printf("[%s] queue pair[%d] created\n", __func__, i);
	}

	return ctx;

destroy_qp:
	if(ctx->send_cq)
		ibv_destroy_cq(ctx->send_cq);
	if(ctx->recv_cq) 
		ibv_destroy_cq(ctx->recv_cq);
	for(int i=0; i<MAX_NODE; i++)
		if(ctx->qp[i]) 
			ibv_destroy_qp(ctx->qp[i]);
dereg_mr:
	if(ctx->mr) 
		ibv_dereg_mr(ctx->mr);
dealloc_pd:
	if(ctx->channel) 
		ibv_destroy_comp_channel(ctx->channel);
	if(ctx->pd) 
		ibv_dealloc_pd(ctx->pd);
	return NULL;
}

void* establish_conn(void*){
	int cur_node = 0;
	int sock, fd, ret;
	struct sockaddr_in local_sock;
	int gid_idx = 0;
	int on = 1;
	running = 1;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if(sock < 0){
		die("Socket creation failed\n");
	}

	memset(&local_sock, 0, sizeof(struct sockaddr_in));
	local_sock.sin_family = AF_INET;
	local_sock.sin_addr.s_addr = htonl(INADDR_ANY);
	local_sock.sin_port = htons(tcp_port);

	if((bind(sock, (struct sockaddr*)&local_sock, sizeof(local_sock))) < 0)
		die("Socket bind failed\n");

	if((listen(sock, 10)) < 0)
		die("Socket listen failed\n");

	while(running){
		socklen_t sin_size = sizeof(struct sockaddr);
		struct sockaddr_in remote_sock;
		char remote_ip[INET_ADDRSTRLEN];
		struct node_info local_node, remote_node;
		union ibv_gid gid;
		memset(&local_node, 0, sizeof(struct node_info));
		memset(&remote_node, 0, sizeof(struct node_info));
		memset(&remote_sock, 0, sizeof(struct sockaddr_in));

		fd = accept(sock, (struct sockaddr*)&remote_sock, (socklen_t*)&sin_size);
		if(fd < 0){
			fprintf(stderr, "Server accept failed\n");
			close(fd);
			close(sock);
			exit(1);
		}
		inet_ntop(AF_INET, &remote_sock.sin_addr, remote_ip, INET_ADDRSTRLEN);
		printf("TCP Socket accepted a connection %d from %s\n", cur_node, remote_ip);

		//	ret = ibv_query_gid(ctx->context, ib_port, 2, &gid);
		ret = ibv_query_gid(ctx->context, ib_port, gid_idx, &gid);
		if(ret){
			fprintf(stderr, "ib_query_gid failed\n");
			close(fd);
			close(sock);
			exit(1);
		}

		local_node.node_id = cur_node;
		local_node.lid = ctx->port_attr.lid;
		local_node.qpn = ctx->qp[cur_node]->qp_num;
		local_node.psn = lrand48() & 0xffffff;
		local_node.mm = ctx->local_mm + (cur_node * LOCAL_META_REGION_SIZE);
		local_node.rkey = ctx->mr->rkey;
		local_node.gid = gid;
		dprintf("[TCP] sent local data: node_id(%d), lid(%d), qpn(%d), psn(%d), mm(%lx), rkey(%x)\n", local_node.node_id, local_node.lid, local_node.qpn, local_node.psn, local_node.mm, local_node.rkey);
		//	ret = tcp_send(fd, &local_node, sizeof(struct node_info));
		ret = write(fd, (char*)&local_node, sizeof(struct node_info));
		if(ret != sizeof(struct node_info)){
			fprintf(stderr, "[TCP] write failed\n");
			close(fd);
			close(sock);
			exit(1);
		}

		//	ret = tcp_recv(fd, &remote_node, sizeof(struct node_info));
		ret = read(fd, (char*)&remote_node, sizeof(struct node_info));
		if(ret != sizeof(struct node_info)){
			fprintf(stderr, "[TCP] read failed\n");
			close(fd);
			close(sock);
			exit(1);
		}
		dprintf("[TCP] received node_id(%d), lid(%d), qpn(%d), psn(%d), mm(%lx), rkey(%x)\n", remote_node.node_id, remote_node.lid, remote_node.qpn, remote_node.psn, remote_node.mm, remote_node.rkey);

		ctx->remote_mm[remote_node.node_id] = remote_node.mm;
		ctx->rkey[remote_node.node_id] = remote_node.rkey;

		//	ctx->rkey[cur_node-1] = remote_node.rkey;
		/*TODO: remote mr
		  1. getting mr info upon each request?
		  2. setting a static mr
		  */
		//	ctx->remote_mm[cur_node-1] = remote_node.mm;
		ret = modify_qp(ctx->qp[remote_node.node_id], local_node.psn, 0, &remote_node);
		if(ret){
			fprintf(stderr, "ib_modify_qp failed for %d client\n", remote_node.node_id);
			close(fd);
			close(sock);
			exit(1);
		}
		uint64_t start = ctx->local_mm + (cur_node * LOCAL_META_REGION_SIZE);
		uint64_t end = ctx->local_mm + ((cur_node+1) * LOCAL_META_REGION_SIZE);
		post_recv(cur_node);

		cur_node++;

		printf("RDMA connection established.\n");
	}
	if(fd)
		close(fd);
	if(sock)
		close(sock);
	exit(0);
}

int query_qp(struct ibv_qp* qp){
	int ret;
	struct ibv_qp_attr attr;
	struct ibv_qp_init_attr init_attr;

	ret = ibv_query_qp(qp, &attr, IBV_QP_STATE, &init_attr);
	switch(attr.qp_state){
		case IBV_QPS_INIT:
			printf("[%s] current qp state is ib_qps_init\n", __func__);
			break;
		case IBV_QPS_RTR:
			printf("[%s] current qp state is ib_qps_rts\n", __func__);
			break;
		case IBV_QPS_RTS:
			printf("[%s] current qp state is ib_qps_rts\n", __func__);
			break;
		case IBV_QPS_RESET:
			printf("[%s] current qp state is ib_qps_reset\n", __func__);
			break;
		case IBV_QPS_SQD:
			printf("[%s] current qp state is ib_qps_sqd\n", __func__);
			break;
		case IBV_QPS_SQE:
			printf("[%s] current qp state is ib_qps_sqe\n", __func__);
			break;
		case IBV_QPS_ERR:
			printf("[%s] current qp state is ib_qps_err\n", __func__);
			break;
		default:
			printf("[%s] current qp state is ib_qps_unknown\n", __func__);
	}
	return 1;
}

int server_init_interface(){
	struct ibv_device** dev_list = NULL;
	struct ibv_device* dev = NULL;
	char* dev_name = NULL;
	struct ibv_context* context;
	struct ibv_device_attr dev_attr;
	int dev_num = 0;
	int size = 4096;
	int rx_depth = 256;
	int ret;

	dev_list = ibv_get_device_list(&dev_num);
	if(!dev_list)
		die("ibv_get_device_list failed\n");

	for(int i=0; i<dev_num; i++){
		if(!dev_name) dev_name = strdup(ibv_get_device_name(dev_list[i]));
		if(!strcmp(ibv_get_device_name(dev_list[i]), dev_name)){
			dev = dev_list[i];
			break;
		}
	}

	if(!dev)
		die("ib_device is not found\n");

	context = ibv_open_device(dev);
	if(!context)
		die("ibv_open_device failed\n");

	ret = ibv_query_device(context, &dev_attr);
	if(ret)
		die("ibv_query_device failed\n");

	ctx = server_init_ctx(dev, size, rx_depth);
	if(!ctx)
		die("server_init_ctx failed\n");

	ret = ibv_query_port(ctx->context, ib_port, &ctx->port_attr);
	if(ret)
		die("ibv_query_port failed\n");

	ibv_free_device_list(dev_list);

	pthread_create(&connection_thread, NULL, &establish_conn, NULL);
	pthread_create(&thread_poll_cq, NULL, &server_recv_poll_cq, (void*)ctx->recv_cq);
	pthread_create(&event_handler_thread, NULL, &event_handler, NULL);

	return 0;
}

void init_server(){
	int status;
	int ret = server_init_interface();
	
	pthread_join(connection_thread, (void **)&status);
	pthread_join(thread_poll_cq, (void **)&status);
	pthread_join(event_handler_thread, (void **)&status);
}
