#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/vmalloc.h>
#include <net/stats.h>

struct net_stats_cb_entry {
	struct list_head list;
	net_stats_cb_t func;
};

static DEFINE_SPINLOCK(net_stats_lock);
static LIST_HEAD(net_stats_cb_list);

int register_net_stats_cb(net_stats_cb_t func)
{
	struct net_stats_cb_entry *entry = vmalloc(sizeof(*entry));

	if (!entry)
		return -ENOMEM;
	entry->func = func;
	spin_lock(&net_stats_lock);
	list_add_tail(&entry->list, &net_stats_cb_list);
	spin_unlock(&net_stats_lock);
	return 0;
}
EXPORT_SYMBOL(register_net_stats_cb);

int unregister_net_stats_cb(net_stats_cb_t func)
{
	struct net_stats_cb_entry *entry;
	bool found = false;

	spin_lock(&net_stats_lock);
	list_for_each_entry(entry, &net_stats_cb_list, list) {
		if (entry->func == func) {
			found = true;
			break;
		}
	}
	spin_unlock(&net_stats_lock);

	if (!found)
		return -ENOENT;

	list_del(&entry->list);
	vfree(entry);
	return 0;
}
EXPORT_SYMBOL(unregister_net_stats_cb);

static int net_stats_cpu_notify(struct notifier_block *nb,
				unsigned long action, void *hcpu)
{
	struct net_stats_cb_entry *entry;
	long cpu = (long)hcpu;
	int ret;

	if ((action & ~CPU_TASKS_FROZEN) == CPU_DYING) {
		/* We call callbacks in dying stage, when machine is stopped */
		spin_lock(&net_stats_lock);
		list_for_each_entry(entry, &net_stats_cb_list, list) {
			ret = entry->func(cpu);
			if (ret)
				break;
		}
		spin_unlock(&net_stats_lock);

		if (ret)
			return NOTIFY_BAD;
	}
	return NOTIFY_OK;
}

static struct notifier_block net_stats_nfb = {
	.notifier_call = net_stats_cpu_notify,
};

static int __init net_stats_init(void)
{
	return register_cpu_notifier(&net_stats_nfb);
}

subsys_initcall(net_stats_init);
