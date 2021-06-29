# Server side code

It is userspace program saving page contents into Persistent Memory.
It communicate with clients kernel module (Xcache client).

PM server use CCEH and hotring as page content storage.

## Directory Structure
- src/ : various hashtables (CuckCoo, CCEH, LinearProbing, etc.)

- util/ : Type definition, filter, hash fucntions, etc.)

## Defines
NORMALPUT : PUT requires only one RTT, but memcpy overhead engaged.

NORMALGET : GET requires 2RTT and also memcpy.

BIGMRPUT  : No memcpy overhead but 2RTTs.

BIGMRGET  : No memcpy overhead with 1RTT.

CBLOOMFILTER : Client side bloomfilter. Have to send server side bloomfilter to client to sync.

## Hyperparameter
(Have to sync with client)

rdma_svr.h : NUM_QUEUES

rdma_svr.h : NUM_HASH

rdma_svr.h : BF_SIZE   (Bloom Filter Size)

rdma_svr.h : BUFFER_SIZE   (Buffer Size)

## Requirement
~~C++ Boost library and include neeeded.~~

## How to Compile
```cd build && cmake .. && make```

or ```cd build && cmake .. -G Ninja && ninja``` to use ninja as builder

or simply use Makefile ```make```

## How to run

```make LinearProbing``` to compile.

```./rdma_svr -t 7777``` to run server on port 7777

```./rdma_svr -t 7777 -b``` to use bloomfilter 

## KV testing
```make LinearProbing``` to compile.

```./kv_linear -W 10-19 -d /dataset/input_sort.txt -n 10000000 -v -h -b```

## BF testing
```
g++ bftest.cpp -lssl -lcrypto -I./ -g
./a.out
```

## Todos
- [ ] Support CMAKE
- [ ] increase hash_func size (util.h)
- [x] Big Bloomfilter
- [x] statistic
- [ ] Free space management

## Q&A
### Q. Local protection error (err 4) occurs when many pages processed.
A. Increase BUFFER_SIZE.

### Q. Client rdmatest reports failed Search.
A. It is not error. Increasing server buffer size would make it correct.

## Timeline

2020/5/21: can communicate with pmdfc_client (HOLA, HOLASI).

2020/6/8: can communicate with pmdfc_client (PUT_PAGE, GET_PAGE, SUCCESS)

2020/6/19: support multi client

2020/6/22: deal with message processing using producer and consumer model

2020/7/1: refactor code

2021/03/18: Support RDMA, Multi-client

2021/05/20: Support various data structure (LinearProbing, CCEH, CuckCoo, ...)

2021/06/10: RDMA integrated Bloom Filter added.


## reference

hotring (https://www.usenix.org/conference/fast20/presentation/chen-jiqiang)

CCEH (https://github.com/DICL/CCEH)
