.PHONY: all queue clean

CFLAGS := -g
LD := gcc
EXTRA_FLAGS := -lrt -lpthread

all: test 

test:
	gcc -o runQueue test_queue.c ${EXTRA_FLAGS} -I./ 
	g++ -c -o obj/CCEH.o CCEH.cpp ${EXTRA_FLAGS} -DINPLACE
	g++ -o runCCEH test_cceh.cpp obj/CCEH.o ${EXTRA_FLAGS} -DINPLACE
