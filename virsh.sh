#!/bin/bash

if [[ $1 == "vm1" ]] ; then
	HOST=zombie1
elif [[ $1 == "vm2" ]] ; then
	HOST=zombie2
elif [[ $1 == "vm3" ]] ; then
	HOST=zombie3
elif [[ $1 == "all" ]]; then
	HOST="zombie1 zombie2 zombie3"
else
	HOST=$1
fi

COMMAND=${2}

for v in $HOST
do
	sudo virsh $COMMAND $v
done
