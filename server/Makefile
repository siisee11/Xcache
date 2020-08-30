.PHONY: all clean
.DEFAULT_GOAL := all

CC=g++
LIBS=-lrt -lm -lpmemobj -lpthread -lboost_thread -lboost_system
CFLAGS=-O3 -std=c++11 -w 
INCLUDES=-I./ 

LD_LIBRARY_PATH=$LD_LIBRARY_PATH:./lib/pmdk:/root/downloads/boost_1_73_0/stage/lib
export LD_LIBRARY_PATH

all: main

main: 
#	$(CXX) $(CFLAGS) -c -o src/CCEH.o src/CCEH.cpp -DINPLACE -lpmemobj -lpmem
	$(CC) $(CFLAGS) -g -c src/CCEH.cpp -o src/CCEH.o $(LIBS) $(INCLUDES) -DINPLACE
	g++ -g -std=c++11 -o server multiQ_server.cpp src/CCEH.cpp $(LIBS) $(INCLUDES)

run:
	sudo ./server ./jy/FILE83

free:
	sudo rm -rf ./jy/*

clean: 
	rm -rf src/*.o server
