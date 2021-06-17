.PHONY: clean all

#CFLAGS := -Wall -O2 -g -ggdb -Werror -lrdmacm -libverbs -lpthread
CFLAGS := -std=c++17 -g -Wall
#CFLAGS := -std=c++17 -O3
LIBS := -lrdmacm -libverbs -lpthread -lpmemobj -lnuma  -lpmem  -lssl -lcrypto

#LDLIBS := ${LDLIBS} -lrdmacm -libverbs -lpthread
CXX := g++
INCLUDES=-I./

APPS := rdma_svr rdma_svr_odp rdma_svr_bigmr rdma_svr_mix rdma_svr_mix_srq rdma_svr_onesided rdma_svr_twosided kv_cuckoo kv_linear kv_ext kv_level kv_path kv

all:
	$(CXX) $(CFLAGS) -c -o CCEH.o CCEH_hybrid.cpp $(INCLUDES) $(LIBS) -DINLINE
	$(CXX) $(CFLAGS) -c -o KV.o KV.cpp $(INCLUDES) $(LIBS) 
	$(CXX) $(CFLAGS) -c -o lfcq.o circular_queue.cpp $(LIBS)

mix:
	$(CXX) $(CFLAGS) -c -o CCEH.o CCEH_hybrid.cpp $(INCLUDES) $(LIBS) -DINLINE
	$(CXX) $(CFLAGS) -c -o NuMA_KV.o NuMA_KV.cpp $(INCLUDES) $(LIBS) -DKV_DEBUG
	$(CXX) $(CFLAGS) -c -o lfcq.o circular_queue.cpp $(LIBS)
	$(CXX) $(CFLAGS) -o rdma_svr_mix rdma_svr.cpp lfcq.o CCEH.o NuMA_KV.o $(INCLUDES) $(LIBS) -DTIME_CHECK -DNORMALPUT -DBIGMRGET

bigmr:
	$(CXX) $(CFLAGS) -c -o CCEH.o CCEH_hybrid.cpp $(INCLUDES) $(LIBS) -DINLINE
	$(CXX) $(CFLAGS) -c -o NuMA_KV.o NuMA_KV.cpp $(INCLUDES) $(LIBS) -DKV_DEBUG
	$(CXX) $(CFLAGS) -c -o lfcq.o circular_queue.cpp $(LIBS)
	$(CXX) $(CFLAGS) -o rdma_svr_bigmr rdma_svr.cpp lfcq.o CCEH.o NuMA_KV.o $(INCLUDES) $(LIBS) -DTIME_CHECK -DBIGMRPUT -DBIGMRGET

twosided:
	$(CXX) $(CFLAGS) -c -o lfcq.o circular_queue.cpp $(LIBS)
	$(CXX) $(CFLAGS) -o rdma_svr_twosided rdma_svr.cpp lfcq.o src/cuckoo_hash.o KV_cuckoo.o $(INCLUDES) $(LIBS) -DTIME_CHECK -DTWOSIDED

oneside:
	$(CXX) $(CFLAGS) -c -o CCEH.o CCEH_hybrid.cpp $(INCLUDES) $(LIBS) -DINLINE
	$(CXX) $(CFLAGS) -c -o NuMA_KV.o NuMA_KV.cpp $(INCLUDES) $(LIBS) -DKV_DEBUG
	$(CXX) $(CFLAGS) -c -o lfcq.o circular_queue.cpp $(LIBS)
	$(CXX) $(CFLAGS) -o rdma_svr_onesided rdma_svr.cpp lfcq.o CCEH.o NuMA_KV.o $(INCLUDES) $(LIBS) -DTIME_CHECK -DONESIDED

Cuckoo: KV.cpp src/cuckoo_hash.cpp src/cuckoo_hash.h
	$(CXX) $(CFLAGS) -c src/cuckoo_hash.cpp -o src/cuckoo_hash.o $(LIBS) $(INCLUDES)
	$(CXX) $(CFLAGS) -c -o KV_cuckoo.o KV.cpp $(INCLUDES) $(LIBS) -DCUCKOO
	$(CXX) $(CFLAGS) -o kv_cuckoo test_KV.cpp src/cuckoo_hash.o KV_cuckoo.o $(LIBS) $(INCLUDES)
	$(CXX) $(CFLAGS) -o rdma_svr rdma_svr.cpp  src/cuckoo_hash.o KV_cuckoo.o $(INCLUDES) $(LIBS) -DTIME_CHECK -DTWOSIDED

LinearProbing: src/linear_probing.cpp src/linear_probing.h
	$(CXX) $(CFLAGS) -c src/linear_probing.cpp -o src/linear_probing.o $(LIBS) $(INCLUDES)
	$(CXX) $(CFLAGS) -c -o Logger.o Logger.cpp $(INCLUDES) $(LIBS)
	$(CXX) $(CFLAGS) -c -o KV_linear.o KV.cpp $(INCLUDES) $(LIBS) -DKV_DEBUG
	$(CXX) $(CFLAGS) -o kv_linear test_KV.cpp src/linear_probing.o KV_linear.o Logger.o $(LIBS) $(INCLUDES)
	$(CXX) $(CFLAGS) -o rdma_svr rdma_svr.cpp  src/linear_probing.o KV_linear.o Logger.o $(INCLUDES) $(LIBS) -DTIME_CHECK -DTWOSIDED

CuckooProbing: src/cuckoo_probing.cpp src/cuckoo_probing.h
	$(CXX) $(CFLAGS) -c src/cuckoo_probing.cpp -o src/cuckoo_probing.o $(LIBS) $(INCLUDES)
	$(CXX) $(CFLAGS) -c -o Logger.o Logger.cpp $(INCLUDES) $(LIBS)
	$(CXX) $(CFLAGS) -c -o KV_cuckoop.o KV.cpp $(INCLUDES) $(LIBS) -DKV_DEBUG
	$(CXX) $(CFLAGS) -c -o lfcq.o circular_queue.cpp $(LIBS)
	$(CXX) $(CFLAGS) -o kv_cuckoop test_KV.cpp src/cuckoo_probing.o KV_cuckoop.o Logger.o $(LIBS) $(INCLUDES)
	$(CXX) $(CFLAGS) -o rdma_svr rdma_svr.cpp  src/cuckoo_probing.o KV_cuckoop.o Logger.o $(INCLUDES) $(LIBS) -DTIME_CHECK -DTWOSIDED

Extendible: src/extendible_hash.cpp src/extendible_hash.h
	$(CXX) $(CFLAGS) -c src/extendible_hash.cpp -o src/extendible_hash.o $(LIBS) $(INCLUDES)
	$(CXX) $(CFLAGS) -c -o KV_ext.o KV.cpp $(INCLUDES) $(LIBS) -DEXT
	$(CXX) $(CFLAGS) -o kv_ext test_KV.cpp src/extendible_hash.o KV_ext.o $(LIBS) $(INCLUDES)

Level: src/Level_hashing.cpp src/Level_hashing.h
	$(CXX) $(CFLAGS) -c src/Level_hashing.cpp -o src/Level_hashing.o $(LIBS) $(INCLUDES)
	$(CXX) $(CFLAGS) -c -o KV_level.o KV.cpp $(INCLUDES) $(LIBS) -DLEVEL
	$(CXX) $(CFLAGS) -o kv_level test_KV.cpp src/Level_hashing.o KV_level.o $(LIBS) $(INCLUDES)

Path: src/path_hashing.cpp src/path_hashing.hpp
	$(CXX) $(CFLAGS) -c src/path_hashing.cpp -o src/path_hashing.o $(LIBS) $(INCLUDES)
	$(CXX) $(CFLAGS) -c -o KV_path.o KV.cpp $(INCLUDES) $(LIBS) -DPATH
	$(CXX) $(CFLAGS) -o kv_path test_KV.cpp src/path_hashing.o KV_path.o $(LIBS) $(INCLUDES)

CCEH: src/cceh.cpp src/cceh.h
	$(CXX) $(CFLAGS) -c src/cceh.cpp -o src/cceh.o $(LIBS) $(INCLUDES)
	$(CXX) $(CFLAGS) -c -o KV_cceh.o KV.cpp $(INCLUDES) $(LIBS) -DDCCEH
	$(CXX) $(CFLAGS) -o kv_cceh test_KV.cpp src/cceh.o KV_cceh.o $(LIBS) $(INCLUDES)
	$(CXX) $(CFLAGS) -o rdma_svr rdma_svr.cpp  src/cceh.o KV_cceh.o $(INCLUDES) $(LIBS) -DTIME_CHECK -DTWOSIDED

rdma_dram:
	#numactl -N 0,1 -m 0,1 ./rdma_svr -t 7777
	./rdma_svr -t 7777

rdma_app_direct:
	#rm -f /mnt/pmem0/pmdfc/pm_mr
	#numactl -N 0,1 ./rdma_svr.out 7777
	./rdma_svr -t 7777

rdma_dax_kmem:
	numactl -N 0,1 -m 0,1 ./rdma_svr.out 7777
	#./rdma_svr.out 7777

gdb:
	gdb ./rdma_svr

clean:
	rm -f *.o src/*.o $(APPS)
