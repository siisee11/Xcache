# JULEE
Dynamic Memory Management for Disaggregated Transcendent Memory

## description

client: JULEE client kernel module

server: JULEE server userspace program

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
