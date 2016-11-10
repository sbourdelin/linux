#ifndef _NAMESPACE_H_
#define _NAMESPACE_H_
#ifdef __KERNEL__

#include <linux/poll.h>
#include <linux/ns_common.h>

struct fs_struct;
struct user_namespace;

struct mnt_namespace {
	atomic_t		count;
	struct ns_common	ns;
	struct mount 		*root;
	struct list_head	list;
	struct user_namespace	*user_ns;
	struct ucounts		*ucounts;
	u64			seq;	/* Sequence number to prevent loops */
	wait_queue_head_t	poll;
	u64			event;
	unsigned int		mounts; /* # of mounts in the namespace */
	unsigned int		pending_mounts;
};

extern struct mnt_namespace *copy_mnt_ns(unsigned long, struct mnt_namespace *,
		struct user_namespace *, struct fs_struct *);
extern void put_mnt_ns(struct mnt_namespace *ns);

extern const struct file_operations proc_mounts_operations;
extern const struct file_operations proc_mountinfo_operations;
extern const struct file_operations proc_mountstats_operations;

#endif
#endif
