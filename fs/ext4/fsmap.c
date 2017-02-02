/*
 * Copyright (C) 2017 Oracle.  All Rights Reserved.
 *
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include "ext4.h"
#include "fsmap.h"
#include "mballoc.h"
#include <linux/sort.h>
#include <linux/list_sort.h>
#include <trace/events/ext4.h>

/* Convert an ext4_fsmap to an fsmap. */
void
ext4_fsmap_from_internal(
	struct super_block	*sb,
	struct fsmap		*dest,
	struct ext4_fsmap	*src)
{
	dest->fmr_device = src->fmr_device;
	dest->fmr_flags = src->fmr_flags;
	dest->fmr_physical = src->fmr_physical << sb->s_blocksize_bits;
	dest->fmr_owner = src->fmr_owner;
	dest->fmr_offset = 0;
	dest->fmr_length = src->fmr_length << sb->s_blocksize_bits;
	dest->fmr_reserved[0] = 0;
	dest->fmr_reserved[1] = 0;
	dest->fmr_reserved[2] = 0;
}

/* Convert an fsmap to an ext4_fsmap. */
void
ext4_fsmap_to_internal(
	struct super_block	*sb,
	struct ext4_fsmap	*dest,
	struct fsmap		*src)
{
	dest->fmr_device = src->fmr_device;
	dest->fmr_flags = src->fmr_flags;
	dest->fmr_physical = src->fmr_physical >> sb->s_blocksize_bits;
	dest->fmr_owner = src->fmr_owner;
	dest->fmr_length = src->fmr_length >> sb->s_blocksize_bits;
}

/* getfsmap query state */
struct ext4_getfsmap_info {
	struct ext4_fsmap_head	*head;
	struct ext4_fsmap	*rkey_low;	/* lowest key */
	ext4_fsmap_format_t	formatter;	/* formatting fn */
	void			*format_arg;	/* format buffer */
	bool			last;		/* last extent? */
	ext4_fsblk_t		next_fsblk;	/* next fsblock we expect */
	u32			dev;		/* device id */

	ext4_group_t		agno;		/* AG number, if applicable */
	struct ext4_fsmap	low;		/* low rmap key */
	struct ext4_fsmap	high;		/* high rmap key */
	struct list_head	meta_list;	/* fixed metadata list */
};

/* Associate a device with a getfsmap handler. */
struct ext4_getfsmap_dev {
	u32			dev;
	int			(*fn)(struct super_block *sb,
				      struct ext4_fsmap *keys,
				      struct ext4_getfsmap_info *info);
};

/* Compare two getfsmap device handlers. */
static int
ext4_getfsmap_dev_compare(
	const void			*p1,
	const void			*p2)
{
	const struct ext4_getfsmap_dev	*d1 = p1;
	const struct ext4_getfsmap_dev	*d2 = p2;

	return d1->dev - d2->dev;
}

/* Compare a record against our starting point */
static bool
ext4_getfsmap_rec_before_low_key(
	struct ext4_getfsmap_info	*info,
	struct ext4_fsmap		*rec)
{
	return rec->fmr_physical < info->low.fmr_physical;
}

/*
 * Format a reverse mapping for getfsmap, having translated rm_startblock
 * into the appropriate daddr units.
 */
static int
ext4_getfsmap_helper(
	struct super_block		*sb,
	struct ext4_getfsmap_info	*info,
	struct ext4_fsmap		*rec)
{
	struct ext4_fsmap		fmr;
	struct ext4_sb_info		*sbi = EXT4_SB(sb);
	ext4_fsblk_t			rec_fsblk = rec->fmr_physical;
	ext4_fsblk_t			key_end;
	ext4_group_t			agno;
	ext4_grpblk_t			cno;
	int				error;

	/*
	 * Filter out records that start before our startpoint, if the
	 * caller requested that.
	 */
	if (ext4_getfsmap_rec_before_low_key(info, rec)) {
		rec_fsblk += rec->fmr_length;
		if (info->next_fsblk < rec_fsblk)
			info->next_fsblk = rec_fsblk;
		return EXT4_QUERY_RANGE_CONTINUE;
	}

	/*
	 * If the caller passed in a length with the low record and
	 * the record represents a file data extent, we incremented
	 * the offset in the low key by the length in the hopes of
	 * finding reverse mappings for the physical blocks we just
	 * saw.  We did /not/ increment next_daddr by the length
	 * because the range query would not be able to find shared
	 * extents within the same physical block range.
	 *
	 * However, the extent we've been fed could have a startblock
	 * past the passed-in low record.  If this is the case,
	 * advance next_daddr to the end of the passed-in low record
	 * so we don't report the extent prior to this extent as
	 * free.
	 */
	key_end = info->rkey_low->fmr_physical + info->rkey_low->fmr_length;
	if (info->dev == info->rkey_low->fmr_device &&
	    info->next_fsblk < key_end && rec_fsblk >= key_end)
		info->next_fsblk = key_end;

	/* Are we just counting mappings? */
	if (info->head->fmh_count == 0) {
		if (rec_fsblk > info->next_fsblk)
			info->head->fmh_entries++;

		if (info->last)
			return EXT4_QUERY_RANGE_CONTINUE;

		info->head->fmh_entries++;

		rec_fsblk += rec->fmr_length;
		if (info->next_fsblk < rec_fsblk)
			info->next_fsblk = rec_fsblk;
		return EXT4_QUERY_RANGE_CONTINUE;
	}

	/*
	 * If the record starts past the last physical block we saw,
	 * then we've found some free space.  Report that too.
	 */
	if (rec_fsblk > info->next_fsblk) {
		if (info->head->fmh_entries >= info->head->fmh_count)
			return EXT4_QUERY_RANGE_ABORT;

		ext4_get_group_no_and_offset(sb, info->next_fsblk, &agno, &cno);
		trace_ext4_fsmap_mapping(sb, info->dev, agno,
				EXT4_C2B(sbi, cno),
				rec_fsblk - info->next_fsblk,
				FMR_OWN_UNKNOWN);

		fmr.fmr_device = info->dev;
		fmr.fmr_physical = info->next_fsblk;
		fmr.fmr_owner = FMR_OWN_UNKNOWN;
		fmr.fmr_length = rec_fsblk - info->next_fsblk;
		fmr.fmr_flags = FMR_OF_SPECIAL_OWNER;
		error = info->formatter(&fmr, info->format_arg);
		if (error)
			return error;
		info->head->fmh_entries++;
	}

	if (info->last)
		goto out;

	/* Fill out the extent we found */
	if (info->head->fmh_entries >= info->head->fmh_count)
		return EXT4_QUERY_RANGE_ABORT;

	ext4_get_group_no_and_offset(sb, rec_fsblk, &agno, &cno);
	trace_ext4_fsmap_mapping(sb, info->dev, agno, EXT4_C2B(sbi, cno),
			rec->fmr_length, rec->fmr_owner);

	fmr.fmr_device = info->dev;
	fmr.fmr_physical = rec_fsblk;
	fmr.fmr_owner = rec->fmr_owner;
	fmr.fmr_flags = FMR_OF_SPECIAL_OWNER;
	fmr.fmr_length = rec->fmr_length;
	error = info->formatter(&fmr, info->format_arg);
	if (error)
		return error;
	info->head->fmh_entries++;

out:
	rec_fsblk += rec->fmr_length;
	if (info->next_fsblk < rec_fsblk)
		info->next_fsblk = rec_fsblk;
	return EXT4_QUERY_RANGE_CONTINUE;
}

/* Transform a blockgroup's free record into a fsmap */
static int
ext4_getfsmap_datadev_helper(
	struct super_block		*sb,
	ext4_group_t			agno,
	ext4_grpblk_t			start,
	ext4_grpblk_t			len,
	void				*priv)
{
	struct ext4_fsmap		irec;
	struct ext4_getfsmap_info	*info = priv;
	struct ext4_metadata_fsmap	*p;
	struct ext4_metadata_fsmap	*tmp;
	struct ext4_sb_info		*sbi = EXT4_SB(sb);
	ext4_fsblk_t			fsb;
	int				error;

	fsb = (EXT4_C2B(sbi, start) + ext4_group_first_block_no(sb, agno));

	/* Merge in any relevant extents from the meta_list */
	list_for_each_entry_safe(p, tmp, &info->meta_list, mf_list) {
		if (p->mf_physical + p->mf_length <= info->next_fsblk) {
			list_del(&p->mf_list);
			kfree(p);
		} else if (p->mf_physical < fsb) {
			irec.fmr_physical = p->mf_physical;
			irec.fmr_length = p->mf_length;
			irec.fmr_owner = p->mf_owner;
			irec.fmr_flags = 0;

			error = ext4_getfsmap_helper(sb, info, &irec);
			if (error)
				return error;

			list_del(&p->mf_list);
			kfree(p);
		}
	}

	/* Otherwise, emit it */
	irec.fmr_physical = fsb;
	irec.fmr_length = EXT4_C2B(sbi, len);
	irec.fmr_owner = FMR_OWN_FREE;
	irec.fmr_flags = 0;

	return ext4_getfsmap_helper(sb, info, &irec);
}

/* Execute a getfsmap query against the log device. */
static int
ext4_getfsmap_logdev(
	struct super_block		*sb,
	struct ext4_fsmap		*keys,
	struct ext4_getfsmap_info	*info)
{
	struct ext4_fsmap		*dkey_low = keys;
	journal_t			*journal = EXT4_SB(sb)->s_journal;
	struct ext4_fsmap		irec;

	/* Set up search keys */
	info->low = *dkey_low;
	info->low.fmr_length = 0;

	memset(&info->high, 0xFF, sizeof(info->high));

	trace_ext4_fsmap_low_key(sb, info->dev, 0,
			info->low.fmr_physical,
			info->low.fmr_length,
			info->low.fmr_owner);

	trace_ext4_fsmap_high_key(sb, info->dev, 0,
			info->high.fmr_physical,
			info->high.fmr_length,
			info->high.fmr_owner);

	if (dkey_low->fmr_physical > 0)
		return 0;
	irec.fmr_physical = journal->j_blk_offset;
	irec.fmr_length = journal->j_maxlen;
	irec.fmr_owner = FMR_OWN_LOG;
	irec.fmr_flags = 0;

	return ext4_getfsmap_helper(sb, info, &irec);
}

/*
 * This function returns the number of file system metadata blocks at
 * the beginning of a block group, including the reserved gdt blocks.
 */
static unsigned int
ext4_getfsmap_count_group_meta_blocks(
	struct super_block	*sb,
	ext4_group_t		block_group)
{
	struct ext4_sb_info	*sbi = EXT4_SB(sb);
	unsigned int		num;

	/* Check for superblock and gdt backups in this group */
	num = ext4_bg_has_super(sb, block_group);

	if (!ext4_has_feature_meta_bg(sb) ||
	    block_group < le32_to_cpu(sbi->s_es->s_first_meta_bg) *
			  sbi->s_desc_per_block) {
		if (num) {
			num += ext4_bg_num_gdb(sb, block_group);
			num += le16_to_cpu(sbi->s_es->s_reserved_gdt_blocks);
		}
	} else { /* For META_BG_BLOCK_GROUPS */
		num += ext4_bg_num_gdb(sb, block_group);
	}
	return num;
}

/* Compare two fixed metadata items. */
static int
ext4_getfsmap_compare_fixed_metadata(
	void				*priv,
	struct list_head		*a,
	struct list_head		*b)
{
	struct ext4_metadata_fsmap	*fa;
	struct ext4_metadata_fsmap	*fb;

	fa = container_of(a, struct ext4_metadata_fsmap, mf_list);
	fb = container_of(b, struct ext4_metadata_fsmap, mf_list);
	if (fa->mf_physical < fb->mf_physical)
		return -1;
	else if (fa->mf_physical > fb->mf_physical)
		return 1;
	return 0;
}

/* Merge adjacent extents of fixed metadata. */
static void
ext4_getfsmap_merge_fixed_metadata(
	struct list_head		*meta_list)
{
	struct ext4_metadata_fsmap	*p;
	struct ext4_metadata_fsmap	*prev = NULL;
	struct ext4_metadata_fsmap	*tmp;

	list_for_each_entry_safe(p, tmp, meta_list, mf_list) {
		if (!prev) {
			prev = p;
			continue;
		}

		if (prev->mf_owner == p->mf_owner &&
		    prev->mf_physical + prev->mf_length == p->mf_physical) {
			prev->mf_length += p->mf_length;
			list_del(&p->mf_list);
			kfree(p);
		} else
			prev = p;
	}
}

/* Free a list of fixed metadata. */
static void
ext4_getfsmap_free_fixed_metadata(
	struct list_head		*meta_list)
{
	struct ext4_metadata_fsmap	*p;
	struct ext4_metadata_fsmap	*tmp;

	list_for_each_entry_safe(p, tmp, meta_list, mf_list) {
		list_del(&p->mf_list);
		kfree(p);
	}
}

/* Find all the fixed metadata in the filesystem. */
int
ext4_getfsmap_find_fixed_metadata(
	struct super_block		*sb,
	struct list_head		*meta_list)
{
	struct ext4_metadata_fsmap	*fsm;
	struct ext4_group_desc		*gdp;
	ext4_group_t			agno;
	unsigned int			nr_super;
	int				error;

	INIT_LIST_HEAD(meta_list);

	/* Collect everything. */
	for (agno = 0; agno < EXT4_SB(sb)->s_groups_count; agno++) {
		gdp = ext4_get_group_desc(sb, agno, NULL);
		if (!gdp) {
			error = -EFSCORRUPTED;
			goto err;
		}

		/* Superblock & GDT */
		nr_super = ext4_getfsmap_count_group_meta_blocks(sb, agno);
		if (nr_super) {
			fsm = kmalloc(sizeof(*fsm), GFP_NOFS);
			if (!fsm) {
				error = -ENOMEM;
				goto err;
			}
			fsm->mf_physical = ext4_group_first_block_no(sb, agno);
			fsm->mf_owner = FMR_OWN_FS;
			fsm->mf_length = nr_super;
			list_add_tail(&fsm->mf_list, meta_list);
		}

		/* Block bitmap */
		fsm = kmalloc(sizeof(*fsm), GFP_NOFS);
		if (!fsm) {
			error = -ENOMEM;
			goto err;
		}
		fsm->mf_physical = ext4_block_bitmap(sb, gdp);
		fsm->mf_owner = FMR_OWN_AG;
		fsm->mf_length = 1;
		list_add_tail(&fsm->mf_list, meta_list);

		/* Inode bitmap */
		fsm = kmalloc(sizeof(*fsm), GFP_NOFS);
		if (!fsm) {
			error = -ENOMEM;
			goto err;
		}
		fsm->mf_physical = ext4_inode_bitmap(sb, gdp);
		fsm->mf_owner = FMR_OWN_INOBT;
		fsm->mf_length = 1;
		list_add_tail(&fsm->mf_list, meta_list);

		/* Inodes */
		fsm = kmalloc(sizeof(*fsm), GFP_NOFS);
		if (!fsm) {
			error = -ENOMEM;
			goto err;
		}
		fsm->mf_physical = ext4_inode_table(sb, gdp);
		fsm->mf_owner = FMR_OWN_INODES;
		fsm->mf_length = EXT4_SB(sb)->s_itb_per_group;
		list_add_tail(&fsm->mf_list, meta_list);
	}

	/* Sort the list */
	list_sort(NULL, meta_list, ext4_getfsmap_compare_fixed_metadata);

	/* Merge adjacent extents */
	ext4_getfsmap_merge_fixed_metadata(meta_list);

	return 0;
err:
	ext4_getfsmap_free_fixed_metadata(meta_list);
	return error;
}

/* Execute a getfsmap query against the buddy bitmaps */
static int
ext4_getfsmap_datadev(
	struct super_block		*sb,
	struct ext4_fsmap		*keys,
	struct ext4_getfsmap_info	*info)
{
	struct ext4_fsmap		*dkey_low;
	struct ext4_fsmap		*dkey_high;
	struct ext4_sb_info		*sbi = EXT4_SB(sb);
	ext4_fsblk_t			start_fsb;
	ext4_fsblk_t			end_fsb;
	ext4_fsblk_t			eofs;
	ext4_group_t			start_ag;
	ext4_group_t			end_ag;
	ext4_grpblk_t			first_cluster;
	ext4_grpblk_t			last_cluster;
	int				error = 0;

	dkey_low = keys;
	dkey_high = keys + 1;
	eofs = ext4_blocks_count(sbi->s_es);
	if (dkey_low->fmr_physical >= eofs)
		return 0;
	if (dkey_high->fmr_physical >= eofs)
		dkey_high->fmr_physical = eofs - 1;
	start_fsb = dkey_low->fmr_physical;
	end_fsb = dkey_high->fmr_physical;

	/* Determine first and last group to examine based on start and end */
	ext4_get_group_no_and_offset(sb, start_fsb, &start_ag, &first_cluster);
	ext4_get_group_no_and_offset(sb, end_fsb, &end_ag, &last_cluster);

	/* Set up search keys */
	info->low = *dkey_low;
	info->low.fmr_physical = EXT4_C2B(sbi, first_cluster);
	info->low.fmr_length = 0;

	memset(&info->high, 0xFF, sizeof(info->high));

	/* Assemble a list of all the fixed-location metadata. */
	error = ext4_getfsmap_find_fixed_metadata(sb, &info->meta_list);
	if (error)
		goto err;

	/* Query each AG */
	for (info->agno = start_ag; info->agno <= end_ag; info->agno++) {
		if (info->agno == end_ag) {
			info->high = *dkey_high;
			info->high.fmr_physical = EXT4_C2B(sbi, last_cluster);
			info->high.fmr_length = 0;
		}

		trace_ext4_fsmap_low_key(sb, info->dev, info->agno,
				info->low.fmr_physical,
				info->low.fmr_length,
				info->low.fmr_owner);

		trace_ext4_fsmap_high_key(sb, info->dev, info->agno,
				info->high.fmr_physical,
				info->high.fmr_length,
				info->high.fmr_owner);

		error = ext4_mballoc_query_range(sb, info->agno,
				EXT4_B2C(sbi, info->low.fmr_physical),
				EXT4_B2C(sbi, info->high.fmr_physical),
				ext4_getfsmap_datadev_helper, info);
		if (error)
			goto err;

		if (info->agno == start_ag)
			memset(&info->low, 0, sizeof(info->low));
	}

	/* Report any free space at the end of the AG */
	info->last = true;
	error = ext4_getfsmap_datadev_helper(sb, info->agno, 0, 0, info);
	if (error)
		goto err;

err:
	ext4_getfsmap_free_fixed_metadata(&info->meta_list);
	return error;
}

/* Do we recognize the device? */
static bool
ext4_getfsmap_is_valid_device(
	struct super_block	*sb,
	struct ext4_fsmap	*fm)
{
	if (fm->fmr_device == 0 || fm->fmr_device == UINT_MAX ||
	    fm->fmr_device == new_encode_dev(sb->s_bdev->bd_dev))
		return true;
	if (EXT4_SB(sb)->journal_bdev &&
	    fm->fmr_device == new_encode_dev(EXT4_SB(sb)->journal_bdev->bd_dev))
		return true;
	return false;
}

/* Ensure that the low key is less than the high key. */
static bool
ext4_getfsmap_check_keys(
	struct ext4_fsmap		*low_key,
	struct ext4_fsmap		*high_key)
{
	if (low_key->fmr_device > high_key->fmr_device)
		return false;
	if (low_key->fmr_device < high_key->fmr_device)
		return true;

	if (low_key->fmr_physical > high_key->fmr_physical)
		return false;
	if (low_key->fmr_physical < high_key->fmr_physical)
		return true;

	if (low_key->fmr_owner > high_key->fmr_owner)
		return false;
	if (low_key->fmr_owner < high_key->fmr_owner)
		return true;

	return false;
}

#define EXT4_GETFSMAP_DEVS	2
/*
 * Get filesystem's extents as described in head, and format for
 * output.  Calls formatter to fill the user's buffer until all
 * extents are mapped, until the passed-in head->fmh_count slots have
 * been filled, or until the formatter short-circuits the loop, if it
 * is tracking filled-in extents on its own.
 */
int
ext4_getfsmap(
	struct super_block		*sb,
	struct ext4_fsmap_head		*head,
	ext4_fsmap_format_t		formatter,
	void				*arg)
{
	struct ext4_fsmap		*rkey_low;	/* request keys */
	struct ext4_fsmap		*rkey_high;
	struct ext4_fsmap		dkeys[2];	/* per-dev keys */
	struct ext4_getfsmap_dev	handlers[EXT4_GETFSMAP_DEVS];
	struct ext4_getfsmap_info	info = {0};
	int				i;
	int				error = 0;

	if (head->fmh_iflags & ~FMH_IF_VALID)
		return -EINVAL;
	rkey_low = head->fmh_keys;
	rkey_high = rkey_low + 1;
	if (!ext4_getfsmap_is_valid_device(sb, rkey_low) ||
	    !ext4_getfsmap_is_valid_device(sb, rkey_high))
		return -EINVAL;

	head->fmh_entries = 0;

	/* Set up our device handlers. */
	memset(handlers, 0, sizeof(handlers));
	handlers[0].dev = new_encode_dev(sb->s_bdev->bd_dev);
	handlers[0].fn = ext4_getfsmap_datadev;
	if (EXT4_SB(sb)->journal_bdev) {
		handlers[1].dev = new_encode_dev(
				EXT4_SB(sb)->journal_bdev->bd_dev);
		handlers[1].fn = ext4_getfsmap_logdev;
	}

	sort(handlers, EXT4_GETFSMAP_DEVS, sizeof(struct ext4_getfsmap_dev),
			ext4_getfsmap_dev_compare, NULL);

	/*
	 * Since we allow the user to copy the last mapping from a previous
	 * call into the low key slot, we have to advance the low key by
	 * whatever the reported length is.
	 */
	dkeys[0] = *rkey_low;
	dkeys[0].fmr_physical += dkeys[0].fmr_length;
	dkeys[0].fmr_owner = 0;
	memset(&dkeys[1], 0xFF, sizeof(struct ext4_fsmap));

	if (!ext4_getfsmap_check_keys(dkeys, rkey_high))
		return -EINVAL;

	info.rkey_low = rkey_low;
	info.formatter = formatter;
	info.format_arg = arg;
	info.head = head;

	/* For each device we support... */
	for (i = 0; i < EXT4_GETFSMAP_DEVS; i++) {
		/* Is this device within the range the user asked for? */
		if (!handlers[i].fn)
			continue;
		if (rkey_low->fmr_device > handlers[i].dev)
			continue;
		if (rkey_high->fmr_device < handlers[i].dev)
			break;

		/*
		 * If this device number matches the high key, we have
		 * to pass the high key to the handler to limit the
		 * query results.  If the device number exceeds the
		 * low key, zero out the low key so that we get
		 * everything from the beginning.
		 */
		if (handlers[i].dev == rkey_high->fmr_device)
			dkeys[1] = *rkey_high;
		if (handlers[i].dev > rkey_low->fmr_device)
			memset(&dkeys[0], 0, sizeof(struct ext4_fsmap));

		info.next_fsblk = dkeys[0].fmr_physical;
		info.dev = handlers[i].dev;
		info.last = false;
		info.agno = -1;
		error = handlers[i].fn(sb, dkeys, &info);
		if (error)
			break;
	}

	head->fmh_oflags = FMH_OF_DEV_T;
	return error;
}
