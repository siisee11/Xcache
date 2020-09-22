#ifndef SERVER_H
#define SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include <time.h>
#include <signal.h>
#include <stddef.h>


//#define DEBUG
#ifdef DEBUG
#define dprintf(...) do{ fprintf(stderr, __VA_ARGS__); fflush(stdout);} while(0)
#else
#define dprintf(...)
#endif

void init_server();
int establish_conn();
int test();

#endif

