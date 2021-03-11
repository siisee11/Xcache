CONFIG_MODULE_SIG=n
obj-m := pmdfc_client.o pmnet.o pmnet_testing.o rdma_testing.o rdpma_mod.o rdpma_testing.o pmdfc_rdma_core.o
#obj-m := pmdfc_client.o pmnet.o pmrdma.o

pmdfc_client-objs := pmdfc.o bloom_filter.o 
pmnet-objs := tcp.o nodemanager.o netdebug.o sys.o masklog.o
pmnet_testing-objs := page_test.o
rdma_testing-objs := rdma_page_test.o
pmdfc_rdma_core-objs := rdma_conn.o rdma_op.o
rdpma_mod-objs := rdpma/ib.o rdpma/ib_cm.o rdpma/ib_recv.o rdpma/ib_send.o rdpma/rdpma.o
rdpma_testing-objs := rdpma/page_test.o

KDIR	:= "/lib/modules/$(shell uname -r)/build"
PWD     := $(shell pwd)
CC      := ${CROSS_COMPILE}gcc
AS      := ${CROSS_COMPILE}as

EXTRA_CFLAGS  += -g 
EXTRA_SYMBERS := /usr/src/ofa_kernel/default/Module.symvers

#EXTRA_CFLAGS  += -g -DMAX_SEQ=32 -DRCACHE_STORE_BUFFER -DRCACHE_LOAD_CACHE -DRCACHE_SG # -DDEBUG
#EXTRA_CFLAGS  += -g -DRCACHE_STORE_BUFFER -DMAX_SEQ=32 -DRCACHE_SG -DRCACHE_STORE_SORTED #-D__DEBUG__
#EXTRA_CFLAGS  += -O2 -DDEBUG -DRCACHE_STORE_BUFFER #-D__DEBUG__
#AFLAGS        += -g -D__ASSEMBLY__ -D__DEBUG__

CLI_IP="10.0.0.51"
SVR_IP="10.0.0.53"
SVR_PORT=7777
#NQ=2 # for test, R:W=1:1
#EXTRA_CFLAGS  += -DSINGLE_TEST
NQ=16 # for test, 0: read, 1: write

#default: prepare modules
default: modules
modules:
	make -C $(KDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(EXTRA_SYMBERS) modules
clean:
	make -C $(KDIR) M=$(PWD) clean
prepare:
	make -C $(KDIR) modules
load:
	insmod ./pmdfc_client.ko
#	script/bit_mask_set.sh
pmnet:
	insmod ./pmnet.ko ip="115.145.173.135" port=5432
net:
	insmod ./pmnet.ko ip="115.145.173.63" port=5432
	insmod ./rdpma_mod.ko ip="10.0.0.53" port=2123 enable=0
	insmod ./pmdfc_client.ko
ipoib:
	insmod ./pmnet.ko ip="10.0.0.53" port=5432
	insmod ./rdpma_mod.ko ip="10.0.0.53" port=2123 enable=0
	insmod ./pmdfc_client.ko
rdma:
	insmod ./pmnet.ko ip="10.0.0.53" port=5432
	insmod ./rdpma_mod.ko ip="10.0.0.53" port=2123 enable=1
	insmod ./pmdfc_client.ko rdma=1
rdma_conn:
	insmod ./pmdfc_rdma_core.ko sport=$(SVR_PORT) sip=$(SVR_IP) cip=$(CLI_IP) nq=$(NQ)
	insmod ./pmdfc_client.ko rdma=1 rdma_direct=1
pmdfc:
	insmod ./pmdfc_client.ko
tcptest:
	insmod ./pmnet_testing.ko
rdmatest:
	insmod ./rdma_testing.ko
unload:
	rmmod ./pmdfc_client.ko
	rmmod ./pmnet.ko
	rmmod ./rdpma_mod.ko
