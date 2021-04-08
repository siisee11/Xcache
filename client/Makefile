CONFIG_MODULE_SIG=n
obj-m := pmdfc_client.o pmdfc_rdma_core.o rdma_testing.o

pmdfc_rdma_core-objs := rdpma.o
pmdfc_client-objs := pmdfc.o
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
	insmod ./pmdfc_rdma_core.ko sport=$(SVR_PORT) sip=$(SVR_IP) cip=$(CIP) nq=$(NQ)

pmdfc_client:
	insmod ./pmdfc_rdma_core.ko sport=$(SVR_PORT) sip=$(SVR_IP) cip=$(CIP) nq=$(NQ)
	insmod ./pmdfc_client.ko 

pmdfc_client_onesided:
	insmod ./pmdfc_rdma_core.ko sport=$(SVR_PORT) sip=$(SVR_IP) cip=$(CIP) nq=$(NQ)
	insmod ./pmdfc_client.ko onesided=1

tcptest:
	insmod ./pmnet_testing.ko

rdmatest:
	insmod ./pmdfc_rdma_core.ko sport=$(SVR_PORT) sip=$(SVR_IP) cip=$(CIP) nq=$(NQ)
	insmod ./rdma_testing.ko

unload_pmdfc_rdma_core:
	rmmod ./pmdfc_rdma_core.ko
	#rmmod ./pmdfc_client.ko
