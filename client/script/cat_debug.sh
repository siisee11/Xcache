#bin/bash!

watch -n 0.1 '
cat /sys/kernel/debug/pmnet/send_tracking;
'

