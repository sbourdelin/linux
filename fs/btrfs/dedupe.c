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

struct inmem_hash {
	struct rb_node hash_node;
	struct rb_node bytenr_node;
	struct list_head lru_list;

	u64 bytenr;
	u32 num_bytes;

	u8 hash[];
};

static int init_dedupe_info(struct btrfs_dedupe_info **ret_info, u16 type,
			    u16 backend, u64 blocksize, u64 limit)
{
	struct btrfs_dedupe_info *dedupe_info;

	dedupe_info = kzalloc(sizeof(*dedupe_info), GFP_NOFS);
	if (!dedupe_info)
		return -ENOMEM;

	dedupe_info->hash_type = type;
	dedupe_info->backend = backend;
	dedupe_info->blocksize = blocksize;
	dedupe_info->limit_nr = limit;

	/* only support SHA256 yet */
	dedupe_info->dedupe_driver = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(dedupe_info->dedupe_driver)) {
		int ret;

		ret = PTR_ERR(dedupe_info->dedupe_driver);
		kfree(dedupe_info);
		return ret;
	}

	dedupe_info->hash_root = RB_ROOT;
	dedupe_info->bytenr_root = RB_ROOT;
	dedupe_info->current_nr = 0;
	INIT_LIST_HEAD(&dedupe_info->lru_list);
	mutex_init(&dedupe_info->lock);

	*ret_info = dedupe_info;
	return 0;
}

static int check_dedupe_parameter(struct btrfs_fs_info *fs_info, u16 hash_type,
				  u16 backend, u64 blocksize, u64 limit_nr,
				  u64 limit_mem, u64 *ret_limit)
{
	if (blocksize > BTRFS_DEDUPE_BLOCKSIZE_MAX ||
	    blocksize < BTRFS_DEDUPE_BLOCKSIZE_MIN ||
	    blocksize < fs_info->tree_root->sectorsize ||
	    !is_power_of_2(blocksize))
		return -EINVAL;
	/*
	 * For new backend and hash type, we return special return code
	 * as they can be easily expended.
	 */
	if (hash_type >= ARRAY_SIZE(btrfs_dedupe_sizes))
		return -EOPNOTSUPP;
	if (backend >= BTRFS_DEDUPE_BACKEND_COUNT)
		return -EOPNOTSUPP;

	/* Backend specific check */
	if (backend == BTRFS_DEDUPE_BACKEND_INMEMORY) {
		if (!limit_nr && !limit_mem)
			*ret_limit = BTRFS_DEDUPE_LIMIT_NR_DEFAULT;
		else {
			u64 tmp = (u64)-1;

			if (limit_mem) {
				tmp = limit_mem / (sizeof(struct inmem_hash) +
					btrfs_dedupe_hash_size(hash_type));
				/* Too small limit_mem to fill a hash item */
				if (!tmp)
					return -EINVAL;
			}
			if (!limit_nr)
				limit_nr = (u64)-1;

			*ret_limit = min(tmp, limit_nr);
		}
	}
	if (backend == BTRFS_DEDUPE_BACKEND_ONDISK)
		*ret_limit = 0;
	return 0;
}

int btrfs_dedupe_enable(struct btrfs_fs_info *fs_info, u16 type, u16 backend,
			u64 blocksize, u64 limit_nr, u64 limit_mem)
{
	struct btrfs_dedupe_info *dedupe_info;
	u64 limit = 0;
	int ret = 0;

	/* only one limit is accepted for enable*/
	if (limit_nr && limit_mem)
		return -EINVAL;

	ret = check_dedupe_parameter(fs_info, type, backend, blocksize,
				     limit_nr, limit_mem, &limit);
	if (ret < 0)
		return ret;

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
	ret = init_dedupe_info(&dedupe_info, type, backend, blocksize, limit);
	if (ret < 0)
		return ret;
	fs_info->dedupe_info = dedupe_info;
	/* We must ensure dedupe_enabled is modified after dedupe_info */
	smp_wmb();
	fs_info->dedupe_enabled = 1;
	return ret;
}
