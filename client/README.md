# Xcache client (RDMA based)

This is client kernel module of *Xcache*.
Each client has to be equipped with RDMA capable device.

## Prerequisite
This module tested on linux kernel version from 5.3 to 5.6 

## How to run
Before load this module, you must initiate server first.

```make``` to compile client module.
```make rdma_conn``` to load RDMA communication module.
```make pmdfc_client``` to load module.
```make rdmatest``` to test RDMA communication with microbenchmark.

```dmesg -w``` to watch kernel dmesg log.

## How to test

1. 
This module works under memory intensive situation.
You can use cgroup to limit memory.

2.
Test fio(File I/O) benchmark.
Under fio_test/ directory, there are some scripts.
`gen_cgroup.sh` will generate cgroup limiting memory to about 1GB.
`run_cgroup.fio.sh' will run fio job under cgroup condition.

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
18/03/2021 	: Remove TCP code. RDMA Xcache works on multiple client.

## TODO
 - [x] ~~TCP Networking~~
 - [x] ~~CCEH integration~~
 - [x] ~~Simple copy file test~~
 - [x] ~~RDMA~~
 - [x] ~~micro benchmark test~~
 - [x] ~~fio test~~
 - [x] ~~Multi-client support~~
 - [ ] performance optimization

## Reference

[How to run Spark-tcpbenchmark?](https://medium.com/@siisee111/spark-benchmark-on-ubuntu-d01171506676)
