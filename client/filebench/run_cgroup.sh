#!/bin/bash 

#fio $1 --status-interval=1 --output-format=terse --output=out &
filebench -f mywebserver.f &
echo $! > /dev/cgroup/memory/test_process/tasks &
echo "START CGROUP BENCH"

#wait
