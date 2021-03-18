# PMDFC
Persistent Memory Distributed File Cache

## history

6/8/2020 	migrated from jeopardy
27/10/2020 	Merge RDMA and TCP code

## description

client: pmdfc client kernel module
server: pmdfc server userspace program

This module implemented under linux kernel 5.3
Please update kernel over 5.3

## script.sh

run fio job on every vm.
$ ./script.sh all run

show fio output from every vm.
$ ./script.sh all out

If you want to run only one VM, then type
$ ./script.sh <VM_name>

## virsh.sh

reset all VM
$ ./virsh.sh all reset

## Reference

[In-kernel networking using tcp ip](https://github.com/abysamross/simple-linux-kernel-tcp-client-server)

[How to debug own module](https://namj.be/kgdb/2020-02-21-kgdb-module/)

