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
#include "qgroup.h"
#include "disk-io.h"
#include "locking.h"

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

static int init_dedupe_tree(struct btrfs_fs_info *fs_info,
			    struct btrfs_dedupe_info *dedupe_info)
{
	struct btrfs_root *dedupe_root;
	struct btrfs_key key;
	struct btrfs_path *path;
	struct btrfs_dedupe_status_item *status;
	struct btrfs_trans_handle *trans;
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	trans = btrfs_start_transaction(fs_info->tree_root, 2);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		goto out;
	}
	dedupe_root = btrfs_create_tree(trans, fs_info,
				       BTRFS_DEDUPE_TREE_OBJECTID);
	if (IS_ERR(dedupe_root)) {
		ret = PTR_ERR(dedupe_root);
		btrfs_abort_transaction(trans, fs_info->tree_root, ret);
		goto out;
	}
	dedupe_info->dedupe_root = dedupe_root;

	key.objectid = 0;
	key.type = BTRFS_DEDUPE_STATUS_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_insert_empty_item(trans, dedupe_root, path, &key,
				      sizeof(*status));
	if (ret < 0) {
		btrfs_abort_transaction(trans, fs_info->tree_root, ret);
		goto out;
	}

	status = btrfs_item_ptr(path->nodes[0], path->slots[0],
				struct btrfs_dedupe_status_item);
	btrfs_set_dedupe_status_blocksize(path->nodes[0], status,
					 dedupe_info->blocksize);
	btrfs_set_dedupe_status_limit(path->nodes[0], status,
			dedupe_info->limit_nr);
	btrfs_set_dedupe_status_hash_type(path->nodes[0], status,
			dedupe_info->hash_type);
	btrfs_set_dedupe_status_backend(path->nodes[0], status,
			dedupe_info->backend);
	btrfs_mark_buffer_dirty(path->nodes[0]);
out:
	btrfs_free_path(path);
	if (ret == 0)
		btrfs_commit_transaction(trans, fs_info->tree_root);
	return ret;
}

int btrfs_dedupe_enable(struct btrfs_fs_info *fs_info, u16 type, u16 backend,
			u64 blocksize, u64 limit_nr, u64 limit_mem)
{
	struct btrfs_dedupe_info *dedupe_info;
	int create_tree;
	u64 compat_ro_flag = btrfs_super_compat_ro_flags(fs_info->super_copy);
	u64 limit;
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
	/* Only one limit is accept */
	if (limit_nr && limit_mem)
		return -EINVAL;

	if (backend == BTRFS_DEDUPE_BACKEND_INMEMORY) {
		if (!limit_nr && !limit_mem)
			limit = BTRFS_DEDUPE_LIMIT_NR_DEFAULT;
		else if (limit_nr)
			limit = limit_nr;
		else
			limit = limit_mem / (sizeof(struct inmem_hash) +
					btrfs_dedupe_sizes[type]);
	}
	if (backend == BTRFS_DEDUPE_BACKEND_ONDISK)
		limit = 0;

	/* Ondisk backend needs DEDUP RO compat feature */
	if (!(compat_ro_flag & BTRFS_FEATURE_COMPAT_RO_DEDUPE) &&
	    backend == BTRFS_DEDUPE_BACKEND_ONDISK)
		return -EOPNOTSUPP;

	/* Meaningless and unable to enable dedupe for RO fs */
	if (fs_info->sb->s_flags & MS_RDONLY)
		return -EROFS;

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

	dedupe_info = NULL;
enable:
	create_tree = compat_ro_flag & BTRFS_FEATURE_COMPAT_RO_DEDUPE;

	ret = init_dedupe_info(&dedupe_info, type, backend, blocksize, limit);
	if (ret < 0)
		return ret;
	if (create_tree) {
		ret = init_dedupe_tree(fs_info, dedupe_info);
		if (ret < 0)
			goto out;
	}

	fs_info->dedupe_info = dedupe_info;
	/* We must ensure dedupe_enabled is modified after dedupe_info */
	smp_wmb();
	fs_info->dedupe_enabled = 1;
out:
	if (ret < 0) {
		crypto_free_shash(dedupe_info->dedupe_driver);
		kfree(dedupe_info);
	}
	return ret;
}

void btrfs_dedupe_status(struct btrfs_fs_info *fs_info,
			 struct btrfs_ioctl_dedupe_args *dargs)
{
	struct btrfs_dedupe_info *dedupe_info = fs_info->dedupe_info;

	if (!fs_info->dedupe_enabled || !dedupe_info) {
		dargs->status = 0;
		dargs->blocksize = 0;
		dargs->backend = 0;
		dargs->hash_type = 0;
		dargs->limit_nr = 0;
		dargs->current_nr = 0;
		return;
	}
	mutex_lock(&dedupe_info->lock);
	dargs->status = 1;
	dargs->blocksize = dedupe_info->blocksize;
	dargs->backend = dedupe_info->backend;
	dargs->hash_type = dedupe_info->hash_type;
	dargs->limit_nr = dedupe_info->limit_nr;
	dargs->limit_mem = dedupe_info->limit_nr *
		(sizeof(struct inmem_hash) +
		 btrfs_dedupe_sizes[dedupe_info->hash_type]);
	dargs->current_nr = dedupe_info->current_nr;
	mutex_unlock(&dedupe_info->lock);
}

int btrfs_dedupe_resume(struct btrfs_fs_info *fs_info,
			struct btrfs_root *dedupe_root)
{
	struct btrfs_dedupe_info *dedupe_info;
	struct btrfs_dedupe_status_item *status;
	struct btrfs_key key;
	struct btrfs_path *path;
	u64 blocksize;
	u64 limit;
	u16 type;
	u16 backend;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = 0;
	key.type = BTRFS_DEDUPE_STATUS_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, dedupe_root, &key, path, 0, 0);
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	} else if (ret < 0) {
		goto out;
	}

	status = btrfs_item_ptr(path->nodes[0], path->slots[0],
				struct btrfs_dedupe_status_item);
	blocksize = btrfs_dedupe_status_blocksize(path->nodes[0], status);
	limit = btrfs_dedupe_status_limit(path->nodes[0], status);
	type = btrfs_dedupe_status_hash_type(path->nodes[0], status);
	backend = btrfs_dedupe_status_backend(path->nodes[0], status);

	ret = init_dedupe_info(&dedupe_info, type, backend, blocksize, limit);
	if (ret < 0)
		goto out;
	dedupe_info->dedupe_root = dedupe_root;

	fs_info->dedupe_info = dedupe_info;
	/* We must ensure dedupe_enabled is modified after dedupe_info */
	smp_wmb();
	fs_info->dedupe_enabled = 1;

out:
	btrfs_free_path(path);
	return ret;
}

static void inmem_destroy(struct btrfs_dedupe_info *dedupe_info);
int btrfs_dedupe_cleanup(struct btrfs_fs_info *fs_info)
{
	struct btrfs_dedupe_info *dedupe_info;

	fs_info->dedupe_enabled = 0;

	/* same as disable */
	smp_wmb();
	dedupe_info = fs_info->dedupe_info;
	fs_info->dedupe_info = NULL;

	if (!dedupe_info)
		return 0;

	if (dedupe_info->backend == BTRFS_DEDUPE_BACKEND_INMEMORY)
		inmem_destroy(dedupe_info);
	if (dedupe_info->dedupe_root) {
		free_root_extent_buffers(dedupe_info->dedupe_root);
		kfree(dedupe_info->dedupe_root);
	}
	crypto_free_shash(dedupe_info->dedupe_driver);
	kfree(dedupe_info);
	return 0;
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

static int ondisk_search_bytenr(struct btrfs_trans_handle *trans,
				struct btrfs_dedupe_info *dedupe_info,
				struct btrfs_path *path, u64 bytenr,
				int prepare_del);
static int ondisk_search_hash(struct btrfs_dedupe_info *dedupe_info, u8 *hash,
			      u64 *bytenr_ret, u32 *num_bytes_ret);
static int ondisk_add(struct btrfs_trans_handle *trans,
		      struct btrfs_dedupe_info *dedupe_info,
		      struct btrfs_dedupe_hash *hash)
{
	struct btrfs_path *path;
	struct btrfs_root *dedupe_root = dedupe_info->dedupe_root;
	struct btrfs_key key;
	struct btrfs_dedupe_hash_item *hash_item;
	u64 bytenr;
	u32 num_bytes;
	int hash_len = btrfs_dedupe_sizes[dedupe_info->hash_type];
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	mutex_lock(&dedupe_info->lock);

	ret = ondisk_search_bytenr(NULL, dedupe_info, path, hash->bytenr, 0);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = 0;
		goto out;
	}
	btrfs_release_path(path);

	ret = ondisk_search_hash(dedupe_info, hash->hash, &bytenr, &num_bytes);
	if (ret < 0)
		goto out;
	/* Same hash found, don't re-add to save dedupe tree space */
	if (ret > 0) {
		ret = 0;
		goto out;
	}

	/* Insert hash->bytenr item */
	memcpy(&key.objectid, hash->hash + hash_len - 8, 8);
	key.type = BTRFS_DEDUPE_HASH_ITEM_KEY;
	key.offset = hash->bytenr;

	ret = btrfs_insert_empty_item(trans, dedupe_root, path, &key,
			sizeof(*hash_item) + hash_len);
	WARN_ON(ret == -EEXIST);
	if (ret < 0)
		goto out;
	hash_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
				   struct btrfs_dedupe_hash_item);
	btrfs_set_dedupe_hash_len(path->nodes[0], hash_item, hash->num_bytes);
	write_extent_buffer(path->nodes[0], hash->hash,
			    (unsigned long)(hash_item + 1), hash_len);
	btrfs_mark_buffer_dirty(path->nodes[0]);
	btrfs_release_path(path);

	/* Then bytenr->hash item */
	key.objectid = hash->bytenr;
	key.type = BTRFS_DEDUPE_BYTENR_ITEM_KEY;
	memcpy(&key.offset, hash->hash + hash_len - 8, 8);

	ret = btrfs_insert_empty_item(trans, dedupe_root, path, &key, hash_len);
	WARN_ON(ret == -EEXIST);
	if (ret < 0)
		goto out;
	write_extent_buffer(path->nodes[0], hash->hash,
			btrfs_item_ptr_offset(path->nodes[0], path->slots[0]),
			hash_len);
	btrfs_mark_buffer_dirty(path->nodes[0]);

out:
	mutex_unlock(&dedupe_info->lock);
	btrfs_free_path(path);
	return ret;
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
	if (dedupe_info->backend == BTRFS_DEDUPE_BACKEND_ONDISK)
		return ondisk_add(trans, dedupe_info, hash);
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

/*
 * If prepare_del is given, this will setup search_slot() for delete.
 * Caller needs to do proper locking.
 *
 * Return > 0 for found.
 * Return 0 for not found.
 * Return < 0 for error.
 */
static int ondisk_search_bytenr(struct btrfs_trans_handle *trans,
				struct btrfs_dedupe_info *dedupe_info,
				struct btrfs_path *path, u64 bytenr,
				int prepare_del)
{
	struct btrfs_key key;
	struct btrfs_root *dedupe_root = dedupe_info->dedupe_root;
	int ret;
	int ins_len = 0;
	int cow = 0;

	if (prepare_del) {
		if (WARN_ON(trans == NULL))
			return -EINVAL;
		cow = 1;
		ins_len = -1;
	}

	key.objectid = bytenr;
	key.type = BTRFS_DEDUPE_BYTENR_ITEM_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(trans, dedupe_root, &key, path,
				ins_len, cow);

	if (ret < 0)
		return ret;
	/*
	 * Although it's almost impossible, it's still possible that
	 * the last 64bits are all 1.
	 */
	if (ret == 0)
		return 1;

	ret = btrfs_previous_item(dedupe_root, path, bytenr,
				  BTRFS_DEDUPE_BYTENR_ITEM_KEY);
	if (ret < 0)
		return ret;
	if (ret > 0)
		return 0;
	return 1;
}

static int ondisk_del(struct btrfs_trans_handle *trans,
		      struct btrfs_dedupe_info *dedupe_info, u64 bytenr)
{
	struct btrfs_root *dedupe_root = dedupe_info->dedupe_root;
	struct btrfs_path *path;
	struct btrfs_key key;
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = bytenr;
	key.type = BTRFS_DEDUPE_BYTENR_ITEM_KEY;
	key.offset = 0;

	mutex_lock(&dedupe_info->lock);

	ret = ondisk_search_bytenr(trans, dedupe_info, path, bytenr, 1);
	if (ret <= 0)
		goto out;

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	ret = btrfs_del_item(trans, dedupe_root, path);
	btrfs_release_path(path);
	if (ret < 0)
		goto out;
	/* Search for hash item and delete it */
	key.objectid = key.offset;
	key.type = BTRFS_DEDUPE_HASH_ITEM_KEY;
	key.offset = bytenr;

	ret = btrfs_search_slot(trans, dedupe_root, &key, path, -1, 1);
	if (WARN_ON(ret > 0)) {
		ret = -ENOENT;
		goto out;
	}
	if (ret < 0)
		goto out;
	ret = btrfs_del_item(trans, dedupe_root, path);

out:
	btrfs_free_path(path);
	mutex_unlock(&dedupe_info->lock);
	return ret;
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
	if (dedupe_info->backend == BTRFS_DEDUPE_BACKEND_ONDISK)
		return ondisk_del(trans, dedupe_info, bytenr);
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

static int remove_dedupe_tree(struct btrfs_root *dedupe_root)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_fs_info *fs_info = dedupe_root->fs_info;
	struct btrfs_path *path;
	struct btrfs_key key;
	struct extent_buffer *node;
	int ret;
	int nr;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
	trans = btrfs_start_transaction(fs_info->tree_root, 2);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		goto out;
	}

	path->leave_spinning = 1;
	key.objectid = 0;
	key.offset = 0;
	key.type = 0;

	while (1) {
		ret = btrfs_search_slot(trans, dedupe_root, &key, path, -1, 1);
		if (ret < 0)
			goto out;
		node = path->nodes[0];
		nr = btrfs_header_nritems(node);
		if (nr == 0) {
			btrfs_release_path(path);
			break;
		}
		path->slots[0] = 0;
		ret = btrfs_del_items(trans, dedupe_root, path, 0, nr);
		if (ret)
			goto out;
		btrfs_release_path(path);
	}

	ret = btrfs_del_root(trans, fs_info->tree_root, &dedupe_root->root_key);
	if (ret)
		goto out;

	list_del(&dedupe_root->dirty_list);
	btrfs_tree_lock(dedupe_root->node);
	clean_tree_block(trans, fs_info, dedupe_root->node);
	btrfs_tree_unlock(dedupe_root->node);
	btrfs_free_tree_block(trans, dedupe_root, dedupe_root->node, 0, 1);
	free_extent_buffer(dedupe_root->node);
	free_extent_buffer(dedupe_root->commit_root);
	kfree(dedupe_root);
	ret = btrfs_commit_transaction(trans, fs_info->tree_root);
out:
	btrfs_free_path(path);
	return ret;
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
	if (dedupe_info->dedupe_root)
		ret = remove_dedupe_tree(dedupe_info->dedupe_root);

	crypto_free_shash(dedupe_info->dedupe_driver);
	kfree(dedupe_info);
	return ret;
}

 /*
 * Return 0 for not found
 * Return >0 for found and set bytenr_ret
 * Return <0 for error
 */
static int ondisk_search_hash(struct btrfs_dedupe_info *dedupe_info, u8 *hash,
			      u64 *bytenr_ret, u32 *num_bytes_ret)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	struct btrfs_root *dedupe_root = dedupe_info->dedupe_root;
	u8 *buf = NULL;
	u64 hash_key;
	int hash_len = btrfs_dedupe_sizes[dedupe_info->hash_type];
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	buf = kmalloc(hash_len, GFP_NOFS);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy(&hash_key, hash + hash_len - 8, 8);
	key.objectid = hash_key;
	key.type = BTRFS_DEDUPE_HASH_ITEM_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, dedupe_root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	WARN_ON(ret == 0);
	while (1) {
		struct extent_buffer *node;
		struct btrfs_dedupe_hash_item *hash_item;
		int slot;

		ret = btrfs_previous_item(dedupe_root, path, hash_key,
					  BTRFS_DEDUPE_HASH_ITEM_KEY);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = 0;
			goto out;
		}

		node = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(node, &key, slot);

		if (key.type != BTRFS_DEDUPE_HASH_ITEM_KEY ||
		    memcmp(&key.objectid, hash + hash_len - 8, 8))
			break;
		hash_item = btrfs_item_ptr(node, slot,
				struct btrfs_dedupe_hash_item);
		read_extent_buffer(node, buf, (unsigned long)(hash_item + 1),
				   hash_len);
		if (!memcmp(buf, hash, hash_len)) {
			ret = 1;
			*bytenr_ret = key.offset;
			*num_bytes_ret = btrfs_dedupe_hash_len(node, hash_item);
			break;
		}
	}
out:
	kfree(buf);
	btrfs_free_path(path);
	return ret;
}

/*
 * Caller must ensure the corresponding ref head is not being run.
 */
static struct inmem_hash *
inmem_search_hash(struct btrfs_dedupe_info *dedupe_info, u8 *hash)
{
	struct rb_node **p = &dedupe_info->hash_root.rb_node;
	struct rb_node *parent = NULL;
	struct inmem_hash *entry = NULL;
	u16 hash_type = dedupe_info->hash_type;
	int hash_len = btrfs_dedupe_sizes[hash_type];

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct inmem_hash, hash_node);

		if (memcmp(hash, entry->hash, hash_len) < 0) {
			p = &(*p)->rb_left;
		} else if (memcmp(hash, entry->hash, hash_len) > 0) {
			p = &(*p)->rb_right;
		} else {
			/* Found, need to re-add it to LRU list head */
			list_del(&entry->lru_list);
			list_add(&entry->lru_list, &dedupe_info->lru_list);
			return entry;
		}
	}
	return NULL;
}

/* Wapper for different backends, caller needs to hold dedupe_info->lock */
static inline int generic_search_hash(struct btrfs_dedupe_info *dedupe_info,
				      u8 *hash, u64 *bytenr_ret,
				      u32 *num_bytes_ret)
{
	if (dedupe_info->backend == BTRFS_DEDUPE_BACKEND_INMEMORY) {
		struct inmem_hash *found_hash;
		int ret;

		found_hash = inmem_search_hash(dedupe_info, hash);
		if (found_hash) {
			ret = 1;
			*bytenr_ret = found_hash->bytenr;
			*num_bytes_ret = found_hash->num_bytes;
		} else {
			ret = 0;
			*bytenr_ret = 0;
			*num_bytes_ret = 0;
		}
		return ret;
	} else if (dedupe_info->backend == BTRFS_DEDUPE_BACKEND_ONDISK) {
		return ondisk_search_hash(dedupe_info, hash, bytenr_ret,
					  num_bytes_ret);
	}
	return -EINVAL;
}

static int generic_search(struct btrfs_dedupe_info *dedupe_info,
			  struct inode *inode, u64 file_pos,
			  struct btrfs_dedupe_hash *hash)
{
	int ret;
	struct btrfs_root *root = BTRFS_I(inode)->root;
	struct btrfs_trans_handle *trans;
	struct btrfs_delayed_ref_root *delayed_refs;
	struct btrfs_delayed_ref_head *head;
	struct btrfs_delayed_ref_head *insert_head;
	struct btrfs_delayed_data_ref *insert_dref;
	struct btrfs_qgroup_extent_record *insert_qrecord = NULL;
	int free_insert = 1;
	u64 bytenr;
	u64 tmp_bytenr;
	u32 num_bytes;

	insert_head = kmem_cache_alloc(btrfs_delayed_ref_head_cachep, GFP_NOFS);
	if (!insert_head)
		return -ENOMEM;
	insert_head->extent_op = NULL;
	insert_dref = kmem_cache_alloc(btrfs_delayed_data_ref_cachep, GFP_NOFS);
	if (!insert_dref) {
		kmem_cache_free(btrfs_delayed_ref_head_cachep, insert_head);
		return -ENOMEM;
	}
	if (root->fs_info->quota_enabled &&
	    is_fstree(root->root_key.objectid)) {
		insert_qrecord = kmalloc(sizeof(*insert_qrecord), GFP_NOFS);
		if (!insert_qrecord) {
			kmem_cache_free(btrfs_delayed_ref_head_cachep,
					insert_head);
			kmem_cache_free(btrfs_delayed_data_ref_cachep,
					insert_dref);
			return -ENOMEM;
		}
	}

	trans = btrfs_join_transaction(root);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		goto free_mem;
	}

again:
	mutex_lock(&dedupe_info->lock);
	ret = generic_search_hash(dedupe_info, hash->hash, &bytenr, &num_bytes);
	if (ret <= 0)
		goto out;

	delayed_refs = &trans->transaction->delayed_refs;

	spin_lock(&delayed_refs->lock);
	head = btrfs_find_delayed_ref_head(trans, bytenr);
	if (!head) {
		/*
		 * We can safely insert a new delayed_ref as long as we
		 * hold delayed_refs->lock.
		 * Only need to use atomic inc_extent_ref()
		 */
		btrfs_add_delayed_data_ref_locked(root->fs_info, trans,
				insert_dref, insert_head, insert_qrecord,
				bytenr, num_bytes, 0, root->root_key.objectid,
				btrfs_ino(inode), file_pos, 0,
				BTRFS_ADD_DELAYED_REF);
		spin_unlock(&delayed_refs->lock);

		/* add_delayed_data_ref_locked will free unused memory */
		free_insert = 0;
		hash->bytenr = bytenr;
		hash->num_bytes = num_bytes;
		ret = 1;
		goto out;
	}

	/*
	 * We can't lock ref head with dedupe_info->lock hold or we will cause
	 * ABBA dead lock.
	 */
	mutex_unlock(&dedupe_info->lock);
	ret = btrfs_delayed_ref_lock(trans, head);
	spin_unlock(&delayed_refs->lock);
	if (ret == -EAGAIN)
		goto again;

	mutex_lock(&dedupe_info->lock);
	/* Search again to ensure the hash is still here */
	ret = generic_search_hash(dedupe_info, hash->hash, &tmp_bytenr,
				  &num_bytes);
	if (ret <= 0) {
		mutex_unlock(&head->mutex);
		goto out;
	}
	if (tmp_bytenr != bytenr) {
		mutex_unlock(&head->mutex);
		mutex_unlock(&dedupe_info->lock);
		goto again;
	}
	hash->bytenr = bytenr;
	hash->num_bytes = num_bytes;

	/*
	 * Increase the extent ref right now, to avoid delayed ref run
	 * Or we may increase ref on non-exist extent.
	 */
	btrfs_inc_extent_ref(trans, root, bytenr, num_bytes, 0,
			     root->root_key.objectid,
			     btrfs_ino(inode), file_pos);
	mutex_unlock(&head->mutex);
out:
	mutex_unlock(&dedupe_info->lock);
	btrfs_end_transaction(trans, root);

free_mem:
	if (free_insert) {
		kmem_cache_free(btrfs_delayed_ref_head_cachep, insert_head);
		kmem_cache_free(btrfs_delayed_data_ref_cachep, insert_dref);
		kfree(insert_qrecord);
	}
	return ret;
}

int btrfs_dedupe_search(struct btrfs_fs_info *fs_info,
			struct inode *inode, u64 file_pos,
			struct btrfs_dedupe_hash *hash)
{
	struct btrfs_dedupe_info *dedupe_info = fs_info->dedupe_info;
	int ret = -EINVAL;

	if (!hash)
		return 0;

	/*
	 * This function doesn't follow fs_info->dedupe_enabled as it will need
	 * to ensure any hashed extent to go through dedupe routine
	 */
	if (WARN_ON(dedupe_info == NULL))
		return -EINVAL;

	if (WARN_ON(btrfs_dedupe_hash_hit(hash)))
		return -EINVAL;

	if (dedupe_info->backend == BTRFS_DEDUPE_BACKEND_INMEMORY ||
	    dedupe_info->backend == BTRFS_DEDUPE_BACKEND_ONDISK)
		ret = generic_search(dedupe_info, inode, file_pos, hash);

	/* It's possible hash->bytenr/num_bytenr already changed */
	if (ret == 0) {
		hash->num_bytes = 0;
		hash->bytenr = 0;
	}
	return ret;
}

int btrfs_dedupe_calc_hash(struct btrfs_fs_info *fs_info,
			   struct inode *inode, u64 start,
			   struct btrfs_dedupe_hash *hash)
{
	int i;
	int ret;
	struct page *p;
	struct btrfs_dedupe_info *dedupe_info = fs_info->dedupe_info;
	struct crypto_shash *tfm = dedupe_info->dedupe_driver;
	struct {
		struct shash_desc desc;
		char ctx[crypto_shash_descsize(tfm)];
	} sdesc;
	u64 dedupe_bs;
	u64 sectorsize = BTRFS_I(inode)->root->sectorsize;

	if (!fs_info->dedupe_enabled || !hash)
		return 0;

	if (WARN_ON(dedupe_info == NULL))
		return -EINVAL;

	WARN_ON(!IS_ALIGNED(start, sectorsize));

	dedupe_bs = dedupe_info->blocksize;

	sdesc.desc.tfm = tfm;
	sdesc.desc.flags = 0;
	ret = crypto_shash_init(&sdesc.desc);
	if (ret)
		return ret;
	for (i = 0; sectorsize * i < dedupe_bs; i++) {
		char *d;

		p = find_get_page(inode->i_mapping,
				  (start >> PAGE_CACHE_SHIFT) + i);
		if (WARN_ON(!p))
			return -ENOENT;
		d = kmap(p);
		ret = crypto_shash_update(&sdesc.desc, d, sectorsize);
		kunmap(p);
		page_cache_release(p);
		if (ret)
			return ret;
	}
	ret = crypto_shash_final(&sdesc.desc, hash->hash);
	return ret;
}
