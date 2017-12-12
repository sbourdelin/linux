// SPDX-License-Identifier: GPL-2.0
/*
 * Some non-inline ceph helpers
 */
#include <linux/module.h>
#include <linux/ceph/types.h>

void ceph_file_layout_from_legacy(struct ceph_file_layout *fl,
				  struct ceph_file_layout_legacy *legacy)
{
	fl->stripe_unit = le32_to_cpu(legacy->fl_stripe_unit);
	fl->stripe_count = le32_to_cpu(legacy->fl_stripe_count);
	fl->object_size = le32_to_cpu(legacy->fl_object_size);
	fl->pool_id = le32_to_cpu(legacy->fl_pg_pool);
	if (fl->pool_id == 0 && fl->stripe_unit == 0 &&
	    fl->stripe_count == 0 && fl->object_size == 0)
		fl->pool_id = -1;
}
EXPORT_SYMBOL(ceph_file_layout_from_legacy);

void ceph_file_layout_to_legacy(struct ceph_file_layout *fl,
				struct ceph_file_layout_legacy *legacy)
{
	legacy->fl_stripe_unit = cpu_to_le32(fl->stripe_unit);
	legacy->fl_stripe_count = cpu_to_le32(fl->stripe_count);
	legacy->fl_object_size = cpu_to_le32(fl->object_size);
	if (fl->pool_id >= 0)
		legacy->fl_pg_pool = cpu_to_le32(fl->pool_id);
	else
		legacy->fl_pg_pool = 0;
}
EXPORT_SYMBOL(ceph_file_layout_to_legacy);

int ceph_flags_to_mode(int flags)
{
	int mode;

#ifdef O_DIRECTORY  /* fixme */
	if ((flags & O_DIRECTORY) == O_DIRECTORY)
		return CEPH_FILE_MODE_PIN;
#endif

	switch (flags & O_ACCMODE) {
	case O_WRONLY:
		mode = CEPH_FILE_MODE_WR;
		break;
	case O_RDONLY:
		mode = CEPH_FILE_MODE_RD;
		break;
	case O_RDWR:
	case O_ACCMODE: /* this is what the VFS does */
		mode = CEPH_FILE_MODE_RDWR;
		break;
	}
#ifdef O_LAZY
	if (flags & O_LAZY)
		mode |= CEPH_FILE_MODE_LAZY;
#endif

	return mode;
}
EXPORT_SYMBOL(ceph_flags_to_mode);

int ceph_caps_for_mode(int mode)
{
	int caps = CEPH_CAP_PIN;

	if (mode & CEPH_FILE_MODE_RD)
		caps |= CEPH_CAP_FILE_SHARED |
			CEPH_CAP_FILE_RD | CEPH_CAP_FILE_CACHE;
	if (mode & CEPH_FILE_MODE_WR)
		caps |= CEPH_CAP_FILE_EXCL |
			CEPH_CAP_FILE_WR | CEPH_CAP_FILE_BUFFER |
			CEPH_CAP_AUTH_SHARED | CEPH_CAP_AUTH_EXCL |
			CEPH_CAP_XATTR_SHARED | CEPH_CAP_XATTR_EXCL;
	if (mode & CEPH_FILE_MODE_LAZY)
		caps |= CEPH_CAP_FILE_LAZYIO;

	return caps;
}
EXPORT_SYMBOL(ceph_caps_for_mode);
