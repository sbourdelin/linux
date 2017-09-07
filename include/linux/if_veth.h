struct veth_priv {
	struct net_device __rcu *peer;
	atomic64_t              dropped;
	unsigned int            requested_headroom;
};

void veth_common_setup(struct net_device *dev);
void veth_dellink(struct net_device *dev, struct list_head *head);
void veth_link_ops_init(struct rtnl_link_ops *ops);
