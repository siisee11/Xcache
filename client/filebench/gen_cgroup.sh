mkdir -p /dev/cgroup/memory
mount -t cgroup -o memory memory /dev/cgroup/memory
mount | grep cgroup | grep memory

mkdir /dev/cgroup/memory/test_process

if [ "$1" == "4" ]; then
	echo 4294967296 > /dev/cgroup/memory/test_process/memory.limit_in_bytes    # 4GB

elif [ "$1" == "3" ]; then
	echo 3221225472 > /dev/cgroup/memory/test_process/memory.limit_in_bytes    # 3GB

elif [ "$1" == "2" ]; then
	echo 2147483648 > /dev/cgroup/memory/test_process/memory.limit_in_bytes    # 2GB
	
elif [ "$1" == "8" ]; then
	echo 8589934592 > /dev/cgroup/memory/test_process/memory.limit_in_bytes    # 8GB
	
else
	echo 1073741824 > /dev/cgroup/memory/test_process/memory.limit_in_bytes    # 1GB
fi

#echo 536870912 > /dev/cgroup/memory/test_process/memory.limit_in_bytes    # 0.5GB
#echo 18001 > /dev/cgroup/memory/test_process/tasks
