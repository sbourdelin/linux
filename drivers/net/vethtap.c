#include <linux/etherdevice.h>
#include <linux/if_tap.h>
#include <linux/if_vlan.h>
#include <linux/if_veth.h>
#include <linux/interrupt.h>
#include <linux/nsproxy.h>
#include <linux/compat.h>
#include <linux/if_tun.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/cache.h>
#include <linux/sched/signal.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/cdev.h>
#include <linux/idr.h>
#include <linux/fs.h>
#include <linux/uio.h>

#include <net/net_namespace.h>
#include <net/rtnetlink.h>
#include <net/sock.h>
#include <linux/virtio_net.h>
#include <linux/skb_array.h>

struct vethtap_dev {
	struct veth_priv  veth;
	struct tap_dev    tap;
};

/* Variables for dealing with vethtaps device numbers.
 */
static dev_t vethtap_major;

static const void *vethtap_net_namespace(struct device *d)
{
	struct net_device *dev = to_net_dev(d->parent);

	return dev_net(dev);
}

static struct class vethtap_class = {
	.name = "vethtap",
	.owner = THIS_MODULE,
	.ns_type = &net_ns_type_operations,
	.namespace = vethtap_net_namespace,
};

static struct cdev vethtap_cdev;

#define TUN_OFFLOADS (NETIF_F_HW_CSUM | NETIF_F_TSO_ECN | NETIF_F_TSO | \
		      NETIF_F_TSO6)

static void vethtap_count_tx_dropped(struct tap_dev *tap)
{
	struct vethtap_dev *vethtap = container_of(tap, struct vethtap_dev,
						   tap);
	struct veth_priv *veth = &vethtap->veth;

	atomic64_inc(&veth->dropped);
}

static int vethtap_newlink(struct net *src_net, struct net_device *dev,
			   struct nlattr *tb[], struct nlattr *data[],
			   struct netlink_ext_ack *extack)
{
	struct vethtap_dev *vethtap = netdev_priv(dev);
	int err;

	INIT_LIST_HEAD(&vethtap->tap.queue_list);

	/* Since macvlan supports all offloads by default, make
	 * tap support all offloads also.
	 */
	vethtap->tap.tap_features = TUN_OFFLOADS;

	/* Register callbacks for rx/tx drops accounting and updating
	 * net_device features
	 */
	vethtap->tap.count_tx_dropped = vethtap_count_tx_dropped;
	vethtap->tap.count_rx_dropped = NULL;
	vethtap->tap.update_features  = NULL;

	err = netdev_rx_handler_register(dev, tap_handle_frame, &vethtap->tap);
	if (err)
		return err;

	vethtap->tap.dev = dev;

	return 0;
}

static void vethtap_dellink(struct net_device *dev,
			    struct list_head *head)
{
	struct vethtap_dev *vethtap = netdev_priv(dev);

	netdev_rx_handler_unregister(dev);
	tap_del_queues(&vethtap->tap);
	veth_dellink(dev, head);
}

static void vethtap_setup(struct net_device *dev)
{
	veth_common_setup(dev);
	dev->tx_queue_len = TUN_READQ_SIZE;
}

struct rtnl_link_ops vethtap_link_ops __read_mostly = {
	.kind           = "vethtap",
	.setup		= vethtap_setup,
	.newlink	= vethtap_newlink,
	.dellink	= vethtap_dellink,
	.priv_size      = sizeof(struct vethtap_dev),
};

static int vethtap_device_event(struct notifier_block *unused,
				unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct vethtap_dev *vethtap;
	struct device *classdev;
	dev_t devt;
	int err;
	char tap_name[IFNAMSIZ];

	if (dev->rtnl_link_ops != &vethtap_link_ops)
		return NOTIFY_DONE;

	snprintf(tap_name, IFNAMSIZ, "tap%d", dev->ifindex);
	vethtap = netdev_priv(dev);

	switch (event) {
	case NETDEV_REGISTER:
		/* Create the device node here after the network device has
		 * been registered but before register_netdevice has
		 * finished running.
		 */
		err = tap_get_minor(vethtap_major, &vethtap->tap);
		if (err)
			return notifier_from_errno(err);

		devt = MKDEV(MAJOR(vethtap_major), vethtap->tap.minor);
		classdev = device_create(&vethtap_class, &dev->dev, devt,
					 dev, tap_name);
		if (IS_ERR(classdev)) {
			tap_free_minor(vethtap_major, &vethtap->tap);
			return notifier_from_errno(PTR_ERR(classdev));
		}
		err = sysfs_create_link(&dev->dev.kobj, &classdev->kobj,
					tap_name);
		if (err)
			return notifier_from_errno(err);
		break;
	case NETDEV_UNREGISTER:
		/* vlan->minor == 0 if NETDEV_REGISTER above failed */
		if (vethtap->tap.minor == 0)
			break;
		sysfs_remove_link(&dev->dev.kobj, tap_name);
		devt = MKDEV(MAJOR(vethtap_major), vethtap->tap.minor);
		device_destroy(&vethtap_class, devt);
		tap_free_minor(vethtap_major, &vethtap->tap);
		break;
	case NETDEV_CHANGE_TX_QUEUE_LEN:
		if (tap_queue_resize(&vethtap->tap))
			return NOTIFY_BAD;
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block vethtap_notifier_block __read_mostly = {
	.notifier_call	= vethtap_device_event,
};

int vethtap_init(void)
{
	int err;

	err = tap_create_cdev(&vethtap_cdev, &vethtap_major, "vethtap");

	if (err)
		goto out1;

	err = class_register(&vethtap_class);
	if (err)
		goto out2;

	err = register_netdevice_notifier(&vethtap_notifier_block);
	if (err)
		goto out3;

	veth_link_ops_init(&vethtap_link_ops);
	if (err)
		goto out4;

	return 0;

out4:
	unregister_netdevice_notifier(&vethtap_notifier_block);
out3:
	class_unregister(&vethtap_class);
out2:
	tap_destroy_cdev(vethtap_major, &vethtap_cdev);
out1:
	return err;
}

void vethtap_exit(void)
{
	unregister_netdevice_notifier(&vethtap_notifier_block);
	class_unregister(&vethtap_class);
	tap_destroy_cdev(vethtap_major, &vethtap_cdev);
}
