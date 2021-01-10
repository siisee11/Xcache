CONFIG_MODULE_SIG=n
obj-m := pmdfc_dram.o

pmdfc_dram-objs := pmdfc.o bloom_filter.o 
#pmnet-objs := tcp.o nodemanager.o netdebug.o sys.o masklog.o

KDIR	:= "/lib/modules/$(shell uname -r)/build"
#KDIR	:= "/lib/modules/$(shell uname -r)/build"
PWD     := $(shell pwd)
CC      := ${CROSS_COMPILE}gcc
AS      := ${CROSS_COMPILE}as

EXTRA_CFLAGS  += -g 
#EXTRA_CFLAGS  += -DPMDFC_BF
#DEBUG_FLAGS += -DPMDFC_DEBUG=1 
#DEBUG_FLAGS += -DPMDFC_PMCB=1
#DEBUG_FLAGS += -DPMDFC_DEBUG_FS=1

#default: prepare modules
default: modules
modules:
	make -j 16 -C $(KDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(EXTRA_SYMBERS) modules

clean:
	make -j 16 -C $(KDIR) M=$(PWD) clean

load_dram:
	insmod ./pmdfc_dram.ko
unload_dram:
	rmmod ./pmdfc_dram.ko

