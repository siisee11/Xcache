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
	$(CC) $(CFLAGS) -c src/CCEH_LSB.cpp -o src/CCEH_LSB.o $(LIBS) $(INCLUDES)
	g++ -g -std=c++11 -o server server.cpp src/CCEH_LSB.cpp $(LIBS) $(INCLUDES)

run:
	sudo ./server ./jy/FILE83

clean: 
	rm -rf src/*.o server
