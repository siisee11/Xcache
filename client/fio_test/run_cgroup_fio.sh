#!/bin/bash 

fio $1 &
echo $! > /dev/cgroup/memory/test_process/tasks &
echo "START CGROUP FIO"

wait
