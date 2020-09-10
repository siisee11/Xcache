obj-m := pmrdma.o pmrdma_testing.o
pmrdma-objs := net.o
pmrdma_testing-objs := page_test.o

KDIR	:= "/lib/modules/$(shell uname -r)/build"
PWD		:= $(shell pwd)
CC      := ${CROSS_COMPILE}gcc
AS      := ${CROSS_COMPILE}as

default: modules
modules:
	make -C $(KDIR) M=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean
load:
	insmod ./pmrdma.ko ip="115.145.173.69" port=2123
testing:
	insmod ./pmrdma_testing.ko
