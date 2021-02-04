#!/bin/bash 

sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
fio ./seq_read.fio &
echo $! > /dev/cgroup/memory/test_process/tasks &
echo "START CGROUP FIO"

wait
sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
