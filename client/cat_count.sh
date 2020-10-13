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

echo "total_get: ";
cat /sys/kernel/debug/pmdfc/total_gets;
echo "actual_get: ";
cat /sys/kernel/debug/pmdfc/actual_gets;
echo "miss_get: ";
cat /sys/kernel/debug/pmdfc/miss_gets;
echo "hit_get: ";
cat /sys/kernel/debug/pmdfc/hit_gets;
echo "dropped_puts: ";
cat /sys/kernel/debug/pmdfc/drop_puts;

cat /sys/kernel/debug/pmnet/*;
'

