#ifndef BLK_MQ_DEBUGFS_H
#define BLK_MQ_DEBUGFS_H

#ifdef CONFIG_BLK_DEBUG_FS

#include <linux/blkdev.h>
#include <linux/seq_file.h>

struct blk_mq_debugfs_attr {
	const char *name;
	umode_t mode;
	int (*show)(void *, struct seq_file *);
	ssize_t (*write)(void *, const char __user *, size_t, loff_t *);
	/* Set either .show or .seq_ops. */
	const struct seq_operations *seq_ops;
};

int __blk_mq_debugfs_rq_show(struct seq_file *m, struct request *rq);
int blk_mq_debugfs_rq_show(struct seq_file *m, void *v);

#endif

#endif
