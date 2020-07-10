#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/net.h>
#include <linux/export.h>
#include <linux/sched/mm.h>
#include <linux/delay.h>
#include <net/tcp.h>

#include <asm/uaccess.h>

#include "tcp.h"
#include "nodemanager.h"
#define MLOG_MASK_PREFIX ML_TCP
#include "masklog.h"

#include "tcp_internal.h"

#define SC_NODEF_FMT "node %s (num %u) at %pI4:%u"
#define SC_NODEF_ARGS(sc) sc->sc_node->nd_name, sc->sc_node->nd_num,	\
	&sc->sc_node->nd_ipv4_address,		\
	ntohs(sc->sc_node->nd_ipv4_port)

#define msglog(hdr, fmt, args...) do {					\
	typeof(hdr) __hdr = (hdr);					\
	mlog(ML_MSG, "[mag %u len %u typ %u stat %d sys_stat %d "	\
	     "key %08x num %u] " fmt,					\
	     be16_to_cpu(__hdr->magic), be16_to_cpu(__hdr->data_len), 	\
	     be16_to_cpu(__hdr->msg_type), be32_to_cpu(__hdr->status),	\
	     be32_to_cpu(__hdr->sys_status), be32_to_cpu(__hdr->key),	\
	     be32_to_cpu(__hdr->msg_num) ,  ##args);			\
} while (0)

#define sclog(sc, fmt, args...) do {					\
	typeof(sc) __sc = (sc);						\
	mlog(ML_SOCKET, "[sc %p refs %d sock %p node %u page %p "	\
	     "pg_off %zu] " fmt, __sc,					\
	     kref_read(&__sc->sc_kref), __sc->sc_sock,	\
	    __sc->sc_node->nd_num, __sc->sc_page, __sc->sc_page_off ,	\
	    ##args);							\
} while (0)

/* struct workqueue */
static struct workqueue_struct *pmnet_wq;

/* PMNET nodes */
static struct pmnet_node pmnet_nodes[PMNM_MAX_NODES];

static struct pmnet_handshake *pmnet_hand;
static struct pmnet_msg *pmnet_keep_req, *pmnet_keep_resp;

static int pmnet_sys_err_translations[PMNET_ERR_MAX] =
		{[PMNET_ERR_NONE]	= 0,
		 [PMNET_ERR_NO_HNDLR]	= -ENOPROTOOPT,
		 [PMNET_ERR_OVERFLOW]	= -EOVERFLOW,
		 [PMNET_ERR_DIED]	= -EHOSTDOWN,};

/* can't quite avoid *all* internal declarations :/ */
static void pmnet_sc_connect_completed(struct work_struct *work);
static void pmnet_rx_until_empty(struct work_struct *work);
static void pmnet_shutdown_sc(struct work_struct *work);
static void pmnet_listen_data_ready(struct sock *sk);
static void pmnet_sc_send_keep_req(struct work_struct *work);
static void pmnet_idle_timer(struct timer_list *t);
static void pmnet_sc_postpone_idle(struct pmnet_sock_container *sc);
static void pmnet_sc_reset_idle_timer(struct pmnet_sock_container *sc);

#ifdef CONFIG_DEBUG_FS
static void pmnet_init_nst(struct pmnet_send_tracking *nst, u32 msgtype,
			   u32 msgkey, struct task_struct *task, u8 node)
{
	INIT_LIST_HEAD(&nst->st_net_debug_item);
	nst->st_task = task;
	nst->st_msg_type = msgtype;
	nst->st_msg_key = msgkey;
	nst->st_node = node;
}

static inline void pmnet_set_nst_sock_time(struct pmnet_send_tracking *nst)
{
	nst->st_sock_time = ktime_get();
}

static inline void pmnet_set_nst_send_time(struct pmnet_send_tracking *nst)
{
	nst->st_send_time = ktime_get();
}

static inline void pmnet_set_nst_status_time(struct pmnet_send_tracking *nst)
{
	nst->st_status_time = ktime_get();
}

static inline void pmnet_set_nst_sock_container(struct pmnet_send_tracking *nst,
						struct pmnet_sock_container *sc)
{
	nst->st_sc = sc;
}

static inline void pmnet_set_nst_msg_id(struct pmnet_send_tracking *nst,
					u32 msg_id)
{
	nst->st_id = msg_id;
}

static inline void pmnet_set_sock_timer(struct pmnet_sock_container *sc)
{
	sc->sc_tv_timer = ktime_get();
}

static inline void pmnet_set_data_ready_time(struct pmnet_sock_container *sc)
{
	sc->sc_tv_data_ready = ktime_get();
}

static inline void pmnet_set_advance_start_time(struct pmnet_sock_container *sc)
{
	sc->sc_tv_advance_start = ktime_get();
}

static inline void pmnet_set_advance_stop_time(struct pmnet_sock_container *sc)
{
	sc->sc_tv_advance_stop = ktime_get();
}

static inline void pmnet_set_func_start_time(struct pmnet_sock_container *sc)
{
	sc->sc_tv_func_start = ktime_get();
}

static inline void pmnet_set_func_stop_time(struct pmnet_sock_container *sc)
{
	sc->sc_tv_func_stop = ktime_get();
}

#else  /* CONFIG_DEBUG_FS */
# define pmnet_init_nst(a, b, c, d, e)
# define pmnet_set_nst_sock_time(a)
# define pmnet_set_nst_send_time(a)
# define pmnet_set_nst_status_time(a)
# define pmnet_set_nst_sock_container(a, b)
# define pmnet_set_nst_msg_id(a, b)
# define pmnet_set_sock_timer(a)
# define pmnet_set_data_ready_time(a)
# define pmnet_set_advance_start_time(a)
# define pmnet_set_advance_stop_time(a)
# define pmnet_set_func_start_time(a)
# define pmnet_set_func_stop_time(a)
#endif /* CONFIG_DEBUG_FS */

#define CONFIG_PMDFC_FS_STATS
#ifdef CONFIG_PMDFC_FS_STATS
#if 0
static ktime_t pmnet_get_func_run_time(struct pmnet_sock_container *sc)
{
	return ktime_sub(sc->sc_tv_func_stop, sc->sc_tv_func_start);
}
#endif

static void pmnet_update_send_stats(struct pmnet_send_tracking *nst,
				    struct pmnet_sock_container *sc)
{
	sc->sc_tv_status_total = ktime_add(sc->sc_tv_status_total,
					   ktime_sub(ktime_get(),
						     nst->st_status_time));
	sc->sc_tv_send_total = ktime_add(sc->sc_tv_send_total,
					 ktime_sub(nst->st_status_time,
						   nst->st_send_time));
	sc->sc_tv_acquiry_total = ktime_add(sc->sc_tv_acquiry_total,
					    ktime_sub(nst->st_send_time,
						      nst->st_sock_time));
	sc->sc_send_count++;
}

#if 0
static void pmnet_update_recv_stats(struct pmnet_sock_container *sc)
{
	sc->sc_tv_process_total = ktime_add(sc->sc_tv_process_total,
					    pmnet_get_func_run_time(sc));
	sc->sc_recv_count++;
}
#endif

#else

# define pmnet_update_send_stats(a, b)

# define pmnet_update_recv_stats(sc)

#endif /* CONFIG_PMDFC_FS_STATS */


static inline unsigned int pmnet_reconnect_delay(void)
{
	return pmnm_single_cluster->cl_reconnect_delay_ms;
}

static inline unsigned int pmnet_keepalive_delay(void)
{
	return pmnm_single_cluster->cl_keepalive_delay_ms;
}

static inline unsigned int pmnet_idle_timeout(void)
{
	return pmnm_single_cluster->cl_idle_timeout_ms;
}

static inline int pmnet_sys_err_to_errno(enum pmnet_system_error err)
{
	int trans;
	BUG_ON(err >= PMNET_ERR_MAX);
	trans = pmnet_sys_err_translations[err];

	/* Just in case we mess up the translation table above */
	BUG_ON(err != PMNET_ERR_NONE && trans == 0);
	return trans;
}

/* get pmnet_node by number */
static struct pmnet_node * pmnet_nn_from_num(u8 node_num)
{
	BUG_ON(node_num >= ARRAY_SIZE(pmnet_nodes));
	return &pmnet_nodes[node_num];
}

static u8 pmnet_num_from_nn(struct pmnet_node *nn)
{
	BUG_ON(nn == NULL);
	return nn - pmnet_nodes;
}

static int pmnet_prep_nsw(struct pmnet_node *nn, struct pmnet_status_wait *nsw)
{
	int ret;

	spin_lock(&nn->nn_lock);
	ret = idr_alloc(&nn->nn_status_idr, nsw, 0, 0, GFP_ATOMIC);
	if (ret >= 0) {
		nsw->ns_id = ret;
		list_add_tail(&nsw->ns_node_item, &nn->nn_status_list);
	}
	spin_unlock(&nn->nn_lock);
	if (ret < 0)
		return ret;

	init_waitqueue_head(&nsw->ns_wq);
	nsw->ns_sys_status = PMNET_ERR_NONE;
	nsw->ns_status = 0;
	return 0;
}

static void pmnet_complete_nsw_locked(struct pmnet_node *nn,
				      struct pmnet_status_wait *nsw,
				      enum pmnet_system_error sys_status,
				      s32 status)
{
	assert_spin_locked(&nn->nn_lock);

	if (!list_empty(&nsw->ns_node_item)) {
		list_del_init(&nsw->ns_node_item);
		nsw->ns_sys_status = sys_status;
		nsw->ns_status = status;
		idr_remove(&nn->nn_status_idr, nsw->ns_id);
		wake_up(&nsw->ns_wq);
	}
}

static void pmnet_complete_nsw(struct pmnet_node *nn,
			       struct pmnet_status_wait *nsw,
			       u64 id, enum pmnet_system_error sys_status,
			       s32 status)
{
	spin_lock(&nn->nn_lock);
	if (nsw == NULL) {
		if (id > INT_MAX)
			goto out;

		nsw = idr_find(&nn->nn_status_idr, id);
		if (nsw == NULL)
			goto out;
	}

	pmnet_complete_nsw_locked(nn, nsw, sys_status, status);

out:
	spin_unlock(&nn->nn_lock);
	return;
}

static void pmnet_complete_nodes_nsw(struct pmnet_node *nn)
{
	struct pmnet_status_wait *nsw, *tmp;
	unsigned int num_kills = 0;

	assert_spin_locked(&nn->nn_lock);

	list_for_each_entry_safe(nsw, tmp, &nn->nn_status_list, ns_node_item) {
		pmnet_complete_nsw_locked(nn, nsw, PMNET_ERR_DIED, 0);
		num_kills++;
	}

	mlog(0, "completed %d messages for node %u\n", num_kills,
	     pmnet_num_from_nn(nn));
}

static int pmnet_nsw_completed(struct pmnet_node *nn,
			       struct pmnet_status_wait *nsw)
{
	int completed;
	spin_lock(&nn->nn_lock);
	completed = list_empty(&nsw->ns_node_item);
	spin_unlock(&nn->nn_lock);
	return completed;
}


/*
 * This callback function would be called
 * if kref count meets zero
 */
static void sc_kref_release(struct kref *kref)
{
	struct pmnet_sock_container *sc = container_of(kref,
			struct pmnet_sock_container, sc_kref);

	printk(KERN_NOTICE "releasing sc.....\n");

	if (sc->sc_sock) {
		sock_release(sc->sc_sock);
		sc->sc_sock = NULL;
	}

//	pmnm_node_put(sc->sc_node);
	sc->sc_node = NULL;

	if (sc->sc_page)
		__free_page(sc->sc_page);

	kfree(sc);
}

static void sc_put(struct pmnet_sock_container *sc)
{
	if (sc->sc_node->nd_num == 0)
		sclog(sc, "put\n");
	kref_put(&sc->sc_kref, sc_kref_release);
}
static void sc_get(struct pmnet_sock_container *sc)
{
	if (sc->sc_node->nd_num == 0)
		sclog(sc, "get\n");
	kref_get(&sc->sc_kref);
}

static void pmnet_sc_queue_work(struct pmnet_sock_container *sc,
				struct work_struct *work)
{
	sc_get(sc);
	if (!queue_work(pmnet_wq, work))
		sc_put(sc);
}
static void pmnet_sc_queue_delayed_work(struct pmnet_sock_container *sc,
					struct delayed_work *work,
					int delay)
{
	sc_get(sc);
	if (!queue_delayed_work(pmnet_wq, work, delay))
		sc_put(sc);
}
static void pmnet_sc_cancel_delayed_work(struct pmnet_sock_container *sc,
					 struct delayed_work *work)
{
	if (cancel_delayed_work(work))
		sc_put(sc);
}

/* ------------------------------------------------------------ */

static struct pmnet_sock_container *sc_alloc(struct pmnm_node *node)
{
	struct pmnet_sock_container *sc, *ret = NULL;
	struct page *page = NULL;
	struct page *clean_page = NULL;

	page = alloc_page(GFP_NOFS);
	sc = kzalloc(sizeof(*sc), GFP_NOFS);
	if (sc == NULL || page == NULL)
		goto out;

	kref_init(&sc->sc_kref); 	/* sc_kref initialized to 1 */
	sc->sc_node = node;

	INIT_WORK(&sc->sc_connect_work, pmnet_sc_connect_completed);
	INIT_WORK(&sc->sc_rx_work, pmnet_rx_until_empty);
	INIT_WORK(&sc->sc_shutdown_work, pmnet_shutdown_sc);
	INIT_DELAYED_WORK(&sc->sc_keepalive_work, pmnet_sc_send_keep_req);

	timer_setup(&sc->sc_idle_timeout, pmnet_idle_timer, 0);

	ret = sc;
	sc->sc_page = page;
	pmnet_debug_add_sc(sc);
	sc = NULL;
	page = NULL;

out:
	if (page)
		__free_page(page);
	if (clean_page)
		__free_page(clean_page);
	kfree(sc);

	return ret;
}

/* ----------------------------------------------------------- */
static void pmnet_set_nn_state(struct pmnet_node *nn,
			       struct pmnet_sock_container *sc,
			       unsigned valid, int err)
{
	int was_valid = nn->nn_sc_valid;
	int was_err = nn->nn_persistent_error;
	struct pmnet_sock_container *old_sc = nn->nn_sc;

	assert_spin_locked(&nn->nn_lock);

	if (was_valid && !valid && err == 0)
		err = -ENOTCONN;

	BUG_ON(sc && nn->nn_sc && nn->nn_sc != sc);

	if (was_valid && !valid && err == 0)
		err = -ENOTCONN;


	pr_info("node %u sc %p -> %p, valid %u -> %u, err %d -> %d\n",
	     pmnet_num_from_nn(nn), nn->nn_sc, sc, nn->nn_sc_valid, valid,
	     nn->nn_persistent_error, err);

	nn->nn_sc = sc;
	nn->nn_sc_valid = valid ? 1 : 0;
	nn->nn_persistent_error = err;

	/* mirrors pmnet_tx_can_proceed() */
	if (nn->nn_persistent_error || nn->nn_sc_valid)
		wake_up(&nn->nn_sc_wq);

	if (was_valid && !was_err && nn->nn_persistent_error) {
//		o2quo_conn_err(pmnet_num_from_nn(nn));
//		queue_delayed_work(pmnet_wq, &nn->nn_still_up,
//				   msecs_to_jiffies(PMNET_QUORUM_DELAY_MS));
	}

	if (was_valid && !valid) {
		if (old_sc)
			pr_info("pmnet: No longer connected to "
				SC_NODEF_FMT "\n", SC_NODEF_ARGS(old_sc));
		pmnet_complete_nodes_nsw(nn);
	}

	if (!was_valid && valid) {
//		o2quo_conn_up(pmnet_num_from_nn(nn));
		cancel_delayed_work(&nn->nn_connect_expired);
		pr_info("pmnet: %s " SC_NODEF_FMT "\n",
		       "Connected to" ,
		       SC_NODEF_ARGS(sc));
	}

	/* trigger the connecting worker func as long as we're not valid,
	 * it will back off if it shouldn't connect.  This can be called
	 * from node config teardown and so needs to be careful about
	 * the work queue actually being up. */
	if (!valid && pmnet_wq) {
#if 0
		unsigned long delay = 0;
		/* delay if we're within a RECONNECT_DELAY of the
		 * last attempt */
		delay = (nn->nn_last_connect_attempt +
			 msecs_to_jiffies(pmnet_reconnect_delay()))
			- jiffies;
		if (delay > msecs_to_jiffies(pmnet_reconnect_delay()))
			delay = 0;
		mlog(ML_CONN, "queueing conn attempt in %lu jiffies\n", delay);
		queue_delayed_work(pmnet_wq, &nn->nn_connect_work, delay);

		/*
		 * Delay the expired work after idle timeout.
		 *
		 * We might have lots of failed connection attempts that run
		 * through here but we only cancel the connect_expired work when
		 * a connection attempt succeeds.  So only the first enqueue of
		 * the connect_expired work will do anything.  The rest will see
		 * that it's already queued and do nothing.
		 */
		delay += msecs_to_jiffies(pmnet_idle_timeout());
		queue_delayed_work(pmnet_wq, &nn->nn_connect_expired, delay);
#endif
	}

	/* keep track of the nn's sc ref for the caller */
	if ((old_sc == NULL) && sc)
		sc_get(sc);
	if (old_sc && (old_sc != sc)) {
		pmnet_sc_queue_work(old_sc, &old_sc->sc_shutdown_work);
		sc_put(old_sc);
	}
}

/* see pmnet_register_callbacks() */
static void pmnet_data_ready(struct sock *sk)
{
	void (*ready)(struct sock *sk);
	struct pmnet_sock_container *sc;

	read_lock_bh(&sk->sk_callback_lock);
	sc = sk->sk_user_data;
	if (sc) {
		sclog(sc, "data_ready hit\n");
		pmnet_set_data_ready_time(sc);
		pmnet_sc_queue_work(sc, &sc->sc_rx_work);
		ready = sc->sc_data_ready;
	} else {
		ready = sk->sk_data_ready;
	}
	read_unlock_bh(&sk->sk_callback_lock);

	ready(sk);
}

/* see pmnet_register_callbacks() */
static void pmnet_state_change(struct sock *sk)
{
	void (*state_change)(struct sock *sk);
	struct pmnet_sock_container *sc;

	read_lock_bh(&sk->sk_callback_lock);
	sc = sk->sk_user_data;
	if (sc == NULL) {
		state_change = sk->sk_state_change;
		goto out;
	}

	sclog(sc, "state_change to %d\n", sk->sk_state);

	state_change = sc->sc_state_change;

	switch(sk->sk_state) {
	/* ignore connecting sockets as they make progress */
	case TCP_SYN_SENT:
	case TCP_SYN_RECV:
		break;
	case TCP_ESTABLISHED:
		pmnet_sc_queue_work(sc, &sc->sc_connect_work);
		break;
	default:
		printk(KERN_INFO "pmnet: Connection to " SC_NODEF_FMT
			" shutdown, state %d\n",
			SC_NODEF_ARGS(sc), sk->sk_state);
		pmnet_sc_queue_work(sc, &sc->sc_shutdown_work);
		break;
	}
out:
	read_unlock_bh(&sk->sk_callback_lock);
	state_change(sk);
}


/*
 * we register callbacks so we can queue work on events before calling
 * the original callbacks.  our callbacks our careful to test user_data
 * to discover when they've reaced with pmnet_unregister_callbacks().
 */
static void pmnet_register_callbacks(struct sock *sk,
		struct pmnet_sock_container *sc)
{
	write_lock_bh(&sk->sk_callback_lock);

	/* accepted sockets inherit the old listen socket data ready */
	if (sk->sk_data_ready == pmnet_listen_data_ready) {
		sk->sk_data_ready = sk->sk_user_data;
		sk->sk_user_data = NULL;
	}

	BUG_ON(sk->sk_user_data != NULL);
	sk->sk_user_data = sc;
	sc_get(sc);

	sc->sc_data_ready = sk->sk_data_ready;
	sc->sc_state_change = sk->sk_state_change;

	sk->sk_data_ready = pmnet_data_ready;
	sk->sk_state_change = pmnet_state_change;

	mutex_init(&sc->sc_send_lock);

	write_unlock_bh(&sk->sk_callback_lock);
}

static int pmnet_unregister_callbacks(struct sock *sk,
			           struct pmnet_sock_container *sc)
{
	int ret = 0;

	write_lock_bh(&sk->sk_callback_lock);
	if (sk->sk_user_data == sc) {
		ret = 1;
		sk->sk_user_data = NULL;
		sk->sk_data_ready = sc->sc_data_ready;
		sk->sk_state_change = sc->sc_state_change;
	}
	write_unlock_bh(&sk->sk_callback_lock);

	return ret;
}

/*
 * this is a little helper that is called by callers who have seen a problem
 * with an sc and want to detach it from the nn if someone already hasn't beat
 * them to it.  if an error is given then the shutdown will be persistent
 * and pending transmits will be canceled.
 */
static void pmnet_ensure_shutdown(struct pmnet_node *nn,
			           struct pmnet_sock_container *sc,
				   int err)
{
	spin_lock(&nn->nn_lock);
	if (nn->nn_sc == sc)
		pmnet_set_nn_state(nn, NULL, 0, err);
	spin_unlock(&nn->nn_lock);
}

/*
 * This work queue function performs the blocking parts of socket shutdown.  A
 * few paths lead here.  set_nn_state will trigger this callback if it sees an
 * sc detached from the nn.  state_change will also trigger this callback
 * directly when it sees errors.  In that case we need to call set_nn_state
 * ourselves as state_change couldn't get the nn_lock and call set_nn_state
 * itself.
 */
static void pmnet_shutdown_sc(struct work_struct *work)
{
	struct pmnet_sock_container *sc =
		container_of(work, struct pmnet_sock_container,
			     sc_shutdown_work);
//	struct pmnet_node *nn = pmnet_nn_from_num(sc->sc_node->nd_num);
	struct pmnet_node *nn = pmnet_nn_from_num(0);

	pr_info("shutting down\n");

	/* drop the callbacks ref and call shutdown only once */
	if (pmnet_unregister_callbacks(sc->sc_sock->sk, sc)) {
		/* we shouldn't flush as we're in the thread, the
		 * races with pending sc work structs are harmless */
		del_timer_sync(&sc->sc_idle_timeout);
		pmnet_sc_cancel_delayed_work(sc, &sc->sc_keepalive_work);
		sc_put(sc);
		kernel_sock_shutdown(sc->sc_sock, SHUT_RDWR);
	}

	/* not fatal so failed connects before the other guy has our
	 * heartbeat can be retried */
	pmnet_ensure_shutdown(nn, sc, 0);
	sc_put(sc);
}

static void pmnet_initialize_handshake(void)
{
//	pmnet_hand->pmhb_heartbeat_timeout_ms = cpu_to_be32(
//			PMHB_MAX_WRITE_TIMEOUT_MS);
	pmnet_hand->pmnet_idle_timeout_ms = cpu_to_be32(pmnet_idle_timeout());
	pmnet_hand->pmnet_keepalive_delay_ms = cpu_to_be32(
			pmnet_keepalive_delay());
	pmnet_hand->pmnet_reconnect_delay_ms = cpu_to_be32(
			pmnet_reconnect_delay());
}

/* TODO: how can I use this func */
static void pmnet_sendpage(struct pmnet_sock_container *sc,
			   void *kmalloced_virt,
			   size_t size)
{
	struct pmnet_node *nn = pmnet_nn_from_num(0);
	ssize_t ret;

	while (1) {
		mutex_lock(&sc->sc_send_lock);
		ret = sc->sc_sock->ops->sendpage(sc->sc_sock,
						 virt_to_page(kmalloced_virt),
						 offset_in_page(kmalloced_virt),
						 size, MSG_DONTWAIT);
		mutex_unlock(&sc->sc_send_lock);
		if (ret == size)
			break;
		if (ret == (ssize_t)-EAGAIN) {
			pr_info("sendpage of size %zu to failed with EAGAIN\n", size);
			cond_resched();
			continue;
		}
		pr_info("sendpage of size %zu to failed with %zd\n", size, ret);
		pmnet_ensure_shutdown(nn, sc, 0);
		break;
	}
}

static void pmnet_init_msg(struct pmnet_msg *msg, u16 data_len, u16 msg_type, u32 key, u32 index)
{
	memset(msg, 0, sizeof(struct pmnet_msg));
	msg->magic = cpu_to_be16(PMNET_MSG_MAGIC);
	msg->data_len = cpu_to_be16(data_len);
	msg->msg_type = cpu_to_be16(msg_type);
	msg->sys_status = cpu_to_be32(PMNET_ERR_NONE);
	msg->status = 0;
	msg->key = cpu_to_be32(key);
	msg->index = cpu_to_be32(index);
}


/*
 * if nn->nn_sc is valid then increase kref (kref_get) and return that sc
 */
static int pmnet_tx_can_proceed(struct pmnet_node *nn,
		struct pmnet_sock_container **sc_ret,
		int *error)
{
	int ret = 0;

	spin_lock(&nn->nn_lock);
	if (nn->nn_persistent_error) {
		ret = 1;
		*sc_ret = NULL;
		*error = nn->nn_persistent_error;
	} else if (nn->nn_sc_valid) {
		kref_get(&nn->nn_sc->sc_kref);
		ret = 1;
		*sc_ret = nn->nn_sc;
		*error = 0;
	}
	spin_unlock(&nn->nn_lock);

	return ret;
}

/* Get a map of all nodes to which this node is currently connected to */
void pmnet_fill_node_map(unsigned long *map, unsigned bytes)
{
	struct pmnet_sock_container *sc;
	int node, ret;

	BUG_ON(bytes < (BITS_TO_LONGS(PMNM_MAX_NODES) * sizeof(unsigned long)));

	memset(map, 0, bytes);
	for (node = 0; node < PMNM_MAX_NODES; ++node) {
		if (!pmnet_tx_can_proceed(pmnet_nn_from_num(node), &sc, &ret))
			continue;
		if (!ret) {
			set_bit(node, map);
			sc_put(sc);
		}
	}
}
EXPORT_SYMBOL_GPL(pmnet_fill_node_map);


static int pmnet_send_tcp_msg(struct socket *sock, struct kvec *vec,
		size_t veclen, size_t total)
{
	int ret;
	struct msghdr msg = {.msg_flags = 0,};


#if 1
	struct kvec *vec1 = NULL;
	struct msghdr msg1 = {.msg_flags = 0,};
	struct pmnet_msg *pmsg = NULL;
	size_t veclen1, total1;
//	char buf[4096];

//	memset(buf, 0, 4096);

	vec1 = kmalloc_array(1, sizeof(struct kvec), GFP_ATOMIC);
	if (vec1 == NULL) {
		pr_info("failed to %zu element kvec!\n", veclen);
		ret = -ENOMEM;
		goto out;
	}

	pmsg = kmalloc(sizeof(struct pmnet_msg), GFP_ATOMIC);
	if (!pmsg) {
		pr_info("failed to allocate a pmnet_msg!\n");
		ret = -ENOMEM;
		goto out;
	}

	pmnet_init_msg(pmsg, 0, 0, 0, 0);

	vec1[0].iov_len = sizeof(struct pmnet_msg);
	vec1[0].iov_base = pmsg;
//	vec1[1].iov_len = 4096;
//	vec1[1].iov_base = buf;

	pmsg->magic = cpu_to_be16(PMNET_MSG_STATUS_MAGIC);  // twiddle the magic
	pmsg->data_len = 0;

	veclen1 = 1;
	total1 = sizeof(struct pmnet_msg) ;

#endif

	if (sock == NULL) {
		ret = -EINVAL;
		goto out;
	}

#if 1
	ret = kernel_sendmsg(sock, &msg1, vec1, veclen1, total1);

	if (likely(ret == total1))
		return 0;
#endif

#if 0
	/* TODO: 얘가 잘못 */
//	ret = kernel_sendmsg(sock, &msg, vec, veclen, total);
//	return 0;
	if (likely(ret == total))
		return 0;

	pr_info("sendmsg returned %d instead of %zu\n", ret, total);
	if (ret >= 0)
		ret = -EPIPE; /* should be smarter, I bet */
#endif
out:
	pr_info("returning error: %d\n", ret);
	return ret;
}

int pmnet_send_message_vec(u32 msg_type, u32 key, u32 index, struct kvec *caller_vec,
		size_t caller_veclen, u8 target_node, int *status)
{
	int ret = 0;
	struct pmnet_msg *msg = NULL;
	size_t veclen, caller_bytes = 0;
	struct kvec *vec = NULL;
	struct pmnet_sock_container *sc = NULL;
	struct pmnet_node *nn = pmnet_nn_from_num(target_node);
	struct pmnet_status_wait nsw = {
		.ns_node_item = LIST_HEAD_INIT(nsw.ns_node_item),
	};
	struct pmnet_send_tracking nst;

	pmnet_init_nst(&nst, msg_type, key, current, target_node);

	if (pmnet_wq == NULL) {
		pr_info("attempt to tx without pmnetd running\n");
		ret = -ESRCH;
		goto out;
	}

	if (caller_veclen == 0) {
		pr_info("caller_veclen is 0\n");
		ret = -EINVAL;
		goto out;
	}

	caller_bytes = iov_length((struct iovec *)caller_vec, caller_veclen);
	if (caller_bytes > PMNET_MAX_PAYLOAD_BYTES) {
		pr_info("caller_bytes(%zu) too large\n", caller_bytes);
		ret = -EINVAL;
		goto out;
	}

	pmnet_debug_add_nst(&nst);

	pmnet_set_nst_sock_time(&nst);
	
	wait_event(nn->nn_sc_wq, pmnet_tx_can_proceed(nn, &sc, &ret));
	if (ret)
		goto out;

	pmnet_set_nst_sock_container(&nst, sc);

	veclen = caller_veclen + 1;
	vec = kmalloc_array(veclen, sizeof(struct kvec), GFP_ATOMIC);
	if (vec == NULL) {
		pr_info("failed to %zu element kvec!\n", veclen);
		ret = -ENOMEM;
		goto out;
	}

	msg = kmalloc(sizeof(struct pmnet_msg), GFP_ATOMIC);
	if (!msg) {
		pr_info("failed to allocate a pmnet_msg!\n");
		ret = -ENOMEM;
		goto out;
	}

	pmnet_init_msg(msg, caller_bytes, msg_type, key, index);

	vec[0].iov_len = sizeof(struct pmnet_msg);
	vec[0].iov_base = msg;
	memcpy(&vec[1], caller_vec, caller_veclen * sizeof(struct kvec));

	/* TODO: is this code working? */
	ret = pmnet_prep_nsw(nn, &nsw);
	if (ret)
		goto out;

	msg->msg_num = cpu_to_be32(nsw.ns_id);
	pmnet_set_nst_msg_id(&nst, nsw.ns_id);

	pmnet_set_nst_send_time(&nst);

	/*
	 * finally, convert the message header to network byte-order
	 * and send 
	 */
	mutex_lock(&sc->sc_send_lock);
	ret = pmnet_send_tcp_msg(sc->sc_sock, vec, veclen,
			sizeof(struct pmnet_msg) + caller_bytes);
	mutex_unlock(&sc->sc_send_lock);
	if (ret < 0) {
		pr_info("error returned from pmnet_send_tcp_msg=%d\n", ret);
		goto out;
	}

	/* wait on other node's handler */
	pmnet_set_nst_status_time(&nst);
	/* TODO: make wait_event work */
//	wait_event(nsw.ns_wq, pmnet_nsw_completed(nn, &nsw));

	pmnet_update_send_stats(&nst, sc);

	/* Note that we avoid overwriting the callers status return
	 * variable if a system error was reported on the other
	 * side. Callers beware. */
	ret = pmnet_sys_err_to_errno(nsw.ns_sys_status);
	if (status && !ret)
		*status = nsw.ns_status;

	mlog(0, "woken, returning system status %d, user status %d\n",
			ret, nsw.ns_status);

out:
	pmnet_debug_del_nst(&nst); /* must be before dropping sc and node */
	if (sc)
		sc_put(sc);
	kfree(vec);
	kfree(msg);
	pmnet_complete_nsw(nn, &nsw, 0, 0, 0);
	return ret;
}
EXPORT_SYMBOL_GPL(pmnet_send_message_vec);

int pmnet_send_message(u32 msg_type, u32 key, u32 index, void *data, u32 len,
		u8 target_node, int *status)
{
	struct kvec vec = {
		.iov_base = data,
		.iov_len = len,
	};
	return pmnet_send_message_vec(msg_type, key, index, &vec, 1,
			target_node, status);
}
EXPORT_SYMBOL_GPL(pmnet_send_message);

#if 0
static int pmnet_send_status_magic(struct socket *sock, struct pmnet_msg *hdr,
				   enum pmnet_system_error syserr, int err)
{
	struct kvec vec = {
		.iov_base = hdr,
		.iov_len = sizeof(struct pmnet_msg),
	};

	BUG_ON(syserr >= PMNET_ERR_MAX);

	/* leave other fields intact from the incoming message, msg_num
	 * in particular */
	hdr->sys_status = cpu_to_be32(syserr);
	hdr->status = cpu_to_be32(err);
	hdr->magic = cpu_to_be16(PMNET_MSG_STATUS_MAGIC);  // twiddle the magic
	hdr->data_len = 0;

	msglog(hdr, "about to send status magic %d\n", err);
	/* hdr has been in host byteorder this whole time */
	return pmnet_send_tcp_msg(sock, &vec, 1, sizeof(struct pmnet_msg));
}
#endif

/* this returns -errno if the header was unknown or too large, etc.
 * after this is called the buffer us reused for the next message */
static int pmnet_process_message(struct pmnet_sock_container *sc,
				 struct pmnet_msg *hdr)
{
	struct pmnet_node *nn = pmnet_nn_from_num(sc->sc_node->nd_num);
	int ret = 0;
//	int ret = 0, handler_status;
//	enum  pmnet_system_error syserr;
//	struct pmnet_msg_handler *nmh = NULL;
//	void *ret_data = NULL;

	msglog(hdr, "processing message\n");

	pmnet_sc_postpone_idle(sc);

	switch(be16_to_cpu(hdr->magic)) {
		case PMNET_MSG_STATUS_MAGIC:
			/* special type for returning message status */
			pmnet_complete_nsw(nn, NULL,
					   be32_to_cpu(hdr->msg_num),
					   be32_to_cpu(hdr->sys_status),
					   be32_to_cpu(hdr->status));
			goto out;
		case PMNET_MSG_KEEP_REQ_MAGIC:
			pmnet_sendpage(sc, pmnet_keep_resp,
				       sizeof(*pmnet_keep_resp));
			goto out;
		case PMNET_MSG_KEEP_RESP_MAGIC:
			goto out;
		case PMNET_MSG_MAGIC:
			break;
		default:
			msglog(hdr, "bad magic\n");
			ret = -EINVAL;
			goto out;
			break;
	}

	switch(be16_to_cpu(hdr->msg_type)) {
		case PMNET_MSG_SUCCESS:
			pmnet_complete_nsw(nn, NULL,
					   be32_to_cpu(hdr->msg_num),
					   be32_to_cpu(hdr->sys_status),
					   be32_to_cpu(hdr->status));

			pr_info("SERVER-->CLIENT: PMNET_MSG_SUCCESS\n");
			break;
	}
	goto out;

#if 0
	/* find a handler for it */
	handler_status = 0;
	nmh = pmnet_handler_get(be16_to_cpu(hdr->msg_type),
				be32_to_cpu(hdr->key));
	if (!nmh) {
		mlog(ML_TCP, "couldn't find handler for type %u key %08x\n",
		     be16_to_cpu(hdr->msg_type), be32_to_cpu(hdr->key));
		syserr = PMNET_ERR_NO_HNDLR;
		goto out_respond;
	}

	syserr = PMNET_ERR_NONE;

	if (be16_to_cpu(hdr->data_len) > nmh->nh_max_len)
		syserr = PMNET_ERR_OVERFLOW;

	if (syserr != PMNET_ERR_NONE)
		goto out_respond;

	pmnet_set_func_start_time(sc);
	sc->sc_msg_key = be32_to_cpu(hdr->key);
	sc->sc_msg_type = be16_to_cpu(hdr->msg_type);
	handler_status = (nmh->nh_func)(hdr, sizeof(struct pmnet_msg) +
					     be16_to_cpu(hdr->data_len),
					nmh->nh_func_data, &ret_data);
	pmnet_set_func_stop_time(sc);

	pmnet_update_recv_stats(sc);

out_respond:
	/* this destroys the hdr, so don't use it after this */
	mutex_lock(&sc->sc_send_lock);
	ret = pmnet_send_status_magic(sc->sc_sock, hdr, syserr,
				      handler_status);
	mutex_unlock(&sc->sc_send_lock);
	hdr = NULL;
	mlog(0, "sending handler status %d, syserr %d returned %d\n",
	     handler_status, syserr, ret);

	if (nmh) {
		BUG_ON(ret_data != NULL && nmh->nh_post_func == NULL);
		if (nmh->nh_post_func)
			(nmh->nh_post_func)(handler_status, nmh->nh_func_data,
					    ret_data);
	}
#endif

out:
#if 0
	if (nmh)
		pmnet_handler_put(nmh);
#endif
	return ret;
}

#if 0
static int pmnet_check_handshake(struct pmnet_sock_container *sc)
{
	struct pmnet_handshake *hand = page_address(sc->sc_page);
	struct pmnet_node *nn = pmnet_nn_from_num(sc->sc_node->nd_num);

	if (hand->protocol_version != cpu_to_be64(pmNET_PROTOCOL_VERSION)) {
		printk(KERN_NOTICE "pmnet: " SC_NODEF_FMT " Advertised net "
		       "protocol version %llu but %llu is required. "
		       "Disconnecting.\n", SC_NODEF_ARGS(sc),
		       (unsigned long long)be64_to_cpu(hand->protocol_version),
		       pmNET_PROTOCOL_VERSION);

		/* don't bother reconnecting if its the wrong version. */
		pmnet_ensure_shutdown(nn, sc, -ENOTCONN);
		return -1;
	}

	/*
	 * Ensure timeouts are consistent with other nodes, otherwise
	 * we can end up with one node thinking that the other must be down,
	 * but isn't. This can ultimately cause corruption.
	 */
	if (be32_to_cpu(hand->pmnet_idle_timeout_ms) !=
				pmnet_idle_timeout()) {
		printk(KERN_NOTICE "pmnet: " SC_NODEF_FMT " uses a network "
		       "idle timeout of %u ms, but we use %u ms locally. "
		       "Disconnecting.\n", SC_NODEF_ARGS(sc),
		       be32_to_cpu(hand->pmnet_idle_timeout_ms),
		       pmnet_idle_timeout());
		pmnet_ensure_shutdown(nn, sc, -ENOTCONN);
		return -1;
	}

	if (be32_to_cpu(hand->pmnet_keepalive_delay_ms) !=
			pmnet_keepalive_delay()) {
		printk(KERN_NOTICE "pmnet: " SC_NODEF_FMT " uses a keepalive "
		       "delay of %u ms, but we use %u ms locally. "
		       "Disconnecting.\n", SC_NODEF_ARGS(sc),
		       be32_to_cpu(hand->pmnet_keepalive_delay_ms),
		       pmnet_keepalive_delay());
		pmnet_ensure_shutdown(nn, sc, -ENOTCONN);
		return -1;
	}

	if (be32_to_cpu(hand->pmhb_heartbeat_timeout_ms) !=
			pmHB_MAX_WRITE_TIMEOUT_MS) {
		printk(KERN_NOTICE "pmnet: " SC_NODEF_FMT " uses a heartbeat "
		       "timeout of %u ms, but we use %u ms locally. "
		       "Disconnecting.\n", SC_NODEF_ARGS(sc),
		       be32_to_cpu(hand->pmhb_heartbeat_timeout_ms),
		       pmHB_MAX_WRITE_TIMEOUT_MS);
		pmnet_ensure_shutdown(nn, sc, -ENOTCONN);
		return -1;
	}

	sc->sc_handshake_ok = 1;

	spin_lock(&nn->nn_lock);
	/* set valid and queue the idle timers only if it hasn't been
	 * shut down already */
	if (nn->nn_sc == sc) {
		pmnet_sc_reset_idle_timer(sc);
		atomic_set(&nn->nn_timeout, 0);
		pmnet_set_nn_state(nn, sc, 1, 0);
	}
	spin_unlock(&nn->nn_lock);

	/* shift everything up as though it wasn't there */
	sc->sc_page_off -= sizeof(struct pmnet_handshake);
	if (sc->sc_page_off)
		memmove(hand, hand + 1, sc->sc_page_off);

	return 0;
}
#endif



static int pmnet_recv_tcp_msg(struct socket *sock, void *data, size_t len)
{
	struct kvec vec = { .iov_len = len, .iov_base = data, };
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT, };
	iov_iter_kvec(&msg.msg_iter, READ, &vec, 1, len);
	return sock_recvmsg(sock, &msg, MSG_DONTWAIT);
}

int pmnet_recv_message_vec(u32 msg_type, u32 key, struct kvec *caller_vec,
		size_t caller_veclen, u8 target_node, int *status)
{
	int ret = 0;
	struct pmnet_msg *msg = NULL;
	size_t veclen, caller_bytes = 0;
	struct kvec *vec = NULL;
	struct pmnet_sock_container *sc = NULL;
	struct pmnet_node *nn = pmnet_nn_from_num(target_node);

	struct socket *conn_socket = NULL;

	/* XXX: DONTWAIT works WAITALL doesn't work */
	/* solved --> WAITALL wait until recv all data 
	 * It blocked because sender send a portion of data 
	 */
	struct msghdr msghdr = {.msg_flags = MSG_DONTWAIT,};
//	struct msghdr msghdr = {.msg_flags = MSG_WAITALL,};

	DECLARE_WAIT_QUEUE_HEAD(recv_wait);

	if (pmnet_wq == NULL) {
		pr_info("attempt to tx without pmnetd running\n");
		ret = -ESRCH;
		goto out;
	}

	if (caller_veclen == 0) {
		pr_info("caller_veclen is 0\n");
		ret = -EINVAL;
		goto out;
	}

	caller_bytes = iov_length((struct iovec *)caller_vec, caller_veclen);
	if (caller_bytes > PMNET_MAX_PAYLOAD_BYTES) {
		pr_info("caller_bytes(%ld) too large\n", caller_bytes);
		ret = -EINVAL;
		goto out;
	}

	*status = -1;

	sc = nn->nn_sc;
	if (sc == NULL)
		pr_info("%s: sc = NULL\n", __func__);
	conn_socket = sc->sc_sock;
	if (conn_socket == NULL)
		pr_info("%s: conn_socket = NULL\n", __func__);
	if (conn_socket->sk == NULL)
		pr_info("%s: conn_socket->sk = NULL\n", __func__);

	veclen = caller_veclen + 1;
	vec = kmalloc(sizeof(struct kvec) * veclen, GFP_ATOMIC);
	if (vec == NULL) {
		pr_info("failed to %zu element kvec!\n", veclen);
		ret = -ENOMEM;
		goto out;
	}

	msg = kmalloc(sizeof(struct pmnet_msg), GFP_ATOMIC);
	if (!msg) {
		pr_info("failed to allocate a pmnet_msg!\n");
		ret = -ENOMEM;
		goto out;
	}

	vec[0].iov_len = sizeof(struct pmnet_msg);
	vec[0].iov_base = msg;
	memcpy(&vec[1], caller_vec, caller_veclen * sizeof(struct kvec));

	wait_event_timeout(recv_wait,\
			!skb_queue_empty(&conn_socket->sk->sk_receive_queue),\
			5*HZ);
	if(!skb_queue_empty(&conn_socket->sk->sk_receive_queue))
	{
read_again:
		ret = kernel_recvmsg(conn_socket, &msghdr, vec, veclen, 
				sizeof(struct pmnet_msg) + caller_bytes, msghdr.msg_flags);

		if(ret == -EAGAIN || ret == -ERESTARTSYS)
		{
				pr_info(" *** mtp | error while reading: %d | "
						"tcp_client_receive *** \n", ret);

				goto read_again;
		}

		pr_info("Client<--SERVER: ( msg_type=%u, data_len=%u )\n", 
				be16_to_cpu(msg->msg_type), be16_to_cpu(msg->data_len));

		if (be16_to_cpu(msg->msg_type) == msg_type) 
		{
			pr_info("recv_message: msg_type matched\n");
			*status = 1;
		} else {
			pr_info("recv_message: msg_type not matched\n");
		}

		if (ret < 0)
			pr_info("ERROR: pmnet_recv_message\n");
	}


out:
	if (sc)
		sc_put(sc);
	kfree(vec);
	kfree(msg);
	return ret;
}
EXPORT_SYMBOL_GPL(pmnet_recv_message_vec);


/* receive message from target node (pm_server) */
int pmnet_recv_message(u32 msg_type, u32 key, void *data, u32 len,
		u8 target_node, int *status)
{
	struct kvec vec = {
		.iov_base = data,
		.iov_len = len,
	};
	return pmnet_recv_message_vec(msg_type, key, &vec, 1,
			target_node, status);
}
EXPORT_SYMBOL_GPL(pmnet_recv_message);

/* ---------------------------------------------------- */

/* this demuxes the queued rx bytes into header or payload bits and calls
 * handlers as each full message is read off the socket.  it returns -error,
 * == 0 eof, or > 0 for progress made.*/
static int pmnet_advance_rx(struct pmnet_sock_container *sc)
{
	struct pmnet_msg *hdr;
	int ret = 0;
	void *data;
	size_t datalen;

	sclog(sc, "receiving\n");
	pmnet_set_advance_start_time(sc);

#if 0
	if (unlikely(sc->sc_handshake_ok == 0)) {
		if(sc->sc_page_off < sizeof(struct pmnet_handshake)) {
			data = page_address(sc->sc_page) + sc->sc_page_off;
			datalen = sizeof(struct pmnet_handshake) - sc->sc_page_off;
			ret = pmnet_recv_tcp_msg(sc->sc_sock, data, datalen);
			if (ret > 0)
				sc->sc_page_off += ret;
		}

		if (sc->sc_page_off == sizeof(struct pmnet_handshake)) {
			pmnet_check_handshake(sc);
			if (unlikely(sc->sc_handshake_ok == 0))
				ret = -EPROTO;
		}
		goto out;
	}
#endif

	/* do we need more header? */
	if (sc->sc_page_off < sizeof(struct pmnet_msg)) {
		data = page_address(sc->sc_page) + sc->sc_page_off;
		datalen = sizeof(struct pmnet_msg) - sc->sc_page_off;
		ret = pmnet_recv_tcp_msg(sc->sc_sock, data, datalen);
		if (ret > 0) {
			sc->sc_page_off += ret;
			/* only swab incoming here.. we can
			 * only get here once as we cross from
			 * being under to over */
			if (sc->sc_page_off == sizeof(struct pmnet_msg)) {
				hdr = page_address(sc->sc_page);
				if (be16_to_cpu(hdr->data_len) >
				    PMNET_MAX_PAYLOAD_BYTES)
					ret = -EOVERFLOW;
			}
		}
		if (ret <= 0)
			goto out;
	}

	if (sc->sc_page_off < sizeof(struct pmnet_msg)) {
		/* oof, still don't have a header */
		goto out;
	}

	/* this was swabbed above when we first read it */
	hdr = page_address(sc->sc_page);

	msglog(hdr, "at page_off %zu\n", sc->sc_page_off);

	/* do we need more payload? */
	if (sc->sc_page_off - sizeof(struct pmnet_msg) < be16_to_cpu(hdr->data_len)) {
		/* need more payload */
		data = page_address(sc->sc_page) + sc->sc_page_off;
		datalen = (sizeof(struct pmnet_msg) + be16_to_cpu(hdr->data_len)) -
			  sc->sc_page_off;
		ret = pmnet_recv_tcp_msg(sc->sc_sock, data, datalen);
		if (ret > 0)
			sc->sc_page_off += ret;
		if (ret <= 0)
			goto out;
	}

	if (sc->sc_page_off - sizeof(struct pmnet_msg) == be16_to_cpu(hdr->data_len)) {
		/* we can only get here once, the first time we read
		 * the payload.. so set ret to progress if the handler
		 * works out. after calling this the message is toast */
		ret = pmnet_process_message(sc, hdr);
		if (ret == 0)
			ret = 1;
		sc->sc_page_off = 0;
	}

out:
	sclog(sc, "ret = %d\n", ret);
	pmnet_set_advance_stop_time(sc);
	return ret;
}


/* this work func is triggerd by data ready.  it reads until it can read no
 * more.  it interprets 0, eof, as fatal.  if data_ready hits while we're doing
 * our work the work struct will be marked and we'll be called again. */
static void pmnet_rx_until_empty(struct work_struct *work)
{
	struct pmnet_sock_container *sc =
		container_of(work, struct pmnet_sock_container, sc_rx_work);
	int ret;

	do {
		ret = pmnet_advance_rx(sc);
	} while (ret > 0);

	if (ret <= 0 && ret != -EAGAIN) {
		struct pmnet_node *nn = pmnet_nn_from_num(sc->sc_node->nd_num);
		pr_info("pmnet_rx_until_empty: saw error %d, closing\n", ret);
		/* not permanent so read failed handshake can retry */
		pmnet_ensure_shutdown(nn, sc, 0);
	}

	sc_put(sc);
}


static int pmnet_set_nodelay(struct socket *sock)
{
	int ret, val = 1;
	mm_segment_t oldfs;

	oldfs = get_fs();
	set_fs(KERNEL_DS);

	/*
	 * Dear unsuspecting programmer,
	 *
	 * Don't use sock_setsockopt() for SOL_TCP.  It doesn't check its level
	 * argument and assumes SOL_SOCKET so, say, your TCP_NODELAY will
	 * silently turn into SO_DEBUG.
	 *
	 * Yours,
	 * Keeper of hilariously fragile interfaces.
	 */
	ret = sock->ops->setsockopt(sock, SOL_TCP, TCP_NODELAY,
			(char __user *)&val, sizeof(val));

	set_fs(oldfs);
	return ret;
}

static int pmnet_set_usertimeout(struct socket *sock)
{
	int user_timeout = PMNET_TCP_USER_TIMEOUT;

	return kernel_setsockopt(sock, SOL_TCP, TCP_USER_TIMEOUT,
			(char *)&user_timeout, sizeof(user_timeout));
}


/* called when a connect completes and after a sock is accepted.  the
 * rx path will see the response and mark the sc valid */
static void pmnet_sc_connect_completed(struct work_struct *work)
{
	struct pmnet_sock_container *sc =
		container_of(work, struct pmnet_sock_container,
			     sc_connect_work);

	/* TODO: Why below log doesn't appear? pr_info works, sclog and mlog doesn't work */
	mlog(ML_MSG, "sc sending handshake with ver %llu id %llx\n",
              (unsigned long long)PMNET_PROTOCOL_VERSION,
	      (unsigned long long)be64_to_cpu(pmnet_hand->connector_id));

	pmnet_initialize_handshake();
	pmnet_sendpage(sc, pmnet_hand, sizeof(*pmnet_hand));
	sc_put(sc);
	pr_info("%s: finished\n", __func__);
}

/* this is called as a work_struct func. */
static void pmnet_sc_send_keep_req(struct work_struct *work)
{
	struct pmnet_sock_container *sc =
		container_of(work, struct pmnet_sock_container,
			     sc_keepalive_work.work);

	pmnet_sendpage(sc, pmnet_keep_req, sizeof(*pmnet_keep_req));
	sc_put(sc);
}

/* socket shutdown does a del_timer_sync against this as it tears down.
 * we can't start this timer until we've got to the point in sc buildup
 * where shutdown is going to be involved */
static void pmnet_idle_timer(struct timer_list *t)
{
	struct pmnet_sock_container *sc = from_timer(sc, t, sc_idle_timeout);
	struct pmnet_node *nn = pmnet_nn_from_num(0);
#ifdef CONFIG_DEBUG_FS
	unsigned long msecs = ktime_to_ms(ktime_get()) -
		ktime_to_ms(sc->sc_tv_timer);
#else
	unsigned long msecs = pmnet_idle_timeout();
#endif

	printk(KERN_NOTICE "pmnet: Connection to " SC_NODEF_FMT " has been "
	       "idle for %lu.%lu secs.\n",
	       SC_NODEF_ARGS(sc), msecs / 1000, msecs % 1000);

	/* idle timerout happen, don't shutdown the connection, but
	 * make fence decision. Maybe the connection can recover before
	 * the decision is made.
	 */
	atomic_set(&nn->nn_timeout, 1);
//	pmquo_conn_err(pmnet_num_from_nn(nn));
//	queue_delayed_work(pmnet_wq, &nn->nn_still_up,
//			msecs_to_jiffies(PMNET_QUORUM_DELAY_MS));

	pmnet_sc_reset_idle_timer(sc);
}

static void pmnet_sc_reset_idle_timer(struct pmnet_sock_container *sc)
{
	pmnet_sc_cancel_delayed_work(sc, &sc->sc_keepalive_work);
	pmnet_sc_queue_delayed_work(sc, &sc->sc_keepalive_work,
		      msecs_to_jiffies(pmnet_keepalive_delay()));
	pmnet_set_sock_timer(sc);
	mod_timer(&sc->sc_idle_timeout,
	       jiffies + msecs_to_jiffies(pmnet_idle_timeout()));
}

static void pmnet_sc_postpone_idle(struct pmnet_sock_container *sc)
{
	struct pmnet_node *nn = pmnet_nn_from_num(sc->sc_node->nd_num);

	/* clear fence decision since the connection recover from timeout*/
	if (atomic_read(&nn->nn_timeout)) {
//		pmquo_conn_up(pmnet_num_from_nn(nn));
		cancel_delayed_work(&nn->nn_still_up);
		atomic_set(&nn->nn_timeout, 0);
	}

	/* Only push out an existing timer */
	if (timer_pending(&sc->sc_idle_timeout))
		pmnet_sc_reset_idle_timer(sc);
}


/* ---------------------------------------------------- */

/* 
 * this work func is kicked whenever a path sets the nn state which doesn't
 * have valid set.  This includes seeing hb come up, losing a connection,
 * having a connect attempt fail, etc. This centralizes the logic which decides
 * if a connect attempt should be made or if we should give up and all future
 * transmit attempts should fail 
 */
static void pmnet_start_connect(struct work_struct *work)
{
	struct pmnet_node *nn = 
		container_of(work, struct pmnet_node, nn_connect_work.work);
	struct pmnet_sock_container *sc = NULL;
	struct pmnm_node *node = NULL;
	struct socket *sock = NULL;
	struct sockaddr_in remoteaddr = {0, };
	int ret = 0, stop;
	unsigned int timeout;

	/*
	 * sock_create allocates the sock with GFP_KERNEL. We must set
	 * per-process flag PF_MEMALLOC_NOIO so that all allocations done
	 * by this process are done as if GFP_NOIO was specified. So we
	 * are not reentering filesystem while doing memory reclaim.
	 */

	/* watch for racing with tearing a node down */
	node = pmnm_get_node_by_num(pmnet_num_from_nn(nn));
	if (node == NULL) {
		printk(KERN_ERR "There is no node available\n");
		goto out;
	}

	spin_lock(&nn->nn_lock);
	/*
	 * see if we already have one pending or have given up.
	 * For nn_timeout, it is set when we close the connection
	 * because of the idle time out. So it means that we have
	 * at least connected to that node successfully once,
	 * now try to connect to it again.
	 */
	timeout = atomic_read(&nn->nn_timeout);
	stop = (nn->nn_sc ||
		(nn->nn_persistent_error &&
		(nn->nn_persistent_error != -ENOTCONN || timeout == 0)));
	spin_unlock(&nn->nn_lock);
	if (stop)
		goto out;

	nn->nn_last_connect_attempt = jiffies;

	sc = sc_alloc(node);
	if (sc == NULL) {
		pr_info("couldn't allocate sc\n");
		ret = -ENOMEM;
		goto out;
	} 

	ret = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (ret < 0) {
		pr_info("can't create socket: %d\n", ret);
		goto out;
	}
	sc->sc_sock = sock; /* freed by sc_kref_release */

	sock->sk->sk_allocation = GFP_ATOMIC;

	ret = pmnet_set_nodelay(sc->sc_sock);
	if (ret) {
		pr_info("setting TCP_NODELAY failed with %d\n", ret);
		goto out;
	}

	ret = pmnet_set_usertimeout(sock);
	if (ret) {
		pr_info("set TCP_USER_TIMEOUT failed with %d\n", ret);
		goto out;
	}

	pmnet_register_callbacks(sc->sc_sock->sk, sc);

	spin_lock(&nn->nn_lock);
	/* XXX: handshake completion will set nn->nn_sc_valid */
	pmnet_set_nn_state(nn, sc, 1, 0);
	spin_unlock(&nn->nn_lock);

	remoteaddr.sin_family = AF_INET;
	remoteaddr.sin_addr.s_addr = node->nd_ipv4_address;
	remoteaddr.sin_port = node->nd_ipv4_port;

	ret = sc->sc_sock->ops->connect(sc->sc_sock,
			(struct sockaddr *)&remoteaddr,
			sizeof(remoteaddr),
			O_NONBLOCK);
	if (ret == -EINPROGRESS)
		ret = 0;

out:
	if (ret && sc) {
		printk(KERN_NOTICE "pmnet: Connect attempt to " SC_NODEF_FMT
		       " failed with errno %d\n", SC_NODEF_ARGS(sc), ret);
		/* 0 err so that another will be queued and attempted
		 * from set_nn_state 
		 */
		pmnet_ensure_shutdown(nn, sc, 0);
	}
	if (sc)
		sc_put(sc);

	/* XXX: pmnm_node_put make error */
#if 0
	if (node)
		pmnm_node_put(node);
#endif

	pr_info("pmnet_start_connect::end\n");
	return;
}

static void pmnet_connect_expired(struct work_struct *work)
{
	struct pmnet_node *nn =
		container_of(work, struct pmnet_node, nn_connect_expired.work);

	spin_lock(&nn->nn_lock);
	if (!nn->nn_sc_valid) {
		printk(KERN_NOTICE "pmnet: No connection established with "
				"node %u after %u.%u seconds, check network and"
				" cluster configuration.\n",
				pmnet_num_from_nn(nn),
				pmnet_idle_timeout() / 1000,
				pmnet_idle_timeout() % 1000);
		pmnet_set_nn_state(nn, NULL, 0, 0);
	}
	spin_unlock(&nn->nn_lock);
}

static void pmnet_still_up(struct work_struct *work)
{
#if 0
	struct pmnet_node *nn =
		container_of(work, struct pmnet_node, nn_still_up.work);

	pmquo_hb_still_up(pmnet_num_from_nn(nn));
#endif
}


void pmnet_disconnect_node(struct pmnm_node *node)
{
	struct pmnet_node *nn = pmnet_nn_from_num(0);

	/* don't reconnect until it's heartbeating again */
	spin_lock(&nn->nn_lock);
	atomic_set(&nn->nn_timeout, 0);
	pmnet_set_nn_state(nn, NULL, 0, -ENOTCONN);
	spin_unlock(&nn->nn_lock);

	if (pmnet_wq) {
		cancel_delayed_work(&nn->nn_connect_expired);
		cancel_delayed_work(&nn->nn_connect_work);
		cancel_delayed_work(&nn->nn_still_up);
		flush_workqueue(pmnet_wq);
	}
}


/* ------------------------------------------------------------ */

static void pmnet_listen_data_ready(struct sock *sk)
{
	void (*ready)(struct sock *sk);

	read_lock_bh(&sk->sk_callback_lock);
	ready = sk->sk_user_data;
	if (ready == NULL) { /* check for teardown race */
		ready = sk->sk_data_ready;
		goto out;
	}

	/* This callback may called twice when a new connection
	 * is  being established as a child socket inherits everything
	 * from a parent LISTEN socket, including the data_ready cb of
	 * the parent. This leads to a hazard. In pmnet_accept_one()
	 * we are still initializing the child socket but have not
	 * changed the inherited data_ready callback yet when
	 * data starts arriving.
	 * We avoid this hazard by checking the state.
	 * For the listening socket,  the state will be TCP_LISTEN; for the new
	 * socket, will be  TCP_ESTABLISHED. Also, in this case,
	 * sk->sk_user_data is not a valid function pointer.
	 */

#if 0
	if (sk->sk_state == TCP_LISTEN) {
		queue_work(pmnet_wq, &pmnet_listen_work);
	} else {
		ready = NULL;
	}
#endif 

	ready = NULL;

out:
	read_unlock_bh(&sk->sk_callback_lock);
	if (ready != NULL)
		ready(sk);
}


/* ------------------------------------------------------------ */

int pmnet_init(void)
{
	unsigned long i;

	pmnet_debugfs_init();

	init_pmnm_cluster();

	pmnet_hand = kzalloc(sizeof(struct pmnet_handshake), GFP_KERNEL);
	pmnet_keep_req = kzalloc(sizeof(struct pmnet_msg), GFP_KERNEL);
	pmnet_keep_resp = kzalloc(sizeof(struct pmnet_msg), GFP_KERNEL);
	if (!pmnet_hand || !pmnet_keep_req || !pmnet_keep_resp)
		goto out;

	pmnet_hand->protocol_version = cpu_to_be64(PMNET_PROTOCOL_VERSION);
	pmnet_hand->connector_id = cpu_to_be64(1);

	pmnet_keep_req->magic = cpu_to_be16(PMNET_MSG_KEEP_REQ_MAGIC);
	pmnet_keep_resp->magic = cpu_to_be16(PMNET_MSG_KEEP_RESP_MAGIC);


	/* Codes from o2net_start_listening */
	BUG_ON(pmnet_wq != NULL);
	/* perpapre work queue */
	mlog(ML_KTHREAD, "starting pmnet thread...\n");
	pmnet_wq = alloc_ordered_workqueue("pmnet", WQ_MEM_RECLAIM);
	if (pmnet_wq == NULL) {
		mlog(ML_ERROR, "unable to launch pmnet thread\n");
		return -ENOMEM; /* ? */
	}

	for (i = 0; i < ARRAY_SIZE(pmnet_nodes) - 1; i++) {
		struct pmnet_node *nn = pmnet_nn_from_num(i);
		
		pr_info("pmnet_init::set pmnet_node\n");
		atomic_set(&nn->nn_timeout, 0);
		spin_lock_init(&nn->nn_lock);

		INIT_DELAYED_WORK(&nn->nn_connect_work, pmnet_start_connect);
		INIT_DELAYED_WORK(&nn->nn_connect_expired, pmnet_connect_expired);
		INIT_DELAYED_WORK(&nn->nn_still_up, pmnet_still_up);
		/* until we see hb from a node we'll return einval */
		nn->nn_persistent_error = -ENOTCONN;
		init_waitqueue_head(&nn->nn_sc_wq);
		idr_init(&nn->nn_status_idr);
		INIT_LIST_HEAD(&nn->nn_status_list);

		/* TODO: queue_delayed_work is it right position? */
		queue_delayed_work(pmnet_wq, &nn->nn_connect_work, 0);
	}

	return 0;

out:
	kfree(pmnet_hand);
	kfree(pmnet_keep_req);
	kfree(pmnet_keep_resp);
	return -ENOMEM;
}

void pmnet_exit(void)
{
	exit_pmnm_cluster();
	kfree(pmnet_hand);
	kfree(pmnet_keep_req);
	kfree(pmnet_keep_resp);
}
