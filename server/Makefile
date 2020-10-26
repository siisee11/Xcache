.PHONY: all clean
.DEFAULT_GOAL := all

CC=g++
#LIBS=-lrt -lm -lpmemobj -lpmem -lpthread -lboost_thread -lboost_system
LDFLAGS=
LIBS=-lrt -lm -lpmemobj -lpmem -lpthread -libverbs
#CFLAGS=-O3 -std=c++11 -w 
CFLAGS=-std=c++11 -w 
INCLUDES=-I./ 

SOURCES=server.cpp tcp.cpp rdma.cpp CCEH.cpp
OBJECTS=server.o 
EXECUTABLE=server

LD_LIBRARY_PATH=$LD_LIBRARY_PATH:./lib/pmdk
export LD_LIBRARY_PATH

#all: $(SOURCES) $(EXECUTABLE)
all: $(EXECUTABLE)
#$(EXECUTABLE): $(OBJECTS) 
#	    $(CC) $(LDFLAGS) $(OBJECTS) -o $@ $(LIBS)

$(EXECUTABLE):
	$(CC) $(CFLAGS) -g server.cpp tcp.cpp queue.cpp rdma.cpp CCEH.cpp -o server $(LIBS) -DINPLACE

#CCEH.o:
#	$(CC) $(CFLAGS) -o CCEH.o CCEH.cpp -DINPLACE $(LIBS)

tcp:
	sudo rm -rf ./jy/*
	sudo ./server -t 5432 -p jy/FILE

rdma:
	sudo rm -rf ./jy/*
	sudo rm -rf ./hk/*
	sudo ./server --rdma -t 2123 -p jy/FILE

gdb:
	sudo rm -rf ./jy/*
	sudo gdb ./server

free:
	sudo rm -rf ./jy/*

clean: 
	rm -rf src/*.o *.o server
