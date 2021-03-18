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
#else
#include <sys/socket.h>
#endif
#include <linux/inet.h>
#include <linux/in.h>


#define PORT 			(5432)
#define CLIENT_PORT 	(2346)
#define DEST_ADDR		("115.145.173.69")
#define CLIENT_ADDR		("10.0.2.15")

#define PMNET_MAX_PAYLOAD_BYTES  (8192 - sizeof(struct pmnet_msg))

/* same as hb delay, we're waiting for another node to recognize our hb */
#define PMNET_RECONNECT_DELAY_MS_DEFAULT	2000
#define PMNET_KEEPALIVE_DELAY_MS_DEFAULT	2000
#define PMNET_IDLE_TIMEOUT_MS_DEFAULT		30000
#define PMNET_TCP_USER_TIMEOUT			0x7fffffff

#define PMNET_MSG_HOLA 			(0)
#define PMNET_MSG_HOLASI		(1)
#define PMNET_MSG_ADIOS 		(2)
#define PMNET_MSG_PUTPAGE 		(3)
#define PMNET_MSG_SUCCESS		(4)
#define PMNET_MSG_GETPAGE 		(5)
#define PMNET_MSG_SENDPAGE 		(6)
#define PMNET_MSG_NOTEXIST		(7)
#define PMNET_MSG_INVALIDATE	(8)

#ifndef PAGE_SIZE
# define PAGE_SIZE 4096
#endif


struct pmnet_msg
{
	__be16 magic;
	__be16 data_len;
	__be16 msg_type;
	__be16 pad1;
	__be32 sys_status;
	__be32 status;
	__be32 key;
	__be32 msg_num;
	__be32 index;
	__u8  buf[0];
};


int pmnet_send_message(u32 msg_type, u32 key, u32, void *data, u32 len,
		       u8 target_node, int *status);
int pmnet_send_message_vec(u32 msg_type, u32 key, u32, struct kvec *vec,
			   size_t veclen, u8 target_node, int *status);
//int pmnet_send_recv_message_vec(u32, u32, u32, struct page* page,
//		struct kvec *caller_vec, size_t caller_veclen, u8 target_node, int *status);
int pmnet_send_recv_message_vec(u32, u32, u32, void* data,
		struct kvec *caller_vec, size_t caller_veclen, u8 target_node, int *status);
int pmnet_send_recv_message(u32 msg_type, u32 key, u32 index, void *, u32 len,
		u8 target_node, int *status);

struct pmnm_node;
int pmnet_start_listening(struct pmnm_node *node);
void pmnet_stop_listening(struct pmnm_node *node);
void pmnet_disconnect_node(struct pmnm_node *node);
void pmnet_fill_node_map(unsigned long *map, unsigned bytes);


int pmnet_init(void);
void pmnet_exit(void);

struct pmnet_send_tracking;
struct pmnet_sock_container;

#ifdef CONFIG_DEBUG_FS
void pmnet_debugfs_init(void);
void pmnet_debugfs_exit(void);
void pmnet_debug_add_nst(struct pmnet_send_tracking *nst);
void pmnet_debug_del_nst(struct pmnet_send_tracking *nst);
void pmnet_debug_add_sc(struct pmnet_sock_container *sc);
void pmnet_debug_del_sc(struct pmnet_sock_container *sc);
#else
static inline void pmnet_debugfs_init(void)
{
}
static inline void pmnet_debugfs_exit(void)
{
}
static inline void pmnet_debug_add_nst(struct pmnet_send_tracking *nst)
{
}
static inline void pmnet_debug_del_nst(struct pmnet_send_tracking *nst)
{
}
static inline void pmnet_debug_add_sc(struct pmnet_sock_container *sc)
{
}
static inline void pmnet_debug_del_sc(struct pmnet_sock_container *sc)
{
}
#endif	/* CONFIG_DEBUG_FS */



#endif /* PMNET_TCP_H */
