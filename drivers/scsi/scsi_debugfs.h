#include <linux/debugfs.h>

struct request;
struct seq_file;

#ifdef CONFIG_DEBUG_FS
extern struct dentry *scsi_debugfs_root;
extern struct dentry *scsi_debugfs_uld;
extern struct dentry *scsi_debugfs_lld;
#endif

void scsi_show_rq(struct seq_file *m, struct request *rq);
