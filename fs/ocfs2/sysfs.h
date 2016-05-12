

#ifndef _SYS_H
#define _SYS_H

extern struct kobj_type ocfs2_sb_ktype;
void ocfs2_report_error(struct ocfs2_super *osb, unsigned long long ino,
		unsigned long long blkno, int error);

#endif
