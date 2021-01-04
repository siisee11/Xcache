.PHONY: clean all

#CFLAGS := -Wall -O2 -g -ggdb -Werror -lrdmacm -libverbs -lpthread
CFLAGS := -Wall -O2 -Werror -lrdmacm -libverbs -lpthread
#CFLAGS := -Wall -O2 -Werror
#LDLIBS := ${LDLIBS} -lrdmacm -libverbs -lpthread
CC := g++

APPS := rdma_svr

all:
	$(CC) rdma_svr.cpp -o rdma_svr.out $(CFLAGS)

clean:
	rm -f ${APPS}
