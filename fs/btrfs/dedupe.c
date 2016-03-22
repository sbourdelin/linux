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

static inline struct inmem_hash *inmem_alloc_hash(u16 type)
{
	if (WARN_ON(type >= ARRAY_SIZE(btrfs_dedupe_sizes)))
		return NULL;
	return kzalloc(sizeof(struct inmem_hash) + btrfs_dedupe_sizes[type],
			GFP_NOFS);
}

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

static int inmem_insert_hash(struct rb_root *root,
			     struct inmem_hash *hash, int hash_len)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct inmem_hash *entry = NULL;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct inmem_hash, hash_node);
		if (memcmp(hash->hash, entry->hash, hash_len) < 0)
			p = &(*p)->rb_left;
		else if (memcmp(hash->hash, entry->hash, hash_len) > 0)
			p = &(*p)->rb_right;
		else
			return 1;
	}
	rb_link_node(&hash->hash_node, parent, p);
	rb_insert_color(&hash->hash_node, root);
	return 0;
}

static int inmem_insert_bytenr(struct rb_root *root,
			       struct inmem_hash *hash)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct inmem_hash *entry = NULL;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct inmem_hash, bytenr_node);
		if (hash->bytenr < entry->bytenr)
			p = &(*p)->rb_left;
		else if (hash->bytenr > entry->bytenr)
			p = &(*p)->rb_right;
		else
			return 1;
	}
	rb_link_node(&hash->bytenr_node, parent, p);
	rb_insert_color(&hash->bytenr_node, root);
	return 0;
}

static void __inmem_del(struct btrfs_dedupe_info *dedupe_info,
			struct inmem_hash *hash)
{
	list_del(&hash->lru_list);
	rb_erase(&hash->hash_node, &dedupe_info->hash_root);
	rb_erase(&hash->bytenr_node, &dedupe_info->bytenr_root);

	if (!WARN_ON(dedupe_info->current_nr == 0))
		dedupe_info->current_nr--;

	kfree(hash);
}

/*
 * Insert a hash into in-memory dedupe tree
 * Will remove exceeding last recent use hash.
 *
 * If the hash mathced with existing one, we won't insert it, to
 * save memory
 */
static int inmem_add(struct btrfs_dedupe_info *dedupe_info,
		     struct btrfs_dedupe_hash *hash)
{
	int ret = 0;
	u16 type = dedupe_info->hash_type;
	struct inmem_hash *ihash;

	ihash = inmem_alloc_hash(type);

	if (!ihash)
		return -ENOMEM;

	/* Copy the data out */
	ihash->bytenr = hash->bytenr;
	ihash->num_bytes = hash->num_bytes;
	memcpy(ihash->hash, hash->hash, btrfs_dedupe_sizes[type]);

	mutex_lock(&dedupe_info->lock);

	ret = inmem_insert_bytenr(&dedupe_info->bytenr_root, ihash);
	if (ret > 0) {
		kfree(ihash);
		ret = 0;
		goto out;
	}

	ret = inmem_insert_hash(&dedupe_info->hash_root, ihash,
				btrfs_dedupe_sizes[type]);
	if (ret > 0) {
		/*
		 * We only keep one hash in tree to save memory, so if
		 * hash conflicts, free the one to insert.
		 */
		rb_erase(&ihash->bytenr_node, &dedupe_info->bytenr_root);
		kfree(ihash);
		ret = 0;
		goto out;
	}

	list_add(&ihash->lru_list, &dedupe_info->lru_list);
	dedupe_info->current_nr++;

	/* Remove the last dedupe hash if we exceed limit */
	while (dedupe_info->current_nr > dedupe_info->limit_nr) {
		struct inmem_hash *last;

		last = list_entry(dedupe_info->lru_list.prev,
				  struct inmem_hash, lru_list);
		__inmem_del(dedupe_info, last);
	}
out:
	mutex_unlock(&dedupe_info->lock);
	return 0;
}

int btrfs_dedupe_add(struct btrfs_trans_handle *trans,
		     struct btrfs_fs_info *fs_info,
		     struct btrfs_dedupe_hash *hash)
{
	struct btrfs_dedupe_info *dedupe_info = fs_info->dedupe_info;

	if (!fs_info->dedupe_enabled || !hash)
		return 0;

	if (WARN_ON(dedupe_info == NULL))
		return -EINVAL;

	if (WARN_ON(!btrfs_dedupe_hash_hit(hash)))
		return -EINVAL;

	/* ignore old hash */
	if (dedupe_info->blocksize != hash->num_bytes)
		return 0;

	if (dedupe_info->backend == BTRFS_DEDUPE_BACKEND_INMEMORY)
		return inmem_add(dedupe_info, hash);
	return -EINVAL;
}

static struct inmem_hash *
inmem_search_bytenr(struct btrfs_dedupe_info *dedupe_info, u64 bytenr)
{
	struct rb_node **p = &dedupe_info->bytenr_root.rb_node;
	struct rb_node *parent = NULL;
	struct inmem_hash *entry = NULL;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct inmem_hash, bytenr_node);

		if (bytenr < entry->bytenr)
			p = &(*p)->rb_left;
		else if (bytenr > entry->bytenr)
			p = &(*p)->rb_right;
		else
			return entry;
	}

	return NULL;
}

/* Delete a hash from in-memory dedupe tree */
static int inmem_del(struct btrfs_dedupe_info *dedupe_info, u64 bytenr)
{
	struct inmem_hash *hash;

	mutex_lock(&dedupe_info->lock);
	hash = inmem_search_bytenr(dedupe_info, bytenr);
	if (!hash) {
		mutex_unlock(&dedupe_info->lock);
		return 0;
	}

	__inmem_del(dedupe_info, hash);
	mutex_unlock(&dedupe_info->lock);
	return 0;
}

/* Remove a dedupe hash from dedupe tree */
int btrfs_dedupe_del(struct btrfs_trans_handle *trans,
		     struct btrfs_fs_info *fs_info, u64 bytenr)
{
	struct btrfs_dedupe_info *dedupe_info = fs_info->dedupe_info;

	if (!fs_info->dedupe_enabled)
		return 0;

	if (WARN_ON(dedupe_info == NULL))
		return -EINVAL;

	if (dedupe_info->backend == BTRFS_DEDUPE_BACKEND_INMEMORY)
		return inmem_del(dedupe_info, bytenr);
	return -EINVAL;
}

static void inmem_destroy(struct btrfs_dedupe_info *dedupe_info)
{
	struct inmem_hash *entry, *tmp;

	mutex_lock(&dedupe_info->lock);
	list_for_each_entry_safe(entry, tmp, &dedupe_info->lru_list, lru_list)
		__inmem_del(dedupe_info, entry);
	mutex_unlock(&dedupe_info->lock);
}

int btrfs_dedupe_disable(struct btrfs_fs_info *fs_info)
{
	struct btrfs_dedupe_info *dedupe_info;
	int ret;

	/* Here we don't want to increase refs of dedupe_info */
	fs_info->dedupe_enabled = 0;

	dedupe_info = fs_info->dedupe_info;

	if (!dedupe_info)
		return 0;

	/* Don't allow disable status change in RO mount */
	if (fs_info->sb->s_flags & MS_RDONLY)
		return -EROFS;

	/*
	 * Wait for all unfinished write to complete dedupe routine
	 * As disable operation is not a frequent operation, we are
	 * OK to use heavy but safe sync_filesystem().
	 */
	down_read(&fs_info->sb->s_umount);
	ret = sync_filesystem(fs_info->sb);
	up_read(&fs_info->sb->s_umount);
	if (ret < 0)
		return ret;

	fs_info->dedupe_info = NULL;

	/* now we are OK to clean up everything */
	if (dedupe_info->backend == BTRFS_DEDUPE_BACKEND_INMEMORY)
		inmem_destroy(dedupe_info);

	crypto_free_shash(dedupe_info->dedupe_driver);
	kfree(dedupe_info);
	return 0;
}
