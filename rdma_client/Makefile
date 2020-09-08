obj-m := pmrdma.o
pmrdma-objs := net.o

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
	insmod ./pmrdma.ko ip="115.145.173.69" port=5432
test:
	insmod test/test_page.ko
