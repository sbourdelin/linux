/*
 * Copyright (C) 2016 Oracle.  All rights reserved.
 * Author: Anand Jain (anand.jain@oracle.com)
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
#include "hash.h"
#include "encrypt.h"
#include "xattr.h"

static const struct btrfs_encrypt_algorithm {
	const char *name;
	size_t keylen;
	size_t ivlen;
	int type_index;
} btrfs_encrypt_algorithm_supported[] = {
	{"ctr(aes)", 16, 16, BTRFS_ENCRYPT_AES}
};

int get_encrypt_type_index(char *type_name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(btrfs_encrypt_algorithm_supported); i++)
		if (!strcmp(btrfs_encrypt_algorithm_supported[i].name, type_name))
			return btrfs_encrypt_algorithm_supported[i].type_index;

	return -EINVAL;
}

/*
 * Returns cipher alg key size if the encryption type is found
 * otherwise 0
 */
size_t get_encrypt_type_len(char *type)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(btrfs_encrypt_algorithm_supported); i++)
		if (!strcmp(btrfs_encrypt_algorithm_supported[i].name, type))
			return btrfs_encrypt_algorithm_supported[i].keylen;

	return 0;
}

void btrfs_disable_encrypt_inode(struct inode *inode)
{
	if (BTRFS_I(inode)->force_compress == BTRFS_ENCRYPT_AES)
		BTRFS_I(inode)->force_compress = 0;
}

/*
 * Helper to get the key.
 * The key can be in
 *                system keyring or
 *                in a file in the external USB drive
 * As of now only keyring type is supported.
 */
int btrfs_request_key(char *key_tag, void *key_data)
{
	int ret;
	const struct user_key_payload *payload;
	struct key *btrfs_key = NULL;

	ret = 0;

	btrfs_key = request_key(BTRFS_CRYPTO_KEY_TYPE, key_tag, NULL);
	if (IS_ERR(btrfs_key)) {
		ret = PTR_ERR(btrfs_key);
		btrfs_key = NULL;
		return ret;
	}

	ret = key_validate(btrfs_key);
	if (ret < 0) {
		key_put(btrfs_key);
		return ret;
	}

	down_read(&btrfs_key->sem);
	payload = user_key_payload(btrfs_key);
	if (IS_ERR_OR_NULL(payload)) {
		pr_err("get payload failed\n");
		ret = PTR_ERR(payload);
		goto out;
	}

	if (payload->datalen != BTRFS_CRYPTO_KEY_SIZE) {
		pr_err("payload datalen does not match the expected\n");
		ret = -EINVAL;
		goto out;
	}

	memcpy(key_data, payload->data, BTRFS_CRYPTO_KEY_SIZE);

out:
	up_read(&btrfs_key->sem);
	key_put(btrfs_key);

	return ret;
}

#if !(BTRFS_CRYPTO_TEST_BYDUMMYENC | BTRFS_CRYPTO_TEST_BYDUMMYKEY)
static int btrfs_get_cipher_name_from_inode(struct inode *inode,
					unsigned char *cipher_name)
{
	struct btrfs_root *root;

	root = BTRFS_I(inode)->root;
	memcpy(cipher_name, root->root_item.encrypt_algo,
				BTRFS_CRYPTO_TFM_NAME_SIZE);
	cipher_name[BTRFS_CRYPTO_TFM_NAME_SIZE] = '\0';
	if (strlen(cipher_name))
		return 0;

	if (root->fs_info->compress_type == BTRFS_ENCRYPT_AES) {
		memset(cipher_name, 0, BTRFS_CRYPTO_TFM_NAME_SIZE);
		memcpy(cipher_name, "ctr(aes)",
				BTRFS_CRYPTO_TFM_NAME_SIZE);
		return 0;
	}

	return -EINVAL;
}
#endif

int btrfs_check_keytag(char *keytag)
{
	unsigned char keydata[BTRFS_CRYPTO_KEY_SIZE];
	return btrfs_request_key(keytag, keydata);
}

int btrfs_validate_keytag(struct inode *inode, unsigned char *keytag)
{
	int ret;
	u32 seed = 0;
	u32 keyhash = ~(u32)0;
	struct btrfs_root_item *ri;
	unsigned char keydata[BTRFS_CRYPTO_KEY_SIZE];

	ri = &(BTRFS_I(inode)->root->root_item);
	if (!ri->crypto_keyhash)
		return -ENOTSUPP;

	ret = btrfs_request_key(keytag, keydata);
	if (ret)
		return ret;

	keyhash = btrfs_crc32c(seed, keydata, BTRFS_CRYPTO_KEY_SIZE);
	if (keyhash != ri->crypto_keyhash) {
		/* wrong key */
		pr_err("BTRFS: %pU wrong key: hash %u expected %u\n",
				ri->uuid, keyhash, ri->crypto_keyhash);
		return -EKEYREJECTED;
	}

	return 0;
}

int btrfs_set_keyhash(struct inode *inode, char *keytag)
{
	int ret;
	u32 seed = 0;
	u32 keyhash = ~(u32)0;
	struct btrfs_root *root;
	unsigned char keydata[BTRFS_CRYPTO_KEY_SIZE+1];

	ret = btrfs_request_key(keytag, keydata);
	if (ret)
		return ret;

	keyhash = btrfs_crc32c(seed, keydata, BTRFS_CRYPTO_KEY_SIZE);
	root = BTRFS_I(inode)->root;
	root->root_item.crypto_keyhash = keyhash;
	return 0;
}

int btrfs_check_key_access(struct inode *inode)
{
	int ret;
	u32 seed = 0;
	u32 keyhash = ~(u32)0;
	struct btrfs_root_item *ri;
	char keytag[BTRFS_CRYPTO_KEYTAG_SIZE + 1];
	unsigned char keydata[BTRFS_CRYPTO_KEY_SIZE + 1];

	ri = &(BTRFS_I(inode)->root->root_item);
	if (!ri->crypto_keyhash)
		return -ENOKEY;

	strncpy(keytag, BTRFS_I(inode)->root->crypto_keytag,
					BTRFS_CRYPTO_KEYTAG_SIZE);
	keytag[BTRFS_CRYPTO_KEYTAG_SIZE] = '\0';
	ret = btrfs_request_key(keytag, keydata);
	if (ret)
		return ret;

	keyhash = btrfs_crc32c(seed, keydata, BTRFS_CRYPTO_KEY_SIZE);
	/*
	 * what if there is different key with the same keytag
	 * check with the hash helps to eliminate this case.
	 */
	if (ri->crypto_keyhash != keyhash)
		return -EKEYREJECTED;

	return 0;
}

int btrfs_get_master_key(struct inode *inode,
					unsigned char *key)
{
	int ret;
	char keytag[BTRFS_CRYPTO_KEYTAG_SIZE + 1];
	struct btrfs_root_item *ri;
	u32 keyhash = ~(u32)0;
	u32 seed = 0;
	unsigned char keydata[BTRFS_CRYPTO_KEY_SIZE + 1];

	ri = &(BTRFS_I(inode)->root->root_item);
	if (strlen(BTRFS_I(inode)->root->crypto_keytag)) {
		strncpy(keytag, BTRFS_I(inode)->root->crypto_keytag,
					BTRFS_CRYPTO_KEYTAG_SIZE);
	} else {
		pr_err("BTRFS: %lu btrfs_get_master_key no keytag\n",
						inode->i_ino);
		return -EINVAL;
	}
	keytag[BTRFS_CRYPTO_KEYTAG_SIZE] = '\0';

	ret = btrfs_request_key(keytag, keydata);
	if (ret)
		return ret;

	keyhash = btrfs_crc32c(seed, keydata, BTRFS_CRYPTO_KEY_SIZE);

	/*
	 * what if there is different key with the same keytag
	 * checking with the hash helps to eliminate this case.
	 */
	if (ri->crypto_keyhash && ri->crypto_keyhash != keyhash) {
		/* wrong key */
		pr_err("BTRFS: %pU wrong key: hash %u expected %u\n",
				ri->uuid, keyhash, ri->crypto_keyhash);
		return -EKEYREJECTED;
	}

	memcpy(key, keydata, BTRFS_CRYPTO_KEY_SIZE);

	return 0;
}

#if !(BTRFS_CRYPTO_TEST_BYDUMMYENC | BTRFS_CRYPTO_TEST_BYDUMMYKEY)
static int btrfs_get_iv_from_inode(struct inode *inode,
				unsigned char *iv, size_t *iv_size)
{
	if (!BTRFS_I(inode)->iv_len)
		return -EINVAL;

	memcpy(iv, BTRFS_I(inode)->cryptoiv, BTRFS_I(inode)->iv_len);

	*iv_size = BTRFS_I(inode)->iv_len;

	return 0;
}
#endif

int btrfs_update_key_to_binode(struct inode *inode)
{
	int ret;
	unsigned char keydata[BTRFS_CRYPTO_KEY_SIZE];

	ret = btrfs_get_master_key(inode, keydata);
	if (ret)
		return ret;

	memcpy(BTRFS_I(inode)->key_payload, keydata,
					BTRFS_CRYPTO_KEY_SIZE);

	BTRFS_I(inode)->key_len = BTRFS_CRYPTO_KEY_SIZE;
	return ret;
}

int btrfs_blkcipher(int encrypt, struct btrfs_blkcipher_req *btrfs_req,
						char *data, size_t len)
{
	int ret = -EFAULT;
	struct scatterlist sg;
	unsigned int ivsize = 0;
	unsigned int blksize = 0;
	char *cipher = "cbc(aes)";
	struct blkcipher_desc desc;
	struct crypto_blkcipher *blkcipher = NULL;

	blkcipher = crypto_alloc_blkcipher(cipher, 0, 0);
	if (IS_ERR(blkcipher)) {
		pr_err("BTRFS: crypto, allocate blkcipher handle for %s\n", cipher);
		return -PTR_ERR(blkcipher);
	}

	blksize = crypto_blkcipher_blocksize(blkcipher);
	if (len < blksize) {
		pr_err("BTRFS: crypto, blk can't work with len %lu\n", len);
		ret = -EINVAL;
		goto out;
	}

	if (crypto_blkcipher_setkey(blkcipher, btrfs_req->key,
					btrfs_req->key_len)) {
		pr_err("BTRFS: crypto, key could not be set\n");
		ret = -EAGAIN;
		goto out;
	}

	ivsize = crypto_blkcipher_ivsize(blkcipher);
	if (ivsize != btrfs_req->iv_len) {
		pr_err("BTRFS: crypto, length differs from expected length\n");
		ret = -EINVAL;
		goto out;
	}

	crypto_blkcipher_set_iv(blkcipher, btrfs_req->cryptoiv,
					btrfs_req->iv_len);

	desc.flags = 0;
	desc.tfm = blkcipher;
	sg_init_one(&sg, data, len);

	if (encrypt) {
		/* encrypt data in place */
		ret = crypto_blkcipher_encrypt(&desc, &sg, &sg, len);
	} else {
		/* decrypt data in place */
		ret = crypto_blkcipher_decrypt(&desc, &sg, &sg, len);
	}

out:
	crypto_free_blkcipher(blkcipher);
	return ret;
}

int btrfs_cipher_iv(int encrypt, struct inode *inode,
					char *data, size_t len)
{
	int ret;
	struct btrfs_blkcipher_req btrfs_req;
	unsigned char key[BTRFS_CRYPTO_KEY_SIZE];
	unsigned char *iv = BTRFS_CRYPTO_IV_IV;

	ret = btrfs_get_master_key(inode, key);
	if (ret) {
		pr_err("BTRFS: crypto, %lu btrfs_get_master_key failed to '%s' iv\n",
				inode->i_ino, encrypt?"encrypt":"decrypt");
		return ret;
	}

	memcpy(btrfs_req.key, key, BTRFS_CRYPTO_KEY_SIZE);
	btrfs_req.key_len = BTRFS_CRYPTO_KEY_SIZE;
	memcpy(btrfs_req.cryptoiv, iv, BTRFS_CRYPTO_IV_SIZE);
	btrfs_req.iv_len = BTRFS_CRYPTO_IV_SIZE;

	ret = btrfs_blkcipher(encrypt, &btrfs_req, data, len);

	return ret;
}

static void btrfs_ablkcipher_cb(struct crypto_async_request *req, int error)
{
	struct btrfs_ablkcipher_result *cb_result = req->data;

	if (error == -EINPROGRESS)
		return;

	cb_result->err = error;

	complete(&cb_result->completion);
}

int btrfs_do_ablkcipher(int enc, struct page *page, unsigned long len,
				struct btrfs_ablkcipher_req_data *btrfs_req)
{
	int ret = 0;
	char *ivdata = NULL;
	unsigned int ivsize = 0;
	unsigned int ivdata_size;
	unsigned int ablksize = 0;
	struct ablkcipher_request *req = NULL;
	struct crypto_ablkcipher *ablkcipher = NULL;
	int key_len = btrfs_req->key_len;

	ablkcipher = crypto_alloc_ablkcipher(btrfs_req->cipher_name, 0, 0);
	if (IS_ERR(ablkcipher)) {
		ret = PTR_ERR(ablkcipher);
		pr_err("BTRFS: crypto, allocate cipher engine '%s' failed: %d\n",
					btrfs_req->cipher_name, ret);
		return ret;
	}

	ablksize = crypto_ablkcipher_blocksize(ablkcipher);
	/* we can't cipher a block less the ciper block size */
	if (len < ablksize) {
		ret = -EINVAL;
		goto out;
	}

	if (ablksize > BTRFS_CRYPTO_KEY_SIZE)
		BUG_ON("Incompatible key for the cipher\n");

	ivsize = crypto_ablkcipher_ivsize(ablkcipher);
	ivdata = btrfs_req->iv;
	ivdata_size = btrfs_req->iv_size;

	if (ivsize != ivdata_size) {
		BUG_ON("IV length differs from expected length\n");
		ret = -EINVAL;
		goto out;
	}

	req = ablkcipher_request_alloc(ablkcipher, GFP_KERNEL);
	if (IS_ERR(req)) {
		pr_info("BTRFS: crypto, could not allocate request queue\n");
		ret = PTR_ERR(req);
		goto out;
	}
	btrfs_req->tfm = ablkcipher;
	btrfs_req->req = req;

	ablkcipher_request_set_tfm(req, ablkcipher);
	ablkcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				btrfs_ablkcipher_cb, &btrfs_req->cb_result);

	ret = crypto_ablkcipher_setkey(ablkcipher, btrfs_req->key, key_len);
	if (ret) {
		pr_err("BTRFS: crypto, cipher '%s' set key failed: len %u %d\n",
				btrfs_req->cipher_name, key_len, ret);
		goto out;
	}

	sg_init_table(&btrfs_req->sg_src, 1);
	sg_set_page(&btrfs_req->sg_src, page, len, 0);
	ablkcipher_request_set_crypt(req, &btrfs_req->sg_src,
				&btrfs_req->sg_src, len, ivdata);

	init_completion(&btrfs_req->cb_result.completion);

	if (enc)
		ret = crypto_ablkcipher_encrypt(btrfs_req->req);
	else
		ret = crypto_ablkcipher_decrypt(btrfs_req->req);

	switch (ret) {
	case 0:
		break;
	case -EINPROGRESS:
	case -EBUSY:
		ret = wait_for_completion_interruptible(
					&btrfs_req->cb_result.completion);
		if (!ret && !btrfs_req->cb_result.err) {
			reinit_completion(&btrfs_req->cb_result.completion);
			break;
		}
	default:
		pr_info("crypto engine: %d result %d\n",
					ret, btrfs_req->cb_result.err);
		break;
	}
	init_completion(&btrfs_req->cb_result.completion);

out:
	if (ablkcipher)
		crypto_free_ablkcipher(ablkcipher);
	if (req)
		ablkcipher_request_free(req);

	return ret;
}

static int btrfs_do_ablkcipher_by_inode(int enc, struct page *page,
				unsigned long len, struct inode *inode)
{
	int ret;
	struct btrfs_ablkcipher_req_data btrfs_req;

	if (!inode) {
		BUG_ON("BTRFS: crypto, needs inode\n");
		return -EINVAL;
	}
	memset(&btrfs_req, 0, sizeof(struct btrfs_ablkcipher_req_data));

#if BTRFS_CRYPTO_TEST_BYDUMMYENC
	if (len < PAGE_SIZE) {
		char *in;
		in = kmap(page);
		/*
		 * not scratched with zero, so to have
		 * higher chance of catching bugs
		 */
		memset(in+len, 'z', PAGE_SIZE - len);
		kunmap(page);
	}
	ret = 0;
#elif BTRFS_CRYPTO_TEST_BYDUMMYKEY
	/*
	 * This is for testing only, especially the extents ops,
	 * we don't worry about security here
	 */
	strncpy(btrfs_req.cipher_name, "ctr(aes)", BTRFS_CRYPTO_TFM_NAME_SIZE);
	strncpy(btrfs_req.key, BTRFS_CRYPTO_IV_IV, BTRFS_CRYPTO_KEY_SIZE);
	strncpy(btrfs_req.iv, BTRFS_CRYPTO_IV_IV, BTRFS_CRYPTO_IV_SIZE);
	btrfs_req.key_len = BTRFS_CRYPTO_KEY_SIZE;
	btrfs_req.iv_size = BTRFS_CRYPTO_IV_SIZE;
	ret = btrfs_do_ablkcipher(enc, page, len, &btrfs_req);
#else

	/* Get the cipher engine name */
	ret = btrfs_get_cipher_name_from_inode(inode, btrfs_req.cipher_name);
	if (ret) {
		pr_err("BTRFS: Error: Invalid cipher name: '%d'\n", ret);
		return -EINVAL;
	}

	/* Get the Key */
	if (BTRFS_I(inode)->key_len)
		memcpy(btrfs_req.key, BTRFS_I(inode)->key_payload,
						BTRFS_CRYPTO_KEY_SIZE);
	else
		ret = btrfs_get_master_key(inode, btrfs_req.key);

	btrfs_req.key_len = BTRFS_CRYPTO_KEY_SIZE;

	if (ret) {
		/* Error getting key */
		if (enc) {
			/* For encrypt its an error*/
			pr_err("BTRFS: crypto, '%lu' Get key failed: %d\n",
						inode->i_ino, ret);
		} else {
			/*
			 * For decrypt, the user with no key, may access
			 * ciphertext
			 */
			if (ret == -ENOKEY || ret == -EKEYREVOKED)
				ret = 0;
			else
				pr_err("BTRFS: crypto, '%lu' Get key failed: %d\n",
						inode->i_ino, ret);
		}
		return ret;
	}

	ret = btrfs_get_iv_from_inode(inode, btrfs_req.iv,
					&btrfs_req.iv_size);
	if (ret) {
		pr_err("BTRFS: crypto, can't get cryptoiv\n");
		return ret;
	}

	ret = btrfs_do_ablkcipher(enc, page, len, &btrfs_req);
#endif

	return ret;
}


static int btrfs_encrypt_pages(struct list_head *na_ws,
			struct address_space *mapping, u64 start,
			unsigned long len, struct page **pages,
			unsigned long nr_pages, unsigned long *nr_out_pages,
			unsigned long *total_in, unsigned long *total_out,
			unsigned long na_max_out, int dont_align)
{
	int ret;
	struct page *in_page;
	struct page *out_page = NULL;
	char *in;
	char *out;
	unsigned long bytes_left = len;
	unsigned long cur_page_len = 0;
	unsigned long cur_page_len_for_out = 0;
	unsigned long i;
	struct inode *inode;
	u64 blocksize;

	*total_in = 0;
	*nr_out_pages = 0;
	*total_out = 0;
	if (!len)
		return 0;

	if (!mapping && !mapping->host) {
		WARN_ON("BTRFS: crypto, need mapped pages\n");
		return -EINVAL;
	}

	inode = mapping->host;
	blocksize = BTRFS_I(inode)->root->sectorsize;
	if (blocksize != PAGE_SIZE)
		pr_err("BTRFS: crypto, fatal, blocksize not same as page size\n");

	for (i = 0; i < nr_pages; i++) {

		in_page = find_get_page(mapping, start >> PAGE_SHIFT);
		cur_page_len = min(bytes_left, PAGE_SIZE);
		out_page = alloc_page(GFP_NOFS| __GFP_HIGHMEM);

		in = kmap(in_page);
		out = kmap(out_page);
		memset(out, 0, PAGE_SIZE);
		memcpy(out, in, cur_page_len);
		kunmap(out_page);
		kunmap(in_page);
		if (dont_align)
			cur_page_len_for_out = cur_page_len;
		else
			cur_page_len_for_out = ALIGN(cur_page_len, blocksize);

		ret = btrfs_do_ablkcipher_by_inode(1, out_page,
						cur_page_len_for_out, inode);
		if (ret) {
			__free_page(out_page);
			return ret;
		}
		put_page(in_page);

		pages[i] = out_page;
		*nr_out_pages = *nr_out_pages + 1;
		*total_in += cur_page_len;
		*total_out += cur_page_len_for_out;

		start += cur_page_len;
		bytes_left = bytes_left - cur_page_len;
		if (!bytes_left)
			break;
	}

	return ret;
}

static int btrfs_decrypt_pages(struct list_head *na_ws, unsigned char *in,
			struct page *out_page, unsigned long na_start_byte,
			size_t in_size, size_t out_size)
{
	int ret;
	char *out_addr;
	struct address_space *mapping;
	struct inode *inode;

	if (!out_page)
		return -EINVAL;

	if (in_size > PAGE_SIZE) {
		WARN_ON("BTRFS: crypto, cant decrypt more than pagesize\n");
		return -EINVAL;
	}

	mapping = out_page->mapping;
	if (!mapping && !mapping->host) {
		WARN_ON("BTRFS: crypto, Need mapped pages\n");
		return -EINVAL;
	}

	inode = mapping->host;

	out_addr = kmap_atomic(out_page);
	memcpy(out_addr, in, in_size);
	kunmap_atomic(out_addr);

	ret = btrfs_do_ablkcipher_by_inode(0, out_page, in_size, inode);

#if BTRFS_CRYPTO_INFO_POTENTIAL_BUG
	if (na_start_byte) {
		pr_err("BTRFS: crypto, a context that a out start is not zero %lu\n",
						na_start_byte);
		BUG_ON(1);
	}
#endif

	return ret;
}

static int btrfs_decrypt_pages_bio(struct list_head *na_ws,
		struct page **in_pages, u64 disk_start, struct bio_vec *bvec,
		int bi_vcnt, size_t in_len)
{
	char *in;
	char *out;
	int ret = 0;
	int more = 0;
	struct page *in_page;
	struct page *out_page;
	unsigned long bytes_left;
	unsigned long total_in_pages;
	unsigned long cur_page_len;
	unsigned long processed_len = 0;
	unsigned long page_in_index = 0;
	unsigned long page_out_index = 0;
	unsigned long saved_page_out_index = 0;
	unsigned long pg_offset = 0;
	struct address_space *mapping;
	struct inode *inode;
	total_in_pages = DIV_ROUND_UP(in_len, PAGE_SIZE);

	if (na_ws)
		return -EINVAL;

	out_page = bvec[page_out_index].bv_page;
	mapping = out_page->mapping;
	if (!mapping && !mapping->host) {
		WARN_ON("BTRFS: crypto, need mapped page\n");
		return -EINVAL;
	}

	inode = mapping->host;

#if BTRFS_CRYPTO_INFO_POTENTIAL_BUG
	/* Hope the call here is an inode specific, or its not ? */
	if (bi_vcnt > 1) {
		int i;
		struct inode *tmp_i;
		for (i = 0; i < bi_vcnt; i++) {
			tmp_i = (bvec[i].bv_page)->mapping->host;
			if (tmp_i != inode)
				pr_err("BTRFS: crypto, pages of diff files %lu and %lu\n",
					tmp_i->i_ino, inode->i_ino);
		}
	}
#endif

	bytes_left = in_len;

#if BTRFS_CRYPTO_INFO_POTENTIAL_BUG
	if (total_in_pages < bi_vcnt)
		pr_err("BTRFS: crypto, untested: pages to be decrypted is less than expected, "\
			"total_in_pages %lu out_nr_pages %d in_len %lu\n",
					total_in_pages, bi_vcnt, in_len);
#endif

	for (page_in_index = 0; page_in_index < total_in_pages;
						page_in_index++) {
		cur_page_len = min(bytes_left, PAGE_SIZE);
		saved_page_out_index = page_out_index;

		in_page = in_pages[page_in_index];
		in = kmap(in_page);
		more = btrfs_decompress_buf2page(in, processed_len,
				processed_len + cur_page_len, disk_start,
				bvec, bi_vcnt, &page_out_index, &pg_offset);
		kunmap(in_page);

		/*
		 * if page_out_index is incremented then we know data to
		 * decrypt is in the outpage.
		 */
		if (!more || saved_page_out_index != page_out_index) {
			out_page = bvec[saved_page_out_index].bv_page;
			ret = btrfs_do_ablkcipher_by_inode(0, out_page,
						cur_page_len, inode);
			if (ret)
				return ret;

			if (cur_page_len < PAGE_SIZE) {
				out = kmap(out_page);
				memset(out + cur_page_len, 0,
						PAGE_SIZE - cur_page_len);
				kunmap(out_page);
			}
		}

		bytes_left = bytes_left - cur_page_len;
		processed_len = processed_len + cur_page_len;
		if (!more)
			break;
	}
	return 0;
}

const struct btrfs_compress_op btrfs_encrypt_ops = {
	.alloc_workspace	= NULL,
	.free_workspace		= NULL,
	.compress_pages		= btrfs_encrypt_pages,
	.decompress_biovec	= btrfs_decrypt_pages_bio,
	.decompress		= btrfs_decrypt_pages,
};
