# PMDFC client

This is client kernel module.

## How to run
Before load this module, you must initiate server first.

```make``` to compile client module.
```make net``` to load module.
```make ipoib``` to load module using TCP over Infiniband fabric.
```make rdma``` to load module using RDMA.

After load module, you can test it.
```make tcptest``` to test tcp.
```make rdmatest``` to test rdma

```dmesg -w``` to watch kernel dmesg log.

## How to test

1. 
This module works under memory intensive situation.
To see its effect, run spark tpc benchmark.

2.
Under testing/ directory, there is simple file copy test.
Type `./test <number of iteration>` to run test.

3.
File I/O tester fio can be used.

## Performance measure
This module use debugfs to get system information, you can find it under /sys/kernel/debug/pmdfc

## Tuning Paramenters
Change number of preallocated storage and size in pmdfc.h file.

## Timeline

07/01/2020 	: Attach debugfs and sysfs but don't know how to use.
07/08/2020 	: Able to debug pmnet with debugfs under /sys/kernel/debug/pmdfc/
07/12/2020 	: Seperate network module and pmdfc module.
08/18/2020 	: Develop several method for put_page.
10/27/2020 	: Complete merging RDMA and TCP code.

## TODO
 - [x] ~~TCP Networking~~
 - [x] ~~CCEH integration~~
 - [x] ~~Simple copy file test~~
 - [ ] FIO test
 - [ ] NUMA awareness
 - [ ] performance optimization

## Reference

[How to run Spark-tcpbenchmark?](https://medium.com/@siisee111/spark-benchmark-on-ubuntu-d01171506676)
