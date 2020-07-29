#ifndef PMNET_TCP_INTERNAL_H
#define PMNET_TCP_INTERNAL_H

#define PMNET_MSG_MAGIC           ((uint16_t)0xfa55)
#define PMNET_MSG_HOLA_MAGIC      ((uint16_t)0xfa56)
#define PMNET_MSG_HOLASI_MAGIC    ((uint16_t)0xfa57)
#define PMNET_MSG_STATUS_MAGIC    ((uint16_t)0xfa58)
#define PMNET_MSG_KEEP_REQ_MAGIC  ((uint16_t)0xfa59)
#define PMNET_MSG_KEEP_RESP_MAGIC ((uint16_t)0xfa5a)

#define PMNET_PROTOCOL_VERSION 11ULL

/* we're delaying our quorum decision so that heartbeat will have timed
 * out truly dead nodes by the time we come around to making decisions
 * on their number */
//#define PMNET_QUORUM_DELAY_MS	((pmhb_dead_threshold + 2) * PMHB_REGION_TIMEOUT_MS)

struct pmnet_handshake {
	uint64_t protocol_version;
	uint64_t connector_id;
	uint32_t pmhb_heartbeat_timeout_ms;
	uint32_t pmnet_idle_timeout_ms;
	uint32_t pmnet_keepalive_delay_ms;
	uint32_t pmnet_reconnect_delay_ms;
};

struct pmnet_sock_container {
	int 			sockfd;
	unsigned		sc_handshake_ok:1;

//	u32			sc_msg_key;
//	u16			sc_msg_type;
//	struct mutex		sc_send_lock;
};

enum pmnet_system_error {
	PMNET_ERR_NONE = 0,
	PMNET_ERR_NO_HNDLR,
	PMNET_ERR_OVERFLOW,
	PMNET_ERR_DIED,
	PMNET_ERR_MAX
};

#endif /* PMNET_TCP_INTERNAL_H */
