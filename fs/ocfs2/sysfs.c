#include <linux/fs.h>
#include "ocfs2.h"
#include "sysfs.h"

static ssize_t slot_num_show(struct super_block *sb,
			     struct super_block_attribute *attr,
	  		     char *buf)
{
	struct ocfs2_super *osb = OCFS2_SB(sb);
	return sprintf(buf, "%d\n", osb->slot_num);
}

static SB_ATTR_RO(slot_num);
static struct attribute *ocfs2_sb_attrs[] = {
	&sb_attr_slot_num.attr,
	NULL
};

struct kobj_type ocfs2_sb_ktype = {
	.default_attrs	= ocfs2_sb_attrs,
	.sysfs_ops	= &super_block_sysfs_ops,
	.release	= super_block_release,
};



