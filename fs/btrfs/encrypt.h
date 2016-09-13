/*
 * Copyright (C) 2016 Oracle.  All rights reserved.
 * Author: Anand Jain, (anand.jain@oracle.com)
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

#ifndef __ENCRYPT__
#define __ENCRYPT__
/*
 * Encryption sub features defines.
 */
#ifndef BTRFS_CRYPT_SUB_FEATURES
//testing
	//enable method
	#define BTRFS_CRYPTO_TEST_ENABLE_BYMNTOPT	0
	//key choice
	#define BTRFS_CRYPTO_TEST_BYDUMMYKEY		0 //off rest
	#define BTRFS_CRYPTO_TEST_BYDUMMYENC		0 //off rest

//debug
	#define BTRFS_CRYPTO_INFO_POTENTIAL_BUG 	1

//feature
	#define BTRFS_CRYPTO_KEY_TYPE_LOGON		1
#endif

#define BTRFS_CRYPTO_TFM_NAME_SIZE	16
#define BTRFS_CRYPTO_KEYTAG_SIZE	16
#define BTRFS_CRYPTO_KEY_SIZE		16
#define BTRFS_CRYPTO_IV_SIZE		16
#define BTRFS_CRYPTO_IV_IV 	\
	"\x12\x34\x56\x78\x90\xab\xcd\xef\x12\x34\x56\x78\x90\xab\xcd\xef"
#if BTRFS_CRYPTO_KEY_TYPE_LOGON
	#define BTRFS_CRYPTO_KEY_TYPE &key_type_logon
#else
	#define BTRFS_CRYPTO_KEY_TYPE &key_type_user
#endif

struct btrfs_ablkcipher_result {
	struct completion completion;
	int err;
};

struct btrfs_ablkcipher_req_data {
	char cipher_name[17];
	struct scatterlist sg_src;
	struct crypto_ablkcipher *tfm;
	struct ablkcipher_request *req;
	unsigned char key[BTRFS_CRYPTO_KEY_SIZE];
	size_t key_len;
	unsigned char iv[BTRFS_CRYPTO_IV_SIZE];
	size_t iv_size;
	struct btrfs_ablkcipher_result cb_result;
};

struct btrfs_blkcipher_req {
	unsigned char key[BTRFS_CRYPTO_KEY_SIZE];
	size_t key_len;
	unsigned char cryptoiv[BTRFS_CRYPTO_IV_SIZE];
	size_t iv_len;
};

int get_encrypt_type_index(char *type_name);
size_t get_encrypt_type_len(char *encryption_type);
int btrfs_update_key_to_binode(struct inode *inode);
int btrfs_validate_keytag(struct inode *inode, unsigned char *keytag);
int btrfs_check_keytag(char *keytag);
int btrfs_set_keyhash(struct inode *inode, char *keytag);
int btrfs_request_key(char *key_tag, void *key_data);
int btrfs_key_get(struct inode *inode);
void btrfs_key_put(struct inode *inode);
int btrfs_check_key_access(struct inode *inode);
int btrfs_do_ablkcipher(int enc, struct page *page, unsigned long len,
			struct btrfs_ablkcipher_req_data *btrfs_req);
int btrfs_get_master_key(struct inode *inode,
					unsigned char *keydata);
int btrfs_cipher_iv(int encrypt, struct inode *inode,
					char *data, size_t len);
void btrfs_disable_encrypt_inode(struct inode *inode);
void print_hex(char *key, size_t len, char *prefix);
#endif
