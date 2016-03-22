/*
 * Copyright (C) 2016 Fujitsu.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */
#include "ctree.h"
#include "dedupe.h"
#include "btrfs_inode.h"
#include "transaction.h"
#include "delayed-ref.h"

int btrfs_dedupe_enable(struct btrfs_fs_info *fs_info, u16 type, u16 backend,
			u64 blocksize, u64 limit_nr)
{
	struct btrfs_dedupe_info *dedupe_info;
	u64 limit = limit_nr;
	int ret = 0;

	/* Sanity check */
	if (blocksize > BTRFS_DEDUPE_BLOCKSIZE_MAX ||
	    blocksize < BTRFS_DEDUPE_BLOCKSIZE_MIN ||
	    blocksize < fs_info->tree_root->sectorsize ||
	    !is_power_of_2(blocksize))
		return -EINVAL;
	if (type >= ARRAY_SIZE(btrfs_dedupe_sizes))
		return -EINVAL;
	if (backend >= BTRFS_DEDUPE_BACKEND_COUNT)
		return -EINVAL;

	if (backend == BTRFS_DEDUPE_BACKEND_INMEMORY && limit_nr == 0)
		limit = BTRFS_DEDUPE_LIMIT_NR_DEFAULT;
	if (backend == BTRFS_DEDUPE_BACKEND_ONDISK && limit_nr != 0)
		limit = 0;

	dedupe_info = fs_info->dedupe_info;
	if (dedupe_info) {
		/* Check if we are re-enable for different dedupe config */
		if (dedupe_info->blocksize != blocksize ||
		    dedupe_info->hash_type != type ||
		    dedupe_info->backend != backend) {
			btrfs_dedupe_disable(fs_info);
			goto enable;
		}

		/* On-fly limit change is OK */
		mutex_lock(&dedupe_info->lock);
		fs_info->dedupe_info->limit_nr = limit;
		mutex_unlock(&dedupe_info->lock);
		return 0;
	}

enable:
	dedupe_info = kzalloc(sizeof(*dedupe_info), GFP_NOFS);
	if (dedupe_info)
		return -ENOMEM;

	dedupe_info->hash_type = type;
	dedupe_info->backend = backend;
	dedupe_info->blocksize = blocksize;
	dedupe_info->limit_nr = limit;

	/* Only support SHA256 yet */
	dedupe_info->dedupe_driver = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(dedupe_info->dedupe_driver)) {
		btrfs_err(fs_info, "failed to init sha256 driver");
		ret = PTR_ERR(dedupe_info->dedupe_driver);
		goto out;
	}

	dedupe_info->hash_root = RB_ROOT;
	dedupe_info->bytenr_root = RB_ROOT;
	dedupe_info->current_nr = 0;
	INIT_LIST_HEAD(&dedupe_info->lru_list);
	mutex_init(&dedupe_info->lock);

	fs_info->dedupe_info = dedupe_info;
	/* We must ensure dedupe_enabled is modified after dedupe_info */
	smp_wmb();
	fs_info->dedupe_enabled = 1;

out:
	if (ret < 0)
		kfree(dedupe_info);
	return ret;
}
