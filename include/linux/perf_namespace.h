#ifndef _LINUX_PERF_NS_H
#define _LINUX_PERF_NS_H

#include <linux/nsproxy.h>
#include <linux/kref.h>
#include <linux/ns_common.h>

struct user_namespace;
extern struct user_namespace init_user_ns;

struct perf_ns_info {
	u64             time;
	u64             timestamp;
};

struct perf_namespace {
	struct kref kref;
	struct perf_ns_info __percpu *info;
	struct user_namespace *user_ns;	/* Owning user namespace */
	struct ns_common ns;
};
extern struct perf_namespace init_perf_ns;

#ifdef CONFIG_PERF_NS
extern struct perf_namespace *copy_perf_ns(unsigned long flags,
	struct user_namespace *user_ns, struct perf_namespace *old_ns);
extern void free_perf_ns(struct kref *kref);

static inline void get_perf_ns(struct perf_namespace *ns)
{
	kref_get(&ns->kref);
}

static inline void put_perf_ns(struct perf_namespace *ns)
{
	kref_put(&ns->kref, free_perf_ns);
}

#else /* !CONFIG_PERF_NS */
static inline void get_perf_ns(struct perf_namespace *ns)
{
}

static inline void put_perf_ns(struct perf_namespace *ns)
{
}

static inline struct perf_namespace *copy_perf_ns(unsigned long flags,
	struct user_namespace *user_ns, struct perf_namespace *old_ns)
{
	if (flags & CLONE_NEWPERF)
		return ERR_PTR(-EINVAL);

	return old_ns;
}
#endif /* CONFIG_PERF_NS */

#endif /* _LINUX_PERF_NS_H */
