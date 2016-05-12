#include <linux/fs.h>
#include "ocfs2.h"
#include "sysfs.h"
#include "filecheck.h"

static ssize_t slot_num_show(struct super_block *sb,
			     struct super_block_attribute *attr,
	  		     char *buf)
{
	struct ocfs2_super *osb = OCFS2_SB(sb);
	return sprintf(buf, "%d\n", osb->slot_num);
}

static ssize_t file_check_show(struct super_block *sb,
		struct super_block_attribute *attr,
		char *buf)
{
	struct ocfs2_super *osb = OCFS2_SB(sb);
	return ocfs2_filecheck_show(osb, OCFS2_FILECHECK_TYPE_CHK, buf);
}

static ssize_t file_check_store(struct super_block *sb,
		struct super_block_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long t;
	int ret;
	struct ocfs2_super *osb = OCFS2_SB(sb);

	ret = kstrtoul(skip_spaces(buf), 0, &t);
	if (ret)
		return ret;
	return ocfs2_filecheck_add_inode(osb, t);
}

static ssize_t file_fix_show(struct super_block *sb,
		struct super_block_attribute *attr,
		char *buf)
{
	struct ocfs2_super *osb = OCFS2_SB(sb);
	return ocfs2_filecheck_show(osb, OCFS2_FILECHECK_TYPE_FIX, buf);
}

static ssize_t file_fix_store(struct super_block *sb,
		struct super_block_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long t;
	int ret;
	struct ocfs2_super *osb = OCFS2_SB(sb);

	ret = kstrtoul(skip_spaces(buf), 0, &t);
	if (ret)
		return ret;
	return ocfs2_filecheck_add_inode(osb, t);
}

static ssize_t file_check_max_entries_show(struct super_block *sb,
		struct super_block_attribute *attr,
		char *buf)
{
	int len = 0;
	struct ocfs2_super *osb = OCFS2_SB(sb);
	spin_lock(&osb->fc_lock);
	/* Show done, current size and max */
	len += sprintf(buf, "%d\t%d\t%d\n", osb->fc_done, osb->fc_size,
			osb->fc_max);
	spin_unlock(&osb->fc_lock);
	return len;
}

static ssize_t file_check_max_entries_store(struct super_block *sb,
		struct super_block_attribute *attr,
		const char *buf, size_t count)
{

	unsigned long t;
	int ret;
	struct ocfs2_super *osb = OCFS2_SB(sb);

	ret = kstrtoul(skip_spaces(buf), 0, &t);
	if (ret)
		return ret;
	return ocfs2_filecheck_set_max_entries(osb, (int)t);
}

static SB_ATTR_RO(slot_num);
static SB_ATTR(file_check, (S_IWUSR | S_IRUGO));
static SB_ATTR(file_fix, (S_IWUSR | S_IRUGO));
static SB_ATTR(file_check_max_entries, (S_IWUSR | S_IRUGO));

static struct attribute *ocfs2_sb_attrs[] = {
	&sb_attr_slot_num.attr,
	&sb_attr_file_check.attr,
	&sb_attr_file_fix.attr,
	&sb_attr_file_check_max_entries.attr,
	NULL
};

struct kobj_type ocfs2_sb_ktype = {
	.default_attrs	= ocfs2_sb_attrs,
	.sysfs_ops	= &super_block_sysfs_ops,
	.release	= super_block_release,
};

void ocfs2_report_error(struct ocfs2_super *osb, unsigned long long ino,
			unsigned long long blkno, int errno)
{
	char event_name[] = "EVENT=FS_ERROR";
	char device[16];
	char inode_number[16];
	char error_number[16];
	char block_number[16];
	char *envp[] = {event_name, inode_number, error_number, block_number,
		NULL};
	snprintf(device, 16, "DEVICE=%s", osb->sb->s_id);
	snprintf(error_number, 16, "ERROR=%d", errno);
	snprintf(inode_number, 16, "INODE=%llu", ino);
	snprintf(block_number, 16, "BLOCK=%llu", blkno);
	sb_report_event(osb->sb, envp);
}

