# Server side code

It is userspace program saving page contents into Persistent Memory.
It communicate with clients kernel module (xCache client).

PM server use CCEH and hotring as page content storage.

## Hyperparameter
rdma_svr.h : NUM_QUEUES

## Requirement

C++ Boost library and include neeeded.

## How to run

```make``` to compile CCEH.
```./rdma_svr -t 7777``` to run server on port 7777

## Todos

statistic

## Timeline

2020/5/21: can communicate with pmdfc_client (HOLA, HOLASI).
2020/6/8: can communicate with pmdfc_client (PUT_PAGE, GET_PAGE, SUCCESS)
2020/6/19: support multi client
2020/6/22: deal with message processing using producer and consumer model
2020/7/1: refactor code
2021/03/18: RDMA support, Multi-client support

## reference

hotring (https://www.usenix.org/conference/fast20/presentation/chen-jiqiang)

CCEH
