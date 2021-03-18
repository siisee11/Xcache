#ifndef VARIABLES_H
#define VARIABLES_H

#define NUM_NUMA 2

extern size_t initialTableSize;
extern size_t numData;
extern size_t numNetworkThreads;
extern size_t numKVThreads;
extern size_t numPollThreads;
extern bool numa_on;
extern bool verbose_flag;
extern struct bitmask *netcpubuf;
extern struct bitmask *kvcpubuf;
extern struct bitmask *pollcpubuf;

extern int putcnt;
extern int getcnt;

#endif
