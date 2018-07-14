#include <linux/netfilter/nfnetlink_osf.h>
#include <linux/netfilter/nf_osf.h>

/*
 * Indexed by dont-fragment bit.
 * It is the only constant value in the fingerprint.
 */
struct list_head nf_osf_fingers[2];
EXPORT_SYMBOL_GPL(nf_osf_fingers);

static const struct nla_policy nf_osf_policy[OSF_ATTR_MAX + 1] = {
	[OSF_ATTR_FINGER]	= { .len = sizeof(struct nf_osf_user_finger) },
};

int nf_osf_add_callback(struct net *net, struct sock *ctnl,
			struct sk_buff *skb, const struct nlmsghdr *nlh,
			const struct nlattr * const osf_attrs[],
			struct netlink_ext_ack *extack)
{
	struct nf_osf_user_finger *f;
	struct nf_osf_finger *kf = NULL, *sf;
	int err = 0;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (!osf_attrs[OSF_ATTR_FINGER])
		return -EINVAL;

	if (!(nlh->nlmsg_flags & NLM_F_CREATE))
		return -EINVAL;

	f = nla_data(osf_attrs[OSF_ATTR_FINGER]);

	kf = kmalloc(sizeof(struct nf_osf_finger), GFP_KERNEL);
	if (!kf)
		return -ENOMEM;

	memcpy(&kf->finger, f, sizeof(struct nf_osf_user_finger));

	list_for_each_entry(sf, &nf_osf_fingers[!!f->df], finger_entry) {
		if (memcmp(&sf->finger, f, sizeof(struct nf_osf_user_finger)))
			continue;

		kfree(kf);
		kf = NULL;

		if (nlh->nlmsg_flags & NLM_F_EXCL)
			err = -EEXIST;
		break;
	}

	/*
	 * We are protected by nfnl mutex.
	 */
	if (kf)
		list_add_tail_rcu(&kf->finger_entry, &nf_osf_fingers[!!f->df]);

	return err;
}
EXPORT_SYMBOL_GPL(nf_osf_add_callback);

int nf_osf_remove_callback(struct net *net, struct sock *ctnl,
			   struct sk_buff *skb,
			   const struct nlmsghdr *nlh,
			   const struct nlattr * const osf_attrs[],
			   struct netlink_ext_ack *extack)
{
	struct nf_osf_user_finger *f;
	struct nf_osf_finger *sf;
	int err = -ENOENT;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (!osf_attrs[OSF_ATTR_FINGER])
		return -EINVAL;

	f = nla_data(osf_attrs[OSF_ATTR_FINGER]);

	list_for_each_entry(sf, &nf_osf_fingers[!!f->df], finger_entry) {
		if (memcmp(&sf->finger, f, sizeof(struct nf_osf_user_finger)))
			continue;

		/*
		 * We are protected by nfnl mutex.
		 */
		list_del_rcu(&sf->finger_entry);
		kfree_rcu(sf, rcu_head);

		err = 0;
		break;
	}

	return err;
}
EXPORT_SYMBOL_GPL(nf_osf_remove_callback);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Fernando Fernandez <ffmancera@riseup.net>");
