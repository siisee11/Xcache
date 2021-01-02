#!/bin/bash

NVME_FILE_DIR=/mnt/nvme1/fio_test
SSD_FILE_DIR=/mnt/ssd1/fio_test
HDD_FILE_DIR=/mnt/hdd1/fio_test

#storage_list="
#nvme
#ssd
#hdd"

#storage_list=( "nvme" "ssd" "hdd" )
storage_list=( "ssd" )
#job_list=( "rand_read.fio" "rand_rw.fio" )
job_list=( "rand_read.fio" )

MAX_SIZE=16

for storage in ${storage_list[@]}
do
    for JOB_FILE in ${job_list[@]}
    do
        for((S = 1 , T = 16; S <= $MAX_SIZE; S *= 2, T /= 2))
        do

            if [ "$storage" == "nvme" ]; then
                FILE_DIR=$NVME_FILE_DIR/t${T}
            elif [ "$storage" == "ssd" ]; then
                FILE_DIR=$SSD_FILE_DIR/t${T}
            else
                FILE_DIR=$HDD_FILE_DIR/t${T}
            fi

            echo $T $S $FILE_DIR
            /home/daegyu/drop_cache.sh; sleep 2;
            SIZE=${S}g NUMJOBS=${T} DIR=${FILE_DIR} fio $JOB_FILE >> ./eval_log/${storage}_${JOB_FILE}.log
        done

    done
done
