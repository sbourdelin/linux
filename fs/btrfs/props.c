/*
 * Copyright (C) 2014 Filipe David Borba Manana <fdmanana@gmail.com>
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

#include <linux/hashtable.h>
#include <linux/random.h>
#include "props.h"
#include "btrfs_inode.h"
#include "hash.h"
#include "transaction.h"
#include "xattr.h"
#include "compression.h"
#include "encrypt.h"

#define BTRFS_PROP_HANDLERS_HT_BITS 8
static DEFINE_HASHTABLE(prop_handlers_ht, BTRFS_PROP_HANDLERS_HT_BITS);

#define BTRFS_PROP_INHERIT_NONE		(1U << 0)
#define BTRFS_PROP_INHERIT_FOR_DIR	(1U << 1)
#define BTRFS_PROP_INHERIT_FOR_CLONE	(1U << 2)
#define BTRFS_PROP_INHERIT_FOR_SUBVOL	(1U << 3)

struct prop_handler {
	struct hlist_node node;
	const char *xattr_name;
	int (*validate)(struct inode *inode, const char *value, size_t len);
	int (*apply)(struct inode *inode, const char *value, size_t len);
	const char *(*extract)(struct inode *inode);
	int inheritable;
};

static int prop_compression_validate(struct inode *inode, const char *value, size_t len);
static int prop_compression_apply(struct inode *inode,
				  const char *value,
				  size_t len);
static const char *prop_compression_extract(struct inode *inode);

static int prop_encrypt_validate(struct inode *inode, const char *value, size_t len);
static int prop_encrypt_apply(struct inode *inode,
				  const char *value, size_t len);
static const char *prop_encrypt_extract(struct inode *inode);
static int prop_cryptoiv_validate(struct inode *inode, const char *value, size_t len);
static int prop_cryptoiv_apply(struct inode *inode,
				  const char *value, size_t len);
static const char *prop_cryptoiv_extract(struct inode *inode);

static struct prop_handler prop_handlers[] = {
	{
		.xattr_name = XATTR_BTRFS_PREFIX "compression",
		.validate = prop_compression_validate,
		.apply = prop_compression_apply,
		.extract = prop_compression_extract,
		.inheritable = BTRFS_PROP_INHERIT_FOR_DIR| \
				BTRFS_PROP_INHERIT_FOR_CLONE| \
				BTRFS_PROP_INHERIT_FOR_SUBVOL,
	},
	{
		.xattr_name = XATTR_BTRFS_PREFIX "encrypt",
		.validate = prop_encrypt_validate,
		.apply = prop_encrypt_apply,
		.extract = prop_encrypt_extract,
		.inheritable = BTRFS_PROP_INHERIT_FOR_DIR| \
				BTRFS_PROP_INHERIT_FOR_CLONE| \
				BTRFS_PROP_INHERIT_FOR_SUBVOL,
	},
	{
		.xattr_name = XATTR_BTRFS_PREFIX "cryptoiv",
		.validate = prop_cryptoiv_validate,
		.apply = prop_cryptoiv_apply,
		.extract = prop_cryptoiv_extract,
		.inheritable = BTRFS_PROP_INHERIT_FOR_DIR| \
				BTRFS_PROP_INHERIT_FOR_CLONE| \
				BTRFS_PROP_INHERIT_FOR_SUBVOL,
	},
};

void __init btrfs_props_init(void)
{
	int i;

	hash_init(prop_handlers_ht);

	for (i = 0; i < ARRAY_SIZE(prop_handlers); i++) {
		struct prop_handler *p = &prop_handlers[i];
		u64 h = btrfs_name_hash(p->xattr_name, strlen(p->xattr_name));

		hash_add(prop_handlers_ht, &p->node, h);
	}
}

static const struct hlist_head *find_prop_handlers_by_hash(const u64 hash)
{
	struct hlist_head *h;

	h = &prop_handlers_ht[hash_min(hash, BTRFS_PROP_HANDLERS_HT_BITS)];
	if (hlist_empty(h))
		return NULL;

	return h;
}

static const struct prop_handler *
find_prop_handler(const char *name,
		  const struct hlist_head *handlers)
{
	struct prop_handler *h;

	if (!handlers) {
		u64 hash = btrfs_name_hash(name, strlen(name));

		handlers = find_prop_handlers_by_hash(hash);
		if (!handlers)
			return NULL;
	}

	hlist_for_each_entry(h, handlers, node)
		if (!strcmp(h->xattr_name, name))
			return h;

	return NULL;
}

static int __btrfs_set_prop(struct btrfs_trans_handle *trans,
			    struct inode *inode,
			    const char *name,
			    const char *value,
			    size_t value_len,
			    int flags)
{
	const struct prop_handler *handler;
	int ret;

	if (strlen(name) <= XATTR_BTRFS_PREFIX_LEN)
		return -EINVAL;

	handler = find_prop_handler(name, NULL);
	if (!handler)
		return -EINVAL;

	if (value_len == 0) {
		ret = __btrfs_setxattr(trans, inode, handler->xattr_name,
				       NULL, 0, flags);
		if (ret)
			return ret;

		ret = handler->apply(inode, NULL, 0);
		ASSERT(ret == 0);

		return ret;
	}

	ret = handler->validate(inode, value, value_len);
	if (ret) {
		return ret;
	}
	ret = __btrfs_setxattr(trans, inode, handler->xattr_name,
			       value, value_len, flags);
	if (ret) {
		return ret;
	}
	ret = handler->apply(inode, value, value_len);
	if (ret && ret != -EKEYREJECTED) {
		pr_err("BTRFS: property apply failed %s %d %s %lu\n",
					name, ret, value, value_len);
		__btrfs_setxattr(trans, inode, handler->xattr_name,
				 NULL, 0, flags);
		return ret;
	}

	set_bit(BTRFS_INODE_HAS_PROPS, &BTRFS_I(inode)->runtime_flags);

	return ret;
}

int btrfs_set_prop(struct inode *inode,
		   const char *name,
		   const char *value,
		   size_t value_len,
		   int flags)
{
	return __btrfs_set_prop(NULL, inode, name, value, value_len, flags);
}

static int iterate_object_props(struct btrfs_root *root,
				struct btrfs_path *path,
				u64 objectid,
				void (*iterator)(void *,
						 const struct prop_handler *,
						 const char *,
						 size_t),
				void *ctx)
{
	int ret;
	char *name_buf = NULL;
	char *value_buf = NULL;
	int name_buf_len = 0;
	int value_buf_len = 0;

	while (1) {
		struct btrfs_key key;
		struct btrfs_dir_item *di;
		struct extent_buffer *leaf;
		u32 total_len, cur, this_len;
		int slot;
		const struct hlist_head *handlers;

		slot = path->slots[0];
		leaf = path->nodes[0];

		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, path);
			if (ret < 0)
				goto out;
			else if (ret > 0)
				break;
			continue;
		}

		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.objectid != objectid)
			break;
		if (key.type != BTRFS_XATTR_ITEM_KEY)
			break;

		handlers = find_prop_handlers_by_hash(key.offset);
		if (!handlers)
			goto next_slot;

		di = btrfs_item_ptr(leaf, slot, struct btrfs_dir_item);
		cur = 0;
		total_len = btrfs_item_size_nr(leaf, slot);

		while (cur < total_len) {
			u32 name_len = btrfs_dir_name_len(leaf, di);
			u32 data_len = btrfs_dir_data_len(leaf, di);
			unsigned long name_ptr, data_ptr;
			const struct prop_handler *handler;

			this_len = sizeof(*di) + name_len + data_len;
			name_ptr = (unsigned long)(di + 1);
			data_ptr = name_ptr + name_len;

			if (name_len <= XATTR_BTRFS_PREFIX_LEN ||
			    memcmp_extent_buffer(leaf, XATTR_BTRFS_PREFIX,
						 name_ptr,
						 XATTR_BTRFS_PREFIX_LEN))
				goto next_dir_item;

			if (name_len >= name_buf_len) {
				kfree(name_buf);
				name_buf_len = name_len + 1;
				name_buf = kmalloc(name_buf_len, GFP_NOFS);
				if (!name_buf) {
					ret = -ENOMEM;
					goto out;
				}
			}
			read_extent_buffer(leaf, name_buf, name_ptr, name_len);
			name_buf[name_len] = '\0';

			handler = find_prop_handler(name_buf, handlers);
			if (!handler)
				goto next_dir_item;

			if (data_len > value_buf_len) {
				kfree(value_buf);
				value_buf_len = data_len;
				value_buf = kmalloc(data_len, GFP_NOFS);
				if (!value_buf) {
					ret = -ENOMEM;
					goto out;
				}
			}
			read_extent_buffer(leaf, value_buf, data_ptr, data_len);

			iterator(ctx, handler, value_buf, data_len);
next_dir_item:
			cur += this_len;
			di = (struct btrfs_dir_item *)((char *) di + this_len);
		}

next_slot:
		path->slots[0]++;
	}

	ret = 0;
out:
	btrfs_release_path(path);
	kfree(name_buf);
	kfree(value_buf);

	return ret;
}

static void inode_prop_iterator(void *ctx,
				const struct prop_handler *handler,
				const char *value,
				size_t len)
{
	struct inode *inode = ctx;
	struct btrfs_root *root = BTRFS_I(inode)->root;
	int ret;

	ret = handler->apply(inode, value, len);
	if (unlikely(ret)) {
		if (ret != -ENOKEY && ret != -EKEYREVOKED)
			btrfs_warn(root->fs_info,
			   "error applying prop %s to ino %llu (root %llu): %d",
			   handler->xattr_name, btrfs_ino(inode),
			   root->root_key.objectid, ret);
	} else {
		set_bit(BTRFS_INODE_HAS_PROPS, &BTRFS_I(inode)->runtime_flags);
	}
}

int btrfs_load_inode_props(struct inode *inode, struct btrfs_path *path)
{
	struct btrfs_root *root = BTRFS_I(inode)->root;
	u64 ino = btrfs_ino(inode);
	int ret;

	ret = iterate_object_props(root, path, ino, inode_prop_iterator, inode);

	return ret;
}

static int btrfs_create_iv(char **ivdata, unsigned int ivsize)
{
	char *tmp;
	tmp = kmalloc(ivsize+1, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	get_random_bytes(tmp, ivsize);
	tmp[ivsize] = '\0';

	*ivdata = tmp;

	return 0;
}

static int inherit_props(struct btrfs_trans_handle *trans,
			 struct inode *inode,
			 struct inode *parent)
{
	struct btrfs_root *root = BTRFS_I(inode)->root;
	int ret;
	int i;

	if (!test_bit(BTRFS_INODE_HAS_PROPS,
		      &BTRFS_I(parent)->runtime_flags))
		return 0;

	for (i = 0; i < ARRAY_SIZE(prop_handlers); i++) {
		const struct prop_handler *h = &prop_handlers[i];
		const char *value;
		u64 num_bytes;

		/*
		 * BTRFS_CRYPTO_fixme:
		 * should be inheritable only by files inode type
		 */
		if (!h->inheritable)
			continue;

		value = h->extract(parent);
		if (!value)
			continue;

		num_bytes = btrfs_calc_trans_metadata_size(root, 1);
		ret = btrfs_block_rsv_add(root, trans->block_rsv,
					  num_bytes, BTRFS_RESERVE_NO_FLUSH);
		if (ret) {
			if (!strcmp(h->xattr_name, "btrfs.encrypt") ||
				!strcmp(h->xattr_name, "btrfs.cryptoiv"))
				kfree(value);
			goto out;
		}
		if (!strcmp(h->xattr_name, "btrfs.cryptoiv"))
			ret = __btrfs_set_prop(trans, inode, h->xattr_name,
				       value, BTRFS_CRYPTO_IV_SIZE, 0);
		else
			ret = __btrfs_set_prop(trans, inode, h->xattr_name,
				       value, strlen(value), 0);
		if (ret) {
			pr_err("BTRFS: %lu failed to inherit '%s': %d\n",
					inode->i_ino, h->xattr_name, ret);
			if (!strcmp(h->xattr_name, "btrfs.encrypt") ||
				!strcmp(h->xattr_name, "btrfs.cryptoiv"))
				btrfs_disable_encrypt_inode(inode);
			dump_stack();
		}

		btrfs_block_rsv_release(root, trans->block_rsv, num_bytes);
		if (ret) {
			if (!strcmp(h->xattr_name, "btrfs.encrypt") ||
				!strcmp(h->xattr_name, "btrfs.cryptoiv"))
				kfree(value);
			goto out;
		}
		if (!strcmp(h->xattr_name, "btrfs.encrypt") ||
			!strcmp(h->xattr_name, "btrfs.cryptoiv"))
			kfree(value);
	}
	ret = 0;
out:
	return ret;
}

int btrfs_inode_inherit_props(struct btrfs_trans_handle *trans,
			      struct inode *inode,
			      struct inode *dir)
{
	if (!dir)
		return 0;

	return inherit_props(trans, inode, dir);
}

int btrfs_subvol_inherit_props(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root,
			       struct btrfs_root *parent_root)
{
	struct btrfs_key key;
	struct inode *parent_inode, *child_inode;
	int ret;

	key.objectid = BTRFS_FIRST_FREE_OBJECTID;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	parent_inode = btrfs_iget(parent_root->fs_info->sb, &key,
				  parent_root, NULL);
	if (IS_ERR(parent_inode))
		return PTR_ERR(parent_inode);

	child_inode = btrfs_iget(root->fs_info->sb, &key, root, NULL);
	if (IS_ERR(child_inode)) {
		iput(parent_inode);
		return PTR_ERR(child_inode);
	}

	ret = inherit_props(trans, child_inode, parent_inode);
	iput(child_inode);
	iput(parent_inode);

	return ret;
}

static int prop_compression_validate(struct inode *inode, const char *value, size_t len)
{
	if (BTRFS_I(inode)->force_compress == BTRFS_ENCRYPT_AES)
		return -ENOTSUPP;

	if (!strncmp("lzo", value, len))
		return 0;
	else if (!strncmp("zlib", value, len))
		return 0;

	return -EINVAL;
}

static int prop_compression_apply(struct inode *inode,
				  const char *value,
				  size_t len)
{
	int type;

	if (len == 0) {
		BTRFS_I(inode)->flags |= BTRFS_INODE_NOCOMPRESS;
		BTRFS_I(inode)->flags &= ~BTRFS_INODE_COMPRESS;
		BTRFS_I(inode)->force_compress = BTRFS_COMPRESS_NONE;

		return 0;
	}

	if (!strncmp("lzo", value, len))
		type = BTRFS_COMPRESS_LZO;
	else if (!strncmp("zlib", value, len))
		type = BTRFS_COMPRESS_ZLIB;
	else
		return -EINVAL;

	BTRFS_I(inode)->flags &= ~BTRFS_INODE_NOCOMPRESS;
	BTRFS_I(inode)->flags |= BTRFS_INODE_COMPRESS;
	BTRFS_I(inode)->force_compress = type;

	return 0;
}

static const char *prop_compression_extract(struct inode *inode)
{
	switch (BTRFS_I(inode)->force_compress) {
	case BTRFS_COMPRESS_ZLIB:
		return "zlib";
	case BTRFS_COMPRESS_LZO:
		return "lzo";
	}

	return NULL;
}

static int btrfs_split_key_type(const char *val, size_t len,
					char *tfm, char *keytag)
{
	char *tmp;
	char *tmp2;
	char tmp1[BTRFS_CRYPTO_KEYTAG_SIZE + BTRFS_CRYPTO_TFM_NAME_SIZE + 1];

	if (len > BTRFS_CRYPTO_KEYTAG_SIZE + BTRFS_CRYPTO_TFM_NAME_SIZE) {
		return -EINVAL;
	}
	memcpy(tmp1, val, len);
	tmp1[len] = '\0';
	tmp = tmp1;
	tmp2 = strsep(&tmp, "@");
	if (!tmp2)
		return -EINVAL;

	if (strlen(tmp2) > BTRFS_CRYPTO_TFM_NAME_SIZE ||
			strlen(tmp) > BTRFS_CRYPTO_KEYTAG_SIZE)
		return -EINVAL;

	strcpy(tfm, tmp2);
	strcpy(keytag, tmp);

	return 0;
}

/*
 * The required foramt in the value is <crypto_algo>@<key_tag>
 * eg: btrfs.encrypt="ctr(aes)@btrfs:61e0d004"
 */
static int prop_encrypt_validate(struct inode *inode,
					const char *value, size_t len)
{
	int ret;
	size_t keylen;
	char keytag[BTRFS_CRYPTO_KEYTAG_SIZE + 1];
	char keyalgo[BTRFS_CRYPTO_TFM_NAME_SIZE + 1];

	if (BTRFS_I(inode)->force_compress == BTRFS_COMPRESS_ZLIB ||
		BTRFS_I(inode)->force_compress == BTRFS_COMPRESS_LZO)
		return -ENOTSUPP;

	if (!len)
		return 0;

	if (len > (BTRFS_CRYPTO_TFM_NAME_SIZE + BTRFS_CRYPTO_KEYTAG_SIZE ))
		return -EINVAL;

	ret = btrfs_split_key_type(value, len, keyalgo, keytag);
	if (ret) {
		pr_err("BTRFS: %lu mal formed value '%s' %lu\n",
					inode->i_ino, value, len);
		return ret;
	}

	keylen = get_encrypt_type_len(keyalgo);
	if (!keylen)
		return -ENOTSUPP;

	ret = btrfs_check_keytag(keytag);
	if (!ret)
		return ret;

	ret = btrfs_validate_keytag(inode, keytag);
	// check if its newly being set
	if (ret == -ENOTSUPP)
		ret = 0;

	return ret;
}

static int prop_encrypt_apply(struct inode *inode,
				const char *value, size_t len)
{
	int ret;
	u64 root_flags;
	char keytag[BTRFS_CRYPTO_KEYTAG_SIZE];
	char keyalgo[BTRFS_CRYPTO_TFM_NAME_SIZE];
	struct btrfs_root_item *root_item;
	struct btrfs_root *root;

	root_item = &(BTRFS_I(inode)->root->root_item);
	root = BTRFS_I(inode)->root;

	if (len == 0) {
		/* means disable encryption */
		return -EOPNOTSUPP;
	}

	ret = btrfs_split_key_type(value, len, keyalgo, keytag);
	if (ret)
		return ret;

	/* do it only for the subvol or snapshot */
	if (btrfs_ino(inode) == BTRFS_FIRST_FREE_OBJECTID) {
		if (!root_item->crypto_keyhash) {
			pr_info("BTRFS: subvol %pU enable encryption '%s'\n",
							root_item->uuid, keyalgo);
			/*
			 * We are here when xattribute being set for the first time
			 */
			ret = btrfs_set_keyhash(inode, keytag);
			if (!ret) {
				root_flags = btrfs_root_flags(root_item);
				btrfs_set_root_flags(root_item,
					root_flags | BTRFS_ROOT_SUBVOL_ENCRYPT);

				strncpy(root_item->encrypt_algo, keyalgo,
						BTRFS_CRYPTO_TFM_NAME_SIZE);
			}
		} else {
			ret = btrfs_validate_keytag(inode, keytag);
		}
		if (!ret)
			strncpy(root->crypto_keytag, keytag,
						BTRFS_CRYPTO_KEYTAG_SIZE);
	}

	if (!ret) {
		BTRFS_I(inode)->flags |= BTRFS_INODE_ENCRYPT;
		BTRFS_I(inode)->force_compress = get_encrypt_type_index(keyalgo);
	}

	return ret;
}

static int tuplet_encrypt_tfm_and_tag(char *val_out, char *tfm, char *tag)
{
	char tmp_tag[BTRFS_CRYPTO_KEYTAG_SIZE + 1];
	char tmp_tfm[BTRFS_CRYPTO_TFM_NAME_SIZE + 1];
	int sz = BTRFS_CRYPTO_TFM_NAME_SIZE + BTRFS_CRYPTO_KEYTAG_SIZE + 1;

	memcpy(tmp_tag, tag, BTRFS_CRYPTO_KEYTAG_SIZE);
	memcpy(tmp_tfm, tfm, BTRFS_CRYPTO_TFM_NAME_SIZE);

	tmp_tag[BTRFS_CRYPTO_KEYTAG_SIZE] = '\0';
	tmp_tfm[BTRFS_CRYPTO_TFM_NAME_SIZE] = '\0';

	return snprintf(val_out, sz, "%s@%s", tmp_tfm, tmp_tag);
}

static const char *prop_encrypt_extract(struct inode *inode)
{
	struct btrfs_root *root;
	char val[BTRFS_CRYPTO_TFM_NAME_SIZE + BTRFS_CRYPTO_KEYTAG_SIZE + 1];

	if (!(BTRFS_I(inode)->flags & BTRFS_INODE_ENCRYPT))
		return NULL;

	root = BTRFS_I(inode)->root;

	tuplet_encrypt_tfm_and_tag(val, root->root_item.encrypt_algo,
							root->crypto_keytag);

	return kstrdup(val, GFP_NOFS);
}

static int prop_cryptoiv_validate(struct inode *inode,
					const char *value, size_t len)
{
	if (len < BTRFS_CRYPTO_IV_SIZE)
		return -EINVAL;

	return 0;
}

static int prop_cryptoiv_apply(struct inode *inode,
				const char *value, size_t len)
{
	int ret;
	char *tmp_val;

	if (!strlen(BTRFS_I(inode)->root->crypto_keytag))
		return -ENOKEY;

	tmp_val = kmemdup(value, len, GFP_KERNEL);
	/* decrypt iv and apply to binode */
	ret = btrfs_cipher_iv(0, inode, tmp_val, len);
	if (ret) {
		pr_err("BTRFS: %lu prop_cryptoiv_apply failed ret %d len %lu\n",
			inode->i_ino, ret, len);
		return ret;
	}

	memcpy(BTRFS_I(inode)->cryptoiv, tmp_val, len);
	BTRFS_I(inode)->iv_len = len;

	kfree(tmp_val);
	return 0;
}

static const char *prop_cryptoiv_extract(struct inode *inode)
{
	int ret;
	char *ivdata = NULL;

	if (!(BTRFS_I(inode)->flags & BTRFS_INODE_ENCRYPT))
		return NULL;

	ret = btrfs_create_iv(&ivdata, BTRFS_CRYPTO_IV_SIZE);
	if (ret)
		return NULL;

	/* Encrypt iv with master key */
	ret = btrfs_cipher_iv(1, inode, ivdata,
					BTRFS_CRYPTO_IV_SIZE);
	if (ret) {
		pr_err("BTRFS Error: %lu iv encrypt failed: %d\n",
						inode->i_ino, ret);
		kfree(ivdata);
		return NULL;
	}
	return ivdata;
}
