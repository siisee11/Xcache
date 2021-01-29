.PHONY: clean all

#CFLAGS := -Wall -O2 -g -ggdb -Werror -lrdmacm -libverbs -lpthread
#CFLAGS := -std=c++17 -g -Wall -O2 
CFLAGS := -std=c++17 -O3 
LIBS := -lrdmacm -libverbs -lpthread -lpmemobj -lnuma  -lpmem

#LDLIBS := ${LDLIBS} -lrdmacm -libverbs -lpthread
CXX := g++
INCLUDES=-I./

APPS := rdma_svr

all:
	$(CXX) $(CFLAGS) -c -o CCEH.o CCEH_hybrid.cpp $(INCLUDES) $(LIBS) -DINLINE
	$(CXX) $(CFLAGS) -c -o NuMA_KV.o NuMA_KV.cpp $(INCLUDES) $(LIBS)
	$(CXX) $(CFLAGS) -c -o lfcq.o circular_queue.cpp $(LIBS)
	$(CXX) $(CFLAGS) -o rdma_svr rdma_svr.cpp lfcq.o CCEH.o NuMA_KV.o $(INCLUDES) $(LIBS) -DTIME_CHECK

rdma_dram:
	numactl -N 0,1 -m 0,1 ./rdma_svr 7777

rdma_app_direct:
	#rm -f /mnt/pmem0/pmdfc/pm_mr
	#numactl -N 0,1 ./rdma_svr.out 7777
	./rdma_svr.out 7777

rdma_dax_kmem:
	numactl -N 0,1 -m 0,1 ./rdma_svr.out 7777
	#./rdma_svr.out 7777

gdb:
	gdb ./rdma_svr

clean:
	rm -f *.o $(APPS)
