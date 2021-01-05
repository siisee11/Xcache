.PHONY: clean all

#CFLAGS := -Wall -O2 -g -ggdb -Werror -lrdmacm -libverbs -lpthread
CFLAGS := -Wall -O2 -lrdmacm -libverbs -lpthread 
CFLAGS += -lpmemobj
#CFLAGS += -DAPP_DIRECT
CFLAGS += -lnuma -DDAX_KMEM
#LDLIBS := ${LDLIBS} -lrdmacm -libverbs -lpthread
CC := g++

APPS := rdma_svr

all:
	$(CC) rdma_svr.cpp -o rdma_svr.out $(CFLAGS)

rdma_dram:
	#numactl -N 0,1 ./rdma_svr.out 7777
	numactl -N 0,1 -m 0,1 ./rdma_svr.out 7777

rdma_app_direct:
	#rm -f /mnt/pmem0/pmdfc/pm_mr
	#numactl -N 0,1 ./rdma_svr.out 7777
	./rdma_svr.out 7777

rdma_dax_kmem:
	numactl -N 0,1 -m 0,1 ./rdma_svr.out 7777
	#./rdma_svr.out 7777

clean:
	rm -f *.o *.out
