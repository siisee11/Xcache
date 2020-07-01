#!/bin/bash

log_mask="/sys/fs/pmcb/logmask"
for node in ENTRY EXIT TCP MSG SOCKET ERROR NOTICE; do
 echo allow >"$log_mask"/"$node"
done
