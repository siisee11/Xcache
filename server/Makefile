.PHONY: all clean
.DEFAULT_GOAL := all

CC=g++
#LIBS=-lrt -lm -lpmemobj -lpmem -lpthread -lboost_thread -lboost_system
LDFLAGS=
LIBS=-lrt -lm -lpmemobj -lpmem -lpthread -libverbs -lnuma
#CFLAGS=-O3 -std=c++11 -w 
#CFLAGS := -std=c++17 -g
CFLAGS := -std=c++17 -g -O3
INCLUDES=-I./ -I./src/

SOURCES=server.cpp tcp.cpp rdma.cpp CCEH.cpp
OBJECTS=server.o 
EXECUTABLE=server

LD_LIBRARY_PATH=$LD_LIBRARY_PATH:./lib/pmdk
export LD_LIBRARY_PATH

all: CCEH $(EXECUTABLE)

$(EXECUTABLE):
	$(CC) $(CFLAGS) -o bin/tcp_server src/tcp_server.cpp src/lfcq.o src/CCEH.o src/NuMA_KV.o $(INCLUDES) $(LIBS)
	$(CC) $(CFLAGS) -o bin/tcp_server_skewed_pm src/tcp_server.cpp src/lfcq.o src/CCEH_skewed.o src/NuMA_KV_PM.o $(INCLUDES) $(LIBS) -DAPPDIRECT
#	$(CC) $(CFLAGS) -o bin/rdma_server_skewed src/rdma_server.cpp src/lfcq.o src/CCEH_skewed.o src/NuMA_KV_PM.o $(INCLUDES) $(LIBS)

CCEH: 
	$(CXX) $(CFLAGS) -c -o src/CCEH_balanced.o src/CCEH_PM_hybrid.cpp $(INCLUDES) $(LIBS) -DBALANCED
	$(CXX) $(CFLAGS) -c -o src/CCEH_random.o src/CCEH_PM_hybrid.cpp $(INCLUDES) $(LIBS) -DRANDOM
	$(CXX) $(CFLAGS) -c -o src/CCEH_lrfu.o src/CCEH_PM_hybrid.cpp $(INCLUDES) $(LIBS) -DLRFU
	$(CXX) $(CFLAGS) -c -o src/CCEH_skewed.o src/CCEH_PM_hybrid.cpp $(INCLUDES) $(LIBS)
	$(CXX) $(CFLAGS) -c -o src/CCEH.o src/CCEH_hybrid.cpp $(INCLUDES) $(LIBS) -DINLINE
	$(CXX) $(CFLAGS) -c -o src/NuMA_KV_PM.o src/NuMA_KV_PM.cpp $(INCLUDES) $(LIBS) -DKV_DEBUG
	$(CXX) $(CFLAGS) -c -o src/NuMA_KV_PM_NUMA.o src/NuMA_KV_PM.cpp $(INCLUDES) $(LIBS) -DNUMAQ -DKV_DEBUG
	$(CXX) $(CFLAGS) -c -o src/NuMA_KV.o src/NuMA_KV.cpp $(INCLUDES) $(LIBS)
	$(CXX) -std=c++17 -c -o src/lfcq.o src/circular_queue.cpp $(LIBS)

tcp:
	sudo rm -rf /mnt/pmem0/jy/*
	sudo rm -rf /mnt/pmem1/jy/*
	sudo bin/tcp_server -t 5432 --pm_path /jy/1 -W 20 -K 28,38 -P 29,39 --tablesize 32768 --verbose -h

rdma:
	sudo rm -rf /mnt/pmem0/jy/*
	sudo rm -rf /mnt/pmem1/jy/*
	sudo bin/rdma_server_skewed -t 5432 --pm_path /jy/1 --dataset /home/siisee11/git/NuMA/input_rand.txt --nr_data 10000000 -W 20 -K 26-27,36-37 -P 28-29,38-39 --tablesize 32768 --verbose -h

gdb:
	sudo rm -rf /mnt/pmem0/jy/*
	sudo rm -rf /mnt/pmem1/jy/*
	sudo gdb bin/tcp_server

free:
	sudo rm -rf /mnt/pmem0/jy/*
	sudo rm -rf /mnt/pmem1/jy/*

clean: 
	rm -rf src/*.o *.o server
