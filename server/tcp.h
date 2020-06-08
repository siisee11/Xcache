/*
 * tcp.h
 *
 * In kernel networking
 *
 * Copyright (c) 2019, Jaeyoun Nam, SKKU.
 */

#ifndef PMNET_TCP_H
#define PMNET_TPC_H

#include <linux/socket.h>
#ifdef __KERNEL__
#include <net/sock.h>
#include <linux/tcp.h>
#include <linux/inet.h>
#include <linux/in.h>
#else
#include <sys/socket.h>
#endif


#define PORT 			(2325)
#define CLIENT_PORT 	(2346)
#define DEST_ADDR		("115.145.173.67")
#define CLIENT_ADDR		("10.0.2.15")

#define PMNET_MAX_PAYLOAD_BYTES  (8192 - sizeof(struct pmnet_msg))

/* same as hb delay, we're waiting for another node to recognize our hb */
#define PMNET_RECONNECT_DELAY_MS_DEFAULT	2000
#define PMNET_KEEPALIVE_DELAY_MS_DEFAULT	2000
#define PMNET_IDLE_TIMEOUT_MS_DEFAULT		30000
#define PMNET_TCP_USER_TIMEOUT			0x7fffffff

#define PMNET_MSG_HOLA 		0
#define PMNET_MSG_HOLASI	1
#define PMNET_MSG_ADIOS 	2
#define PMNET_MSG_PUTPAGE 	3
#define PMNET_MSG_SUCCESS 	4
#define PMNET_MSG_GETPAGE 	5
#define PMNET_MSG_SENDPAGE 	6

#ifndef PAGE_SIZE
# define PAGE_SIZE 4096
#endif

#if 0
enum {
	PMNET_MSG_HOLA = 0,
	PMNET_MSG_HOLASI,
	PMNET_MSG_ADIOS,
	PMNET_MSG_PUTPAGE,
	PMNET_MSG_GETPAGE,
	PMNET_MSG_SENDPAGE,
};
#endif

struct pmnet_msg
{
	uint16_t magic;
	uint16_t data_len;
	uint16_t msg_type;
	uint16_t pad1;
	uint32_t sys_status;
	uint32_t status;
	uint32_t key;
//	uint32_t msg_num;
	uint32_t index;
	uint8_t  buf[0];
};

static unsigned int inet_addr(const char *str)
{
	int a,b,c,d;
	char arr[4];
	sscanf(str,"%d.%d.%d.%d",&a,&b,&c,&d);
	arr[0] = a; arr[1] = b; arr[2] = c; arr[3] = d;
	return *(unsigned int*)arr;
}

#endif /* PMNET_TCP_H */
