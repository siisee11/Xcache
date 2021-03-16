#!/bin/bash

VM="zombie1 zombie2 zombie3"

if [[ $1 == "all" ]]; then
	if [[ $2 == "run" ]]; then
		for v in $VM
		do
			ssh siisee11@$v 'bash -s' <<- 'ENDSSH'
				# commands to run on remote host 
				hostname
				echo "prepare to run"
				sudo mount -t nfs 192.168.122.1:/nfs /nfs
				cd /nfs/Xcache/client/rdma_direct
				sudo make
				sudo make pmdfc_client
			ENDSSH

		done

		for v in $VM
		do
			ssh siisee11@$v vm=$v 'bash -s' <<- 'ENDSSH'
				cd /nfs/Xcache/client/rdma_direct/fio_test
				sudo ./gen_cgroup.sh
				sudo ./run_cgroup_fio.sh > out_${vm} &
			ENDSSH
		done

	elif [[ $2 == "out" ]]; then
		for v in $VM
		do
			ssh siisee11@$v vm=$v 'bash -s' <<- 'ENDSSH'
				cd /nfs/Xcache/client/rdma_direct/fio_test
				hostname
				cat out_${vm}
			ENDSSH
		done
	fi	

else
	ssh siisee11@$1 'bash -s' <<- 'ENDSSH'
		# commands to run on remote host 
		hostname
		echo "prepare to run"
		sudo mount -t nfs 192.168.122.1:/nfs /nfs
		cd /nfs/Xcache/client/rdma_direct
		sudo make
		sudo make pmdfc_client
		cd fio_test/
		sudo ./gen_cgroup.sh
		sudo ./run_cgroup_fio.sh > out
		hostname
		cat out
	ENDSSH
fi

