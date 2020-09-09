.PHONY: all pmdfc_server clean

CFLAGS := -g
LD := gcc
EXTRA_CFLAGS := -lrt -libverbs -lpthread -lpmemobj -lpmem#-DDEBUG

APP := pmdfc_server
all: server

#pmdfc_server: server.o net.o
#	
#	g++ -g -o $@ $^ ${EXTRA_FLAGS}

server: objs server.o net.o
	g++ -std=c++17 -o ${APP} server.o net.o CCEH.o ${EXTRA_CFLAGS}

objs: server.h server.c net.h net.cpp queue.h CCEH.h CCEH.cpp
	g++ -std=c++17 -c -o server.o server.c ${EXTRA_CFLAGS}
	g++ -std=c++17 -O3 -c -o CCEH.o CCEH.cpp -DINPLACE -lrt -lpthread -lpmemobj -lpmem
	g++ -std=c++17 -c -o net.o net.cpp ${EXTRA_CFLAGS}

run:
	sudo ./pmdfc_server -t 2123

clean:
	rm -f *.o ${APP}
