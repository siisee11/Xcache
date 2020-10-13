#!/bin/bash

avg_time() { 
	local -i n=$1
	local foo real sys user
	shift
	(($# > 0)) || return;
	{ read foo real; read foo user; read foo sys ;} < <(
		{ time -p for((;n--;)){ "$@" &>/dev/null ;} ;} 2>&1
	)
	printf "real: %.5f\nuser: %.5f\nsys : %.5f\n" $(
	bc -l <<<"$real/$n;$user/$n;$sys/$n;" )
}

test1() {
	echo 3 > /proc/sys/vm/drop_caches
	time python read_big_file.py
}


avg_time $1 test1
