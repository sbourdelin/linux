/*
 * Copyright (c) 2014 Christoph Hellwig.
 */
#include "xfs.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_inode.h"

#include <linux/fs.h>

/*
 * Ensure that we do not have any outstanding pNFS layouts that can be used by
 * clients to directly read from or write to this inode.  This must be called
 * before every operation that can remove blocks from the extent map.
 * Additionally we call it during the write operation, where aren't concerned
 * about exposing unallocated blocks but just want to provide basic
 * synchronization between a local writer and pNFS clients.  mmap writes would
 * also benefit from this sort of synchronization, but due to the tricky locking
 * rules in the page fault path all we can do is start the lease break
 * timeout. See usage of break_layout_nowait in xfs_file_iomap_begin to
 * prevent write-faults from allocating blocks or performing extent
 * conversion.
 */
int
xfs_break_layouts(
	struct inode		*inode,
	uint			*iolock)
{
	struct xfs_inode	*ip = XFS_I(inode);
	int			error;

	ASSERT(xfs_isilocked(ip, XFS_IOLOCK_SHARED|XFS_IOLOCK_EXCL));

	while ((error = break_layout(inode, false) == -EWOULDBLOCK)) {
		xfs_iunlock(ip, *iolock);
		error = break_layout(inode, true);
		*iolock = XFS_IOLOCK_EXCL;
		xfs_ilock(ip, *iolock);
	}

	return error;
}
