#ifndef PMNET_TCP_INTERNAL_H
#define PMNET_TCP_INTERNAL_H

#define PMNET_MSG_MAGIC           ((u16)0xfa55)
#define PMNET_MSG_HOLA_MAGIC      ((u16)0xfa56)
#define PMNET_MSG_HOLASI_MAGIC    ((u16)0xfa57)
#define PMNET_MSG_STATUS_MAGIC    ((u16)0xfa58)
#define PMNET_MSG_KEEP_REQ_MAGIC  ((u16)0xfa59)
#define PMNET_MSG_KEEP_RESP_MAGIC ((u16)0xfa5a)

/* we're delaying our quorum decision so that heartbeat will have timed
 * out truly dead nodes by the time we come around to making decisions
 * on their number */
#define PMNET_QUORUM_DELAY_MS	((pmhb_dead_threshold + 2) * PMHB_REGION_TIMEOUT_MS)


struct pmnet_sock_container {
	struct kref			sc_kref;
	/* the next two are valid for the life time of the sc */
	struct socket		*sc_sock;
	struct pmnm_node	*sc_node;

	/* all of these sc work structs hold refs on the sc while they are
	 ** queued.  they should not be able to ref a freed sc.  the teardown
	 ** race is with pmnet_wq destruction in pmnet_stop_listening() */

	/* rx and connect work are generated from socket callbacks.  sc
	 ** shutdown removes the callbacks and then flushes the work queue */
	struct work_struct	sc_rx_work;
	struct work_struct	sc_connect_work;
	/* shutdown work is triggered in two ways.  the simple way is
	 ** for a code path calls ensure_shutdown which gets a lock, removes
	 ** the sc from the nn, and queues the work.  in this case the
	 ** work is single-shot.  the work is also queued from a sock
	 ** callback, though, and in this case the work will find the sc
	 ** still on the nn and will call ensure_shutdown itself.. this
	 ** ends up triggering the shutdown work again, though nothing
	 ** will be done in that second iteration.  so work queue teardown
	 ** has to be careful to remove the sc from the nn before waiting
	 ** on the work queue so that the shutdown work doesn't remove the
	 ** sc and rearm itself.
	 **/
	struct work_struct	sc_shutdown_work;

	struct timer_list	sc_idle_timeout;
	struct delayed_work	sc_keepalive_work;

	unsigned		sc_handshake_ok:1;

	/* This struct page stores basic info(like msg) from client */
	struct page 	*sc_page;

	/* This struct page stores cleancache page from client */
	struct page 	*sc_clean_page;
	size_t			sc_page_off;

	/* original handlers for the sockets */
	void			(*sc_state_change)(struct sock *sk);
	void			(*sc_data_ready)(struct sock *sk);

	u32			sc_msg_key;
	u16			sc_msg_type;
	struct mutex		sc_send_lock;
};

struct pmnet_handshake {
	__be64	protocol_version;
	__be64	connector_id;
	__be32  pmhb_heartbeat_timeout_ms;
	__be32  pmnet_idle_timeout_ms;
	__be32  pmnet_keepalive_delay_ms;
	__be32  pmnet_reconnect_delay_ms;
};

struct pmnet_node {
	/* this is never called from int/bh */
	spinlock_t			nn_lock;

	/* set the moment an sc is allocated and a connect is started */
	struct pmnet_sock_container	*nn_sc;
	/* _valid is only set after the handshake passes and tx can happen */
	unsigned			nn_sc_valid:1;
	/* if this is set tx just returns it */
	int				nn_persistent_error;
	/* It is only set to 1 after the idle time out. */
	atomic_t			nn_timeout;

	/* threads waiting for an sc to arrive wait on the wq for generation
	 * to increase.  it is increased when a connecting socket succeeds
	 * or fails or when an accepted socket is attached. */
	wait_queue_head_t		nn_sc_wq;

	struct idr			nn_status_idr;
	struct list_head		nn_status_list;

	/* connects are attempted from when heartbeat comes up until either hb
	 * goes down, the node is unconfigured, or a connect succeeds.
	 * connect_work is queued from set_nn_state both from hb up and from
	 * itself if a connect attempt fails and so can be self-arming.
	 * shutdown is careful to first mark the nn such that no connects will
	 * be attempted before canceling delayed connect work and flushing the
	 * queue. */
	struct delayed_work		nn_connect_work;
	unsigned long			nn_last_connect_attempt;

	/* this is queued as nodes come up and is canceled when a connection is
	 * established.  this expiring gives up on the node and errors out
	 * transmits */
	struct delayed_work		nn_connect_expired;

	/* after we give up on a socket we wait a while before deciding
	 * that it is still heartbeating and that we should do some
	 * quorum work */
	struct delayed_work		nn_still_up;
};

struct pmnet_msg_handler {
	struct rb_node		nh_node;
	u32			nh_max_len;
	u32			nh_msg_type;
	u32			nh_key;
//	pmnet_msg_handler_func	*nh_func;
	void			*nh_func_data;
//	pmnet_post_msg_handler_func
//				*nh_post_func;
	struct kref		nh_kref;
	struct list_head	nh_unregister_item;
};

enum pmnet_system_error {
	PMNET_ERR_NONE = 0,
	PMNET_ERR_NO_HNDLR,
	PMNET_ERR_OVERFLOW,
	PMNET_ERR_DIED,
	PMNET_ERR_MAX
};

struct pmnet_status_wait {
	enum pmnet_system_error	ns_sys_status;
	s32			ns_status;
	int			ns_id;
	wait_queue_head_t	ns_wq;
	struct list_head	ns_node_item;
};

#endif /* PMNET_TCP_INTERNAL_H */
