/*
 * Copyright (c) 2016 Red Hat, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_inode.h"
#include "xfs_dir2.h"
#include "xfs_ialloc.h"
#include "xfs_alloc.h"
#include "xfs_rtalloc.h"
#include "xfs_bmap.h"
#include "xfs_trans.h"
#include "xfs_trans_priv.h"
#include "xfs_log.h"
#include "xfs_error.h"
#include "xfs_quota.h"
#include "xfs_fsops.h"
#include "xfs_trace.h"
#include "xfs_icache.h"
#include "xfs_sysfs.h"
/* XXX: above copied from xfs_mount.c */
#include "xfs_thin.h"

/*
 * Notes/Issues:
 *
 * - Reservation support depends on the '-o discard' mount option so freed
 *   extents are returned to the pool. Note that online discard has not been
 *   totally reliable in terms of returning freed space to the thin pool. Use
 *   fstrim as a workaround.
 * - The bdev reservation API receives an absolute value reservation from the
 *   caller as opposed to a delta value. The latter is probably more ideal, but
 *   the former helps us use the XFS reserve pool as a broad protection layer
 *   for any potential leaks. For example, free list blocks used for btree
 *   growth are currently not reserved. With a delta API, _any_ unreserved
 *   allocations from the fs will slowly and permanently leak the reservation as
 *   tracked by the bdev. The abs value mechanism covers this kind of slop based
 *   on the locally maintained reservation.
 *   	- What might be ideal to support a delta reservation API is a model (or
 *   	test mode) that requires a reservation to be attached or somehow
 *   	associated with every bdev allocation when the reserve feature is
 *   	enabled (or one that disables allocation via writes altogether in favor
 *   	of provision calls). Otherwise, any unreserved allocation returns an I/O
 *   	error. Such deterministic behavior helps ensure general testing detects
 *   	problems more reliably.
 * - Worst case reservation means each XFS filesystem block is considered a new
 *   dm block allocation. This translates to a significant amount of space given
 *   larger dm block sizes. For example, 4k XFS blocks to 64k dm blocks means
 *   we'll hit ENOSPC sooner and more frequently than typically expected.
 * - The xfs_mod_fdblocks() implementation means the XFS reserve pool blocks are
 *   also reserved from the thin pool. XFS defaults to 8192 reserve pool blocks
 *   in most cases, which translates to 512MB of reserved space. This can be
 *   tuned with: 'xfs_io -xc "resblks <blks>" <mnt>'. Note that insufficient
 *   reserves will result in errors in unexpected areas of code (e.g., page
 *   discards on writeback, inode unlinked list removal failures, etc.).
 */

static inline int
bdev_reserve_space(
	struct xfs_mount			*mp,
	int					mode,
	sector_t				offset,
	sector_t				len,
	sector_t				*res)
{
	struct block_device			*bdev;
	const struct block_device_operations	*ops;

	bdev = mp->m_ddev_targp->bt_bdev;
	ops = bdev->bd_disk->fops;

	return ops->reserve_space(bdev, mode, offset, len, res);
}

/*
 * Reserve blocks from the underlying block device.
 */
int
xfs_thin_reserve(
	struct xfs_mount	*mp,
	sector_t		bb)
{
	int			error;
	sector_t		res;

	mutex_lock(&mp->m_thin_res_lock);

	res = mp->m_thin_res + bb;
	error = bdev_reserve_space(mp, BDEV_RES_MOD, 0, 0, &res);
	if (error) {
		if (error == -ENOSPC)
			trace_xfs_thin_reserve_enospc(mp, mp->m_thin_res, bb);
		goto out;
	}

	trace_xfs_thin_reserve(mp, mp->m_thin_res, bb);
	mp->m_thin_res += bb;

out:
	mutex_unlock(&mp->m_thin_res_lock);
	return error;
}

static int
__xfs_thin_unreserve(
	struct xfs_mount	*mp,
	sector_t		bb)
{
	int			error;
	sector_t		res;

	if (bb > mp->m_thin_res) {
		WARN(1, "unres (%llu) exceeds current res (%llu)",
		     (uint64_t) bb, (uint64_t) mp->m_thin_res);
		bb = mp->m_thin_res;
	}

	res = mp->m_thin_res - bb;
	error = bdev_reserve_space(mp, BDEV_RES_MOD, 0, 0, &res);
	if (error)
		return error;;

	trace_xfs_thin_unreserve(mp, mp->m_thin_res, bb);
	mp->m_thin_res -= bb;

	return error;
}

/*
 * Release a reservation back to the block device.
 */
int
xfs_thin_unreserve(
	struct xfs_mount	*mp,
	sector_t		res)
{
	int			error;

	mutex_lock(&mp->m_thin_res_lock);
	error = __xfs_thin_unreserve(mp, res);
	mutex_unlock(&mp->m_thin_res_lock);

	return error;
}

/*
 * Given a recently allocated extent, ask the block device to provision the
 * underlying space.
 */
int
xfs_thin_provision(
	struct xfs_mount	*mp,
	xfs_fsblock_t		offset,
	xfs_fsblock_t		len,
	sector_t		*res)
{
	sector_t		ores = *res;
	sector_t		bbstart, bblen;
	int			error;

	bbstart = XFS_FSB_TO_DADDR(mp, offset);
	bbstart = round_down(bbstart, mp->m_thin_sectpb);
	bblen = XFS_FSB_TO_BB(mp, len);
	bblen = round_up(bblen, mp->m_thin_sectpb);

	mutex_lock(&mp->m_thin_res_lock);

	WARN_ON(bblen > mp->m_thin_res);

	error = bdev_reserve_space(mp, BDEV_RES_PROVISION, bbstart, bblen,
				   res);
	if (error)
		goto out;
	ASSERT(ores >= *res);

	trace_xfs_thin_provision(mp, mp->m_thin_res, ores - *res);

	/*
	 * Update the local reservation based on the blocks that were actually
	 * allocated.
	 */
	mp->m_thin_res -= (ores - *res);
out:
	mutex_unlock(&mp->m_thin_res_lock);
	return error;
}

int
xfs_thin_init(
	struct xfs_mount			*mp)
{
	struct block_device			*bdev;
	const struct block_device_operations	*ops;
	sector_t				res;
	int					error;
	unsigned int				io_opt;

	bdev = mp->m_ddev_targp->bt_bdev;
	ops = bdev->bd_disk->fops;

	mp->m_thin_reserve = false;
	mutex_init(&mp->m_thin_res_lock);

	if (!ops->reserve_space)
		goto out;
	if (!(mp->m_flags & XFS_MOUNT_DISCARD))
		goto out;

	/* use optimal I/O size as dm-thin block size */
	io_opt = bdev_io_opt(mp->m_super->s_bdev);
	if ((io_opt % BBSIZE) || (io_opt < mp->m_sb.sb_blocksize))
		goto out;
	mp->m_thin_sectpb = io_opt / BBSIZE;

	/* warn about any preexisting reservation */
	error = bdev_reserve_space(mp, BDEV_RES_GET, 0, 0, &res);
	if (error)
		goto out;
	if (res) {
		/* force res count to 0 */
		xfs_warn(mp, "Reset non-zero (%llu sectors) block reservation.",
			 (uint64_t) res);
		res = 0;
		error = bdev_reserve_space(mp, BDEV_RES_MOD, 0, 0, &res);
		if (error)
			goto out;
	}

	mp->m_thin_reserve = true;
out:
	xfs_notice(mp, "Thin pool reservation %s", mp->m_thin_reserve ?
							"enabled" : "disabled");
	if (mp->m_thin_reserve)
		xfs_notice(mp, "Thin reserve blocksize: %u sectors",
			   mp->m_thin_sectpb);
	return 0;
}
