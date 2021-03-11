TARGET_MODULE := my_rdma
obj-m := $(TARGET_MODULE).o

KDIR := "/lib/modules/$(shell uname -r)/build"
PWD := $(shell pwd)
CC := ${CROSS_COMPILE}gcc
AS := ${CROSS_COMPILE}AS

default: modules

modules:
	make -C $(KDIR) M=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean

load:
	insmod ./$(TARGET_MODULE).ko
unload:
	rmmod ./$(TARGET_MODULE).ko
	
