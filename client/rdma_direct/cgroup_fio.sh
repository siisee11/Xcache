#!/bin/bash
#fio ./seq_write.fio &
#fio --directory=/home/siisee11 --name fio_test_file --direct=1 --rw=randread --bs=4K --size=1G --numjobs=16 --time_based --runtime=180 --group_reporting --norandommap &
fio fio-seq-write.fio
echo $! > /dev/cgroup/memory/test_process/tasks &
echo "START CGROUP FIO"
