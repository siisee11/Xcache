obj-m := pmrdma.o pmrdma_testing.o
pmrdma-objs := rdma.o
pmrdma_testing-objs := page_test.o

KDIR	:= "/lib/modules/$(shell uname -r)/build"
PWD		:= $(shell pwd)
CC      := ${CROSS_COMPILE}gcc
AS      := ${CROSS_COMPILE}as

EXTRA_SYMBERS := /usr/src/ofa_kernel/default/Module.symvers

default: modules
modules:
	make -C $(KDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(EXTRA_SYMBERS) modules
clean:
	make -C $(KDIR) M=$(PWD) clean
load:
	insmod ./pmrdma.ko ip="10.0.0.51" port=2123
run:
	insmod ./pmrdma_testing.ko


	#insmod ./pmrdma.ko ip="192.168.0.113" port=2123
