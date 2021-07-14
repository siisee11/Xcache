CONFIG_MODULE_SIG=n
obj-m := julee_client.o julee_rdma_core.o rdma_testing.o juleeswap_client.o

julee_client-objs := julee.o
juleeswap_client-objs := juleeswap.o
julee_rdma_core-objs := rdpma.o bloom_filter.o
rdma_testing-objs := rdpma_page_test.o

KDIR	:= "/lib/modules/$(shell uname -r)/build"
PWD     := $(shell pwd)
CC      := ${CROSS_COMPILE}gcc
AS      := ${CROSS_COMPILE}as

EXTRA_CFLAGS  += -g 

CLI_IP="10.0.0.101"
CIP 	:= "$(shell sh -c "hostname -I|cut -f2 -d ' '")"
SVR_IP="10.0.0.53"
SVR_PORT=7777
NQ=8

#default: prepare modules
default: modules
modules:
	make -C $(KDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(EXTRA_SYMBERS) modules
clean:
	make -C $(KDIR) M=$(PWD) clean

rdma_conn:
	insmod ./julee_rdma_core.ko sport=$(SVR_PORT) sip=$(SVR_IP) cip=$(CIP) nq=$(NQ)

julee_client:
	insmod ./julee_rdma_core.ko sport=$(SVR_PORT) sip=$(SVR_IP) cip=$(CIP) nq=$(NQ)
	insmod ./julee_client.ko 
#	insmod ./juleeswap_client.ko 

julee_client_onesided:
	insmod ./julee_rdma_core.ko sport=$(SVR_PORT) sip=$(SVR_IP) cip=$(CIP) nq=$(NQ)
	insmod ./julee_client.ko onesided=1

tcptest:
	insmod ./pmnet_testing.ko

rdmatest:
	insmod ./julee_rdma_core.ko sport=$(SVR_PORT) sip=$(SVR_IP) cip=$(CIP) nq=$(NQ)
	insmod ./rdma_testing.ko

unload_julee_rdma_core:
	rmmod ./julee_rdma_core.ko
	#rmmod ./julee_client.ko
