/*
 * Copyright (C) 2016 Oracle.  All rights reserved.
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
#include <linux/string.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/random.h>
#include <linux/pagemap.h>
#include <keys/user-type.h>
#include "compression.h"
#include <linux/slab.h>
#include <linux/keyctl.h>
#include <linux/key-type.h>
#include <linux/cred.h>
#include <keys/user-type.h>
#include "ctree.h"
#include "btrfs_inode.h"
#include "props.h"

static const struct btrfs_encrypt_algorithm {
	const char *name;
	size_t	keylen;
} btrfs_encrypt_algorithm_supported[] = {
	{"aes", 16}
};

/*
 * Returns cipher alg key size if the encryption type is found
 * otherwise 0
 */
size_t btrfs_check_encrypt_type(char *type)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(btrfs_encrypt_algorithm_supported); i++)
		if (!strcmp(btrfs_encrypt_algorithm_supported[i].name, type))
			return btrfs_encrypt_algorithm_supported[i].keylen;

	return 0;
}

/* key management*/
static int btrfs_request_key(char *key_tag, void *key_data)
{
	int ret;
	const struct user_key_payload *payload;
	struct key *btrfs_key = NULL;

	ret = 0;
	btrfs_key = request_key(&key_type_user, key_tag, NULL);
	if (IS_ERR(btrfs_key)) {
		ret = PTR_ERR(btrfs_key);
		btrfs_key = NULL;
		return ret;
	}

	/*
	 * caller just need key not payload so return
	 */
	if (!key_data)
		return 0;

	ret = key_validate(btrfs_key);
	if (ret < 0)
		goto out;

	rcu_read_lock(); // TODO: check down_write key->sem ?
	payload = user_key_payload(btrfs_key);
	if (IS_ERR_OR_NULL(payload)) {
		ret = PTR_ERR(payload);
		goto out;
	}

	/*
	 * As of now we just hard code as we just use ASE now
	 */
	if (payload->datalen != 16)
		ret = -EINVAL;
	else
		memcpy(key_data, payload->data, 16);

out:
	rcu_read_unlock();
	key_put(btrfs_key);

	return ret;
}

static int btrfs_get_key_data_from_inode(struct inode *inode, unsigned char *keydata)
{
	int ret;
	char keytag[15];
	struct btrfs_inode *binode;
	struct btrfs_root_item *ri;

	binode = BTRFS_I(inode);
	ri = &(binode->root->root_item);
	strncpy(keytag, ri->encrypt_keytag, 14);
	keytag[14] = '\0';

	ret = btrfs_request_key(keytag, keydata);
	return ret;
}

int btrfs_update_key_data_to_binode(struct inode *inode)
{
	int ret;
	unsigned char keydata[16];
	struct btrfs_inode *binode;

	ret = btrfs_get_key_data_from_inode(inode, keydata);
	if (ret)
		return ret;

	binode = BTRFS_I(inode);
	memcpy(binode->key_payload, keydata, 16);

	return ret;
}

int btrfs_get_keytag(struct address_space *mapping, char *keytag, struct inode **inode)
{
	struct btrfs_inode *binode;
	struct btrfs_root_item *ri;

	if (!mapping)
		return -EINVAL;

	if (!(mapping->host))
		return -EINVAL;

	binode = BTRFS_I(mapping->host);
	ri = &(binode->root->root_item);

	strncpy(keytag, ri->encrypt_keytag, 14);
	keytag[14] = '\0';
	if (inode)
		*inode = &binode->vfs_inode;

	return 0;
}

/* Encrypt and decrypt */
struct workspace {
	struct list_head list;
};

struct btrfs_crypt_result {
	struct completion completion;
	int err;
};

struct btrfs_ablkcipher_def {
	struct scatterlist sg;
	struct crypto_ablkcipher *tfm;
	struct ablkcipher_request *req;
	struct btrfs_crypt_result result;
};

int btrfs_do_blkcipher(int enc, char *data, size_t len)
{
	int ret = -EFAULT;
	struct scatterlist sg;
	unsigned int ivsize = 0;
	char *cipher = "cbc(aes)";
	struct blkcipher_desc desc;
	struct crypto_blkcipher *blkcipher = NULL;
	char *charkey =
		"\x12\x34\x56\x78\x90\xab\xcd\xef\x12\x34\x56\x78\x90\xab\xcd\xef";
	char *chariv =
		"\x12\x34\x56\x78\x90\xab\xcd\xef\x12\x34\x56\x78\x90\xab\xcd\xef";

	blkcipher = crypto_alloc_blkcipher(cipher, 0, 0);
	if (IS_ERR(blkcipher)) {
		printk("could not allocate blkcipher handle for %s\n", cipher);
		return -PTR_ERR(blkcipher);
	}

	if (crypto_blkcipher_setkey(blkcipher, charkey, 16)) {
		printk("key could not be set\n");
		ret = -EAGAIN;
		goto out;
	}

	ivsize = crypto_blkcipher_ivsize(blkcipher);
	if (ivsize) {
		if (ivsize != strlen(chariv)) {
			printk("length differs from expected length\n");
			ret = -EINVAL;
			goto out;
		}
		crypto_blkcipher_set_iv(blkcipher, chariv, ivsize);
	}

	desc.flags = 0;
	desc.tfm = blkcipher;
	sg_init_one(&sg, data, len);

	if (enc) {
		/* encrypt data in place */
		ret = crypto_blkcipher_encrypt(&desc, &sg, &sg, len);
	} else {
		/* decrypt data in place */
		ret = crypto_blkcipher_decrypt(&desc, &sg, &sg, len);
	}

	return ret;

out:
	crypto_free_blkcipher(blkcipher);
	return ret;
}

static void btrfs_ablkcipher_cb(struct crypto_async_request *req, int error)
{
	struct btrfs_crypt_result *result;

	if (error == -EINPROGRESS) {
		pr_info("Encryption callback reports error\n");
		return;
	}

	result = req->data;
	result->err = error;
	complete(&result->completion);
	pr_info("Encryption finished successfully\n");
}

static unsigned int btrfs_ablkcipher_encdec(struct btrfs_ablkcipher_def *ablk, int enc)
{
	int rc = 0;

	if (enc)
		rc = crypto_ablkcipher_encrypt(ablk->req);
	else
		rc = crypto_ablkcipher_decrypt(ablk->req);

	switch (rc) {
	case 0:
		break;
	case -EINPROGRESS:
	case -EBUSY:
		rc = wait_for_completion_interruptible(
			&ablk->result.completion);
		if (!rc && !ablk->result.err) {
			reinit_completion(&ablk->result.completion);
			break;
		}
	default:
		pr_info("ablkcipher encrypt returned with %d result %d\n",
		       rc, ablk->result.err);
		break;
	}
	init_completion(&ablk->result.completion);

	return rc;
}

void btrfs_cipher_get_ivdata(char **ivdata, unsigned int ivsize, unsigned int *ivdata_size)
{
	/* fixme */
	if (0) {
		*ivdata = kmalloc(ivsize, GFP_KERNEL);
		get_random_bytes(ivdata, ivsize);
		*ivdata_size = ivsize;
	} else {
		*ivdata = kstrdup(
			"\x12\x34\x56\x78\x90\xab\xcd\xef\x12\x34\x56\x78\x90\xab\xcd\xef",
			GFP_NOFS);
		*ivdata_size = strlen(*ivdata);
	}
}

static int btrfs_do_ablkcipher(int endec, struct page *page, unsigned long len,
							struct inode *inode)
{
	int ret = -EFAULT;
	unsigned char key_data[16];
	char *ivdata = NULL;
	unsigned int key_size;
	unsigned int ivsize = 0;
	unsigned int ivdata_size;
	unsigned int ablksize = 0;
	struct btrfs_ablkcipher_def ablk_akin;
	struct ablkcipher_request *req = NULL;
	struct crypto_ablkcipher *ablkcipher = NULL;

	ret = 0;

	if (!inode) {
		BUG_ON("Need inode\n");
		return -EINVAL;
	}
	/* get key from the inode */
	ret = btrfs_get_key_data_from_inode(inode, key_data);

	key_size = 16; //todo: defines, but review for suitable cipher

	ablkcipher = crypto_alloc_ablkcipher("cts(cbc(aes))", 0, 0);
	if (IS_ERR(ablkcipher)) {
		pr_info("could not allocate ablkcipher handle\n");
		return PTR_ERR(ablkcipher);
	}

	ablksize = crypto_ablkcipher_blocksize(ablkcipher);
	/* we can't cipher a block less the ciper block size */
	if (len < ablksize || len > PAGE_CACHE_SIZE) {
		ret = -EINVAL;
		goto out;
	}

	ivsize = crypto_ablkcipher_ivsize(ablkcipher);
	if (ivsize) {
		btrfs_cipher_get_ivdata(&ivdata, ivsize, &ivdata_size);
		if (ivsize != ivdata_size) {
			BUG_ON("IV length differs from expected length\n");
			ret = -EINVAL;
			goto out;
		}
	} else {
		BUG_ON("This cipher doesn't need ivdata, but are we ready ?\n");
		ret = -EINVAL;
		goto out;
	}

	req = ablkcipher_request_alloc(ablkcipher, GFP_KERNEL);
	if (IS_ERR(req)) {
		pr_info("could not allocate request queue\n");
		ret = PTR_ERR(req);
		goto out;
	}

	ablkcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
					btrfs_ablkcipher_cb, &ablk_akin.result);

	if (crypto_ablkcipher_setkey(ablkcipher, key_data, key_size)) {
		printk("key could not be set\n");
		ret = -EAGAIN;
		goto out;
	}

	ablk_akin.tfm = ablkcipher;
	ablk_akin.req = req;

	sg_init_table(&ablk_akin.sg, 1);
	sg_set_page(&ablk_akin.sg, page, len, 0);
	ablkcipher_request_set_crypt(req, &ablk_akin.sg, &ablk_akin.sg, len, ivdata);

	init_completion(&ablk_akin.result.completion);

	ret = btrfs_ablkcipher_encdec(&ablk_akin, endec);

out:
	if (ablkcipher)
		crypto_free_ablkcipher(ablkcipher);
	if (req)
		ablkcipher_request_free(req);

	kfree(ivdata);

	return ret;
}

static int btrfs_encrypt_pages(struct list_head *na_ws, struct address_space *mapping,
			u64 start, unsigned long len, struct page **pages,
			unsigned long nr_pages, unsigned long *na_out_pages,
			unsigned long *na_total_in, unsigned long *na_total_out,
			unsigned long na_max_out)
{
	int ret;
	struct page *in_page;
	struct page *out_page;
	char *in;
	char *out;
	unsigned long bytes_left = len;
	unsigned long cur_page_len;
	unsigned long cur_page;
	struct inode *inode;

	*na_total_in = 0;
	*na_out_pages = 0;

	if (!mapping && !mapping->host) {
		WARN_ON("Need mapped pages\n");
		return -EINVAL;
	}

	inode = mapping->host;

	for (cur_page = 0; cur_page < nr_pages; cur_page++) {

		WARN_ON(!bytes_left);

		in_page = find_get_page(mapping, start >> PAGE_CACHE_SHIFT);
		out_page = alloc_page(GFP_NOFS| __GFP_HIGHMEM);
		cur_page_len = min(bytes_left, PAGE_CACHE_SIZE);

		in = kmap(in_page);
		out = kmap(out_page);
		memcpy(out, in, cur_page_len);
		kunmap(out_page);
		kunmap(in_page);

		ret = btrfs_do_ablkcipher(1, out_page, cur_page_len, inode);
		if (ret) {
			__free_page(out_page);
			return ret;
		}

		pages[cur_page] = out_page;
		*na_out_pages = *na_out_pages + 1;
		*na_total_in = *na_total_in + cur_page_len;

		start += cur_page_len;
		bytes_left = bytes_left - cur_page_len;
	}

	return ret;
}

static int btrfs_decrypt_pages(struct list_head *na_ws, unsigned char *in, struct page *out_page,
				unsigned long na_start_byte, size_t in_size, size_t out_size)
{
	int ret;
	char *out_addr;
	char keytag[24];
	struct address_space *mapping;
	struct inode *inode;

	if (!out_page)
		return -EINVAL;

	if (in_size > PAGE_CACHE_SIZE)
		return -EINVAL;

	memset(keytag, '\0', 24);

	mapping = out_page->mapping;
	if (!mapping && !mapping->host) {
		WARN_ON("Need mapped pages\n");
		return -EINVAL;
	}

	inode = mapping->host;

	out_addr = kmap(out_page);
	memcpy(out_addr, in, in_size);
	kunmap(out_page);

	ret = btrfs_do_ablkcipher(0, out_page, in_size, inode);

	return ret;
}

static int btrfs_decrypt_pages_bio(struct list_head *na_ws, struct page **in_pages,
					u64 in_start_offset, struct bio_vec *out_pages_bio,
					int bi_vcnt, size_t in_len)
{
	char *in;
	char *out;
	int ret = 0;
	struct page *in_page;
	struct page *out_page;
	unsigned long cur_page_n;
	unsigned long bytes_left;
	unsigned long in_nr_pages;
	unsigned long cur_page_len;
	unsigned long processed_len = 0;
	struct address_space *mapping;
	struct inode *inode;

	if (na_ws)
		return -EINVAL;

	out_page = out_pages_bio[0].bv_page;
	mapping = out_page->mapping;
	if (!mapping && !mapping->host) {
		WARN_ON("Need mapped page\n");
		return -EINVAL;
	}

	inode = mapping->host;

	in_nr_pages = DIV_ROUND_UP(in_len, PAGE_CACHE_SIZE);
	bytes_left = in_len;
	WARN_ON(in_nr_pages != bi_vcnt);

	for (cur_page_n = 0; cur_page_n < in_nr_pages; cur_page_n++) {
		WARN_ON(!bytes_left);

		in_page = in_pages[cur_page_n];
		out_page = out_pages_bio[cur_page_n].bv_page;

		cur_page_len = min(bytes_left, PAGE_CACHE_SIZE);

		in = kmap(in_page);
		out = kmap(out_page);
		memcpy(out, in, cur_page_len);
		kunmap(out_page);
		kunmap(in_page);

		ret = btrfs_do_ablkcipher(0, out_page, cur_page_len, inode);

		if (ret && ret != -ENOKEY)
			goto error_out;

		if (cur_page_len < PAGE_CACHE_SIZE) {
			out = kmap(out_page);
			memset(out + cur_page_len, 0, PAGE_CACHE_SIZE - cur_page_len);
			kunmap(out_page);
		}

		bytes_left = bytes_left - cur_page_len;
		processed_len = processed_len + cur_page_len;

		//flush_dcache_page(out_page);
	}
	WARN_ON(processed_len != in_len);
	WARN_ON(bytes_left);

error_out:
	return ret;
}

const struct btrfs_compress_op btrfs_encrypt_ops = {
	.alloc_workspace	= NULL,
	.free_workspace		= NULL,
	.compress_pages		= btrfs_encrypt_pages,
	.decompress_biovec	= btrfs_decrypt_pages_bio,
	.decompress		= btrfs_decrypt_pages,
};
