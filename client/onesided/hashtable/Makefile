CONFIG_MODULE_SIG=n
TARGET_MODULE := ht_test
obj-m := $(TARGET_MODULE).o

KDIR	:= "/lib/modules/$(shell uname -r)/build"
PWD     := $(shell pwd)
CC      := ${CROSS_COMPILE}gcc
AS      := ${CROSS_COMPILE}as

#default: prepare modules
default: modules
modules:
	make -C $(KDIR) M=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean
prepare:
	make -C $(KDIR) modules
load:
	insmod ./$(TARGET_MODULE).ko
unload:
	rmmod ./$(TARGET_MODULE).ko
