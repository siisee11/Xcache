#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kthread.h>

#include <linux/errno.h>
#include <linux/types.h>

#include <linux/netdevice.h>
#include <linux/ip.h>
#include <linux/in.h>

#include <linux/unistd.h>
#include <linux/wait.h>

#include <net/sock.h>
#include <net/tcp.h>
#include <net/inet_connection_sock.h>
#include <net/request_sock.h>

#include "tcp.h"
#include "nodemanager.h"

#define DEFAULT_PORT 2325
#define MODULE_NAME "tcp_server"
#define MAX_CONNS 16


int tcp_server_start(struct pmnm_node *node)
{
	int ret;
	ret = pmnet_start_listening(node);
	if (ret < 0)
		pr_info("pmnet_start_listening failed\n");

	return ret;
}

static int __init network_server_init(void)
{
	struct pmnm_node *node = NULL;

	pr_info(" *** mtp | network_server initiated | "
			"network_server_init ***\n");

	init_pmnm_cluster();
	
	node = pmnm_get_node_by_num(0);

	tcp_server_start(node);
	return 0;
}

static void __exit network_server_exit(void)
{
	int ret;

	exit_pmnm_cluster();

	pr_info(" *** mtp | network server module unloaded | "
			"network_server_exit *** \n");
}

module_init(network_server_init)
module_exit(network_server_exit)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NAM JAEYOUN");
