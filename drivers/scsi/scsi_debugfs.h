#include <linux/debugfs.h>

#ifdef CONFIG_DEBUG_FS
extern struct dentry *scsi_debugfs_root;
extern struct dentry *scsi_debugfs_uld;
extern struct dentry *scsi_debugfs_lld;
#endif

#ifdef CONFIG_BLK_DEBUG_FS
struct request;
struct seq_file;

void scsi_show_rq(struct seq_file *m, struct request *rq);
#endif
