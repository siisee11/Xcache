#!/bin/bash

sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
./read_file /test/$1Gdummyfile >> ./eval_log/$1G_rand_read.log
vim ./eval_log/$1G_rand_read.log
