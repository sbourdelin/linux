#include <linux/dax.h>
#include <linux/uio.h>
#include "ctree.h"
#include "btrfs_inode.h"
#include "extent_io.h"

static ssize_t em_dax_rw(struct inode *inode, struct extent_map *em, u64 pos,
		u64 len, struct iov_iter *iter)
{
        struct dax_device *dax_dev = fs_dax_get_by_bdev(em->bdev);
        ssize_t map_len;
        pgoff_t blk_pg;
        void *kaddr;
        sector_t blk_start;
        unsigned offset = pos & (PAGE_SIZE - 1);

        len = min(len + offset, em->len - (pos - em->start));
        len = ALIGN(len, PAGE_SIZE);
        blk_start = (get_start_sect(em->bdev) << 9) + (em->block_start + (pos - em->start));
        blk_pg = blk_start - offset;
        map_len = dax_direct_access(dax_dev, PHYS_PFN(blk_pg), PHYS_PFN(len), &kaddr, NULL);
        map_len = PFN_PHYS(map_len);
        kaddr += offset;
        map_len -= offset;
        if (map_len > len)
                map_len = len;
        if (iov_iter_rw(iter) == WRITE)
                return dax_copy_from_iter(dax_dev, blk_pg, kaddr, map_len, iter);
        else
                return dax_copy_to_iter(dax_dev, blk_pg, kaddr, map_len, iter);
}

ssize_t btrfs_file_dax_read(struct kiocb *iocb, struct iov_iter *to)
{
        size_t ret = 0, done = 0, count = iov_iter_count(to);
        struct extent_map *em;
        u64 pos = iocb->ki_pos;
        u64 end = pos + count;
        struct inode *inode = file_inode(iocb->ki_filp);

        if (!count)
                return 0;

        end = i_size_read(inode) < end ? i_size_read(inode) : end;

        while (pos < end) {
                u64 len = end - pos;

                em = btrfs_get_extent(BTRFS_I(inode), NULL, 0, pos, len, 0);
                if (IS_ERR(em)) {
                        if (!ret)
                                ret = PTR_ERR(em);
                        goto out;
                }

                BUG_ON(em->flags & EXTENT_FLAG_FS_MAPPING);

		if (em->block_start == EXTENT_MAP_HOLE) {
			u64 zero_len = min(em->len - (em->start - pos), len);
			ret = iov_iter_zero(zero_len, to);
		} else {
			ret = em_dax_rw(inode, em, pos, len, to);
		}
                if (ret < 0)
                        goto out;
                pos += ret;
                done += ret;
        }

out:
        iocb->ki_pos += done;
        return done ? done : ret;
}

static int copy_extent_page(struct extent_map *em, void *daddr, u64 pos)
{
        struct dax_device *dax_dev;
	void *saddr;
	sector_t start;
	size_t len;

	if (em->block_start == EXTENT_MAP_HOLE) {
		memset(daddr, 0, PAGE_SIZE);
	} else {
		dax_dev = fs_dax_get_by_bdev(em->bdev);
		start = (get_start_sect(em->bdev) << 9) + (em->block_start + (pos - em->start));
		len = dax_direct_access(dax_dev, PHYS_PFN(start), 1, &saddr, NULL);
		memcpy(daddr, saddr, PAGE_SIZE);
	}
	free_extent_map(em);

	return 0;
}


ssize_t btrfs_file_dax_write(struct kiocb *iocb, struct iov_iter *from)
{
	ssize_t ret, done = 0, count = iov_iter_count(from);
        struct inode *inode = file_inode(iocb->ki_filp);
	u64 pos = iocb->ki_pos;
	u64 start = round_down(pos, PAGE_SIZE);
	u64 end = round_up(pos + count, PAGE_SIZE);
	struct extent_state *cached_state = NULL;
	struct extent_changeset *data_reserved = NULL;
	struct extent_map *first = NULL, *last = NULL;

	ret = btrfs_delalloc_reserve_space(inode, &data_reserved, start, end - start);
	if (ret < 0)
		return ret;

	/* Grab a reference of the first extent to copy data */
	if (start < pos) {
		first = btrfs_get_extent(BTRFS_I(inode), NULL, 0, start, end - start, 0);
		if (IS_ERR(first)) {
			ret = PTR_ERR(first);
			goto out2;
		}
	}

	/* Grab a reference of the last extent to copy data */
	if (pos + count < end) {
		last = btrfs_get_extent(BTRFS_I(inode), NULL, 0, end - PAGE_SIZE, PAGE_SIZE, 0);
		if (IS_ERR(last)) {
			ret = PTR_ERR(last);
			goto out2;
		}
	}

	lock_extent_bits(&BTRFS_I(inode)->io_tree, start, end, &cached_state);
	while (done < count) {
		struct extent_map *em;
		struct dax_device *dax_dev;
		int offset = pos & (PAGE_SIZE - 1);
		u64 estart = round_down(pos, PAGE_SIZE);
		u64 elen = end - estart;
		size_t len = count - done;
		sector_t dstart;
		void *daddr;
		ssize_t maplen;

		/* Read the current extent */
                em = btrfs_get_extent(BTRFS_I(inode), NULL, 0, estart, elen, 0);
		if (IS_ERR(em)) {
			ret = PTR_ERR(em);
			goto out;
		}

		/* Get a new extent */
		ret = btrfs_get_extent_map_write(&em, NULL, inode, estart, elen);
		if (ret < 0)
			goto out;

		dax_dev = fs_dax_get_by_bdev(em->bdev);
		/* Calculate start address start of destination extent */
		dstart = (get_start_sect(em->bdev) << 9) + em->block_start;
		maplen = dax_direct_access(dax_dev, PHYS_PFN(dstart),
				PHYS_PFN(em->len), &daddr, NULL);

		/* Copy front of extent page */
		if (offset)
			ret = copy_extent_page(first, daddr, estart);

		/* Copy end of extent page */
		if ((pos + len > estart + PAGE_SIZE) && (pos + len < em->start + em->len))
			ret = copy_extent_page(last, daddr + em->len - PAGE_SIZE, em->start + em->len - PAGE_SIZE);

		/* Copy the data from the iter */
		maplen = PFN_PHYS(maplen);
		maplen -= offset;
		ret = dax_copy_from_iter(dax_dev, dstart, daddr + offset, maplen, from);
		if (ret < 0)
			goto out;
		pos += ret;
		done += ret;
	}
out:
	unlock_extent_cached(&BTRFS_I(inode)->io_tree, start, end, &cached_state);
	if (done) {
		btrfs_update_ordered_extent(inode, start,
				end - start, true);
		iocb->ki_pos += done;
		if (iocb->ki_pos > i_size_read(inode))
			i_size_write(inode, iocb->ki_pos);
	}

	btrfs_delalloc_release_extents(BTRFS_I(inode), count, false);
out2:
	if (count - done > 0)
		btrfs_delalloc_release_space(inode, data_reserved, pos,
				count - done, true);
	extent_changeset_free(data_reserved);
        return done ? done : ret;

}
