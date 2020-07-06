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
//#define PMNET_MAX_PAYLOAD_BYTES  1000000

/* same as hb delay, we're waiting for another node to recognize our hb */
#define PMNET_RECONNECT_DELAY_MS_DEFAULT	2000
#define PMNET_KEEPALIVE_DELAY_MS_DEFAULT	2000
#define PMNET_IDLE_TIMEOUT_MS_DEFAULT		30000
#define PMNET_TCP_USER_TIMEOUT			0x7fffffff

#define PMNET_MSG_HOLA 			0
#define PMNET_MSG_HOLASI		1
#define PMNET_MSG_ADIOS 		2
#define PMNET_MSG_PUTPAGE 		3
#define PMNET_MSG_SUCCESS 		4
#define PMNET_MSG_GETPAGE 		5
#define PMNET_MSG_SENDPAGE 		6
#define PMNET_MSG_NOTEXIST 		7
#define PMNET_MSG_INVALIDATE 	8

#ifndef PAGE_SIZE
# define PAGE_SIZE 4096
#endif

struct pmnet_handshake {
	uint64_t protocol_version;
	uint64_t connector_id;
	uint32_t pmhb_heartbeat_timeout_ms;
	uint32_t pmnet_idle_timeout_ms;
	uint32_t pmnet_keepalive_delay_ms;
	uint32_t pmnet_reconnect_delay_ms;
};


struct pmnet_msg
{
	uint16_t magic;
	uint16_t data_len;
	uint16_t msg_type;
	uint16_t pad1;
	uint32_t sys_status;
	uint32_t status;
	uint32_t key;
	uint32_t msg_num;
	uint32_t index;
	uint8_t  buf[0];
};

#endif /* PMNET_TCP_H */
