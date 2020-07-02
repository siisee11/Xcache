CONFIG_MODULE_SIG=n
TARGET_MODULE := pmdfc_client
obj-m := $(TARGET_MODULE).o

pmdfc_client-objs := sync_pmdfc.o tcp.o bloom_filter.o nodemanager.o netdebug.o sys.o masklog.o

KDIR	:= "/lib/modules/$(shell uname -r)/build"
PWD     := $(shell pwd)
CC      := ${CROSS_COMPILE}gcc
AS      := ${CROSS_COMPILE}as
EXTRA_CFLAGS  += -g 
#EXTRA_CFLAGS  += -g -DMAX_SEQ=32 -DRCACHE_STORE_BUFFER -DRCACHE_LOAD_CACHE -DRCACHE_SG # -DDEBUG
# EXTRA_CFLAGS  += -g -DRCACHE_STORE_BUFFER -DMAX_SEQ=32 -DRCACHE_SG -DRCACHE_STORE_SORTED #-D__DEBUG__
# EXTRA_CFLAGS  += -O2 -DDEBUG -DRCACHE_STORE_BUFFER #-D__DEBUG__
#AFLAGS        += -g -D__ASSEMBLY__ -D__DEBUG__

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
	script/bit_mask_set.sh
unload:
	rmmod ./$(TARGET_MODULE).ko
