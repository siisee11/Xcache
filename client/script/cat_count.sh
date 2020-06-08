#bin/bash!

watch -n 1 '
echo "succ_gets : ";
cat /sys/kernel/debug/cleancache/succ_gets;
echo "failed_gets : ";
cat /sys/kernel/debug/cleancache/failed_gets;
echo "puts : ";
cat /sys/kernel/debug/cleancache/puts;
echo "invalidates : ";
cat /sys/kernel/debug/cleancache/invalidates;
'

