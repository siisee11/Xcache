#!/bin/bash 

#fio $1 --status-interval=1 --output-format=terse --output=out &
fio $1 --output=out &
echo $! > /dev/cgroup/memory/test_process/tasks &
echo "START CGROUP FIO"

#wait
