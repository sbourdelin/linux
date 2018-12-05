#include <linux/dax.h>
#include <linux/uio.h>
#include "ctree.h"
#include "btrfs_inode.h"

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

