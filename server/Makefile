.PHONY: all clean
.DEFAULT_GOAL := all

CC=g++
LIBS=-lrt -lm -lpmemobj
CFLAGS=-O3 -std=c++11 -w 
INCLUDES=-I./ 

LD_LIBRARY_PATH=$LD_LIBRARY_PATH:./lib/pmdk
export LD_LIBRARY_PATH

all: main

main: 
	$(CC) $(CFLAGS) -c src/CCEH_LSB.cpp -o src/CCEH_LSB.o $(LIBS) $(INCLUDES)
	g++ -std=c++17 -I./ -lrt -lpthread -O3 -o server server.cpp src/CCEH_LSB.cpp -lpmemobj

run:
	sudo ./server ./jy/FILE83

clean: 
	rm -rf src/*.o
