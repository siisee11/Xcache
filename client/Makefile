CONFIG_MODULE_SIG=n
obj-m := pmdfc_client.o pmnet.o pmrdma.o

pmdfc_client-objs := pmdfc.o bloom_filter.o 
pmnet-objs := tcp.o nodemanager.o netdebug.o sys.o masklog.o
pmrdma-objs := rdma.o

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
	insmod ./pmdfc_client.ko
#	script/bit_mask_set.sh
net:
	insmod ./pmnet.ko ip="115.145.173.69" port=5432
	insmod ./pmdfc_client.ko
rdma:
	insmod ./pmrdma.ko ip="115.145.173.69" port=2123
	insmod ./pmdfc_client.ko
unload:
	rmmod ./pmnet.ko
	rmmod ./pmdfc_client.ko
	rmmod ./pmrdma.ko
