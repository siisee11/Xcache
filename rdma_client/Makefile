obj-m := rdpma_mod.o rdpma_testing.o rdpma_ib.o 
rdpma_ib-objs := ib-single-module.o ib_rdma.o
rdpma_mod-objs := ib.o ib_cm.o ib_recv.o ib_send.o rdpma.o 
rdpma_testing-objs := page_test.o

KDIR	:= "/lib/modules/$(shell uname -r)/build"
PWD		:= $(shell pwd)
CC      := ${CROSS_COMPILE}gcc
AS      := ${CROSS_COMPILE}as

EXTRA_CFLAGS  += -g 
#EXTRA_SYMBERS := /usr/src/ofa_kernel-5.1/default/Module.symvers
#EXTRA_SYMBERS := /usr/src/ofa_kernel/default/Module.symvers

default: modules
modules:
	make -C $(KDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(EXTRA_SYMBERS) modules
clean:
	make -C $(KDIR) M=$(PWD) clean
ib:
	insmod ./rdpma_ib.ko
load:
	insmod ./rdpma_mod.ko ip="10.0.0.53" port=2123
run:
	insmod ./rdpma_testing.ko
unload:
	rmmod rdpma_ib
	rmmod rdpma_mod
