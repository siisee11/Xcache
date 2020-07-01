# PMDFC client

This is client kernel module.

## How to run

```make``` to compile client module.
```make load``` to load module.
```dmesg -w``` to watch kernel dmesg log.

## Run with spark

This module works under memory intensive situation.
To see its effect, run spark tpc benchmark.


## Timeline

7/1/2020 : Attach debugfs and sysfs but don't know how to use.


## Reference

[How to run Spark-tcpbenchmark?](https://medium.com/@siisee111/spark-benchmark-on-ubuntu-d01171506676)
