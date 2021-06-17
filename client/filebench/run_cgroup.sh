#!/bin/bash 

echo 0 | sudo tee /proc/sys/kernel/randomize_va_space
#fio $1 --status-interval=1 --output-format=terse --output=out &
filebench -f $1 &
echo $! > /dev/cgroup/memory/test_process/tasks &
echo "START CGROUP BENCH"

#wait
