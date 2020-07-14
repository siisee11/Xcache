.PHONY: all pmdfc_server clean

CFLAGS := -g
LD := gcc
EXTRA_CFLAGS := -lrt -libverbs -lpthread -dl #-DDEBUG

APP := pmdfc_server
all: server

#pmdfc_server: server.o net.o
#	
#	g++ -g -o $@ $^ ${EXTRA_FLAGS}

server: objs obj/server.o obj/net.o obj/CCEH.o
	g++ -std=c++17 -o ${APP} obj/server.o obj/net.o obj/CCEH.o ${EXTRA_CFLAGS}
#gcc -g -o ${APP} obj/server.o obj/net.o ${EXTRA_CFLAGS}

objs: server.h server.c net.h net.cpp queue.h CCEH.h CCEH.cpp
	g++ -std=c++17 -c -o obj/server.o server.c ${EXTRA_CFLAGS}
	g++ -std=c++17 -O3 -c -o obj/CCEH.o CCEH.cpp -DINPLACE -lrt -lpthread
	g++ -std=c++17 -c -o obj/net.o net.cpp obj/CCEH.o ${EXTRA_CFLAGS} -fpermissive

clean:
	rm -f obj/* ${APP}
