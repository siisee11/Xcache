#ifndef _JULEE_H_
#define _JULEE_H_

#include <linux/hashtable.h>
#include <linux/atomic.h>

enum{
	MSG_WRITE_REQUEST,
	MSG_WRITE_REQUEST_REPLY,
	MSG_WRITE,
	MSG_WRITE_REPLY,
	MSG_READ_REQUEST,
	MSG_READ_REQUEST_REPLY,
	MSG_READ,
	MSG_READ_REPLY
};

/* server TX messages */
enum{				
	TX_WRITE_BEGIN,
	TX_WRITE_READY,
	TX_WRITE_COMMITTED,		
	TX_WRITE_ABORTED,
	TX_READ_BEGIN,
	TX_READ_READY,
	TX_READ_COMMITTED,
	TX_READ_ABORTED,
};


struct ht_data {
    u64 longkey;
    u64 roffset;
    struct hlist_node h_node;
};

#endif
