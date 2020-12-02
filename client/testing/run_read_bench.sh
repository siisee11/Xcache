#!/bin/bash 

NVME_FILE_DIR=/mnt/nvme1/dummy
SSD_FILE_DIR=/mnt/ssd1/dummy
HDD_FILE_DIR=/mnt/hdd1/dummy

storage_list=( "nvme" "ssd" "hdd" )

for storage in ${storage_list[@]}
do
	for((i=8; i <= 40; i+=8))
	do
		if [ "$storage" == "nvme" ]; then
			FILE_DIR=$NVME_FILE_DIR
		elif [ "$storage" == "ssd" ]; then
			FILE_DIR=$SSD_FILE_DIR
		else
			FILE_DIR=$HDD_FILE_DIR
		fi

		echo $FILE_DIR $iG file...

		./read_file.out $FILE_DIR/${i}Gdummyfile >> ./eval_log/${storage}_rand_read.log
		~/drop_cache.sh

	done
done
