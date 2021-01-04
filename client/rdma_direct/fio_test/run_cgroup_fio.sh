#!/bin/bash 

fio ./seq_read.fio &
echo $! > /dev/cgroup/memory/test_process/tasks &
echo "START CGROUP FIO"

wait
