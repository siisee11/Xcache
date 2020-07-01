# Server side code

It is userspace program saving page contents into Persistent Memory.

It communicate with clients kernel module (pmdfc_client).

PM server use CCEH and hotring as page content storage.


## Requirement

C++ Boost library and include neeeded.

## How to run

```make``` to compile CCEH.

```./a.sh```  to compile server.cpp

```./server ./jy/FILE``` to run server.


## Timeline

2020/5/21: can communicate with pmdfc_client (HOLA, HOLASI).
2020/6/8: can communicate with pmdfc_client (PUT_PAGE, GET_PAGE, SUCCESS)
2020/6/19: support multi client
2020/6/22: deal with message processing using producer and consumer model
2020/7/1: refactor code

## reference

hotring (https://www.usenix.org/conference/fast20/presentation/chen-jiqiang)

CCEH
