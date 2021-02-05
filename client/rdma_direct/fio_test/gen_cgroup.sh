mkdir -p /dev/cgroup/memory
mount -t cgroup -o memory memory /dev/cgroup/memory
mount | grep cgroup | grep memory

mkdir /dev/cgroup/memory/test_process
echo 4000000000 > /dev/cgroup/memory/test_process/memory.limit_in_bytes

#echo 18001 > /dev/cgroup/memory/test_process/tasks


