/*
 * fscrypt_private.h
 *
 * Copyright (C) 2015, Google, Inc.
 *
 * This contains encryption key functions.
 *
 * Written by Michael Halcrow, Ildar Muslukhov, and Uday Savagaonkar, 2015.
 */

#ifndef _FSCRYPT_PRIVATE_H
#define _FSCRYPT_PRIVATE_H

#include <linux/fscrypt_supp.h>
#include <crypto/hash.h>

/* Encryption parameters */
#define FS_IV_SIZE			16
#define FS_AES_128_ECB_KEY_SIZE		16
#define FS_AES_128_CBC_KEY_SIZE		16
#define FS_AES_128_CTS_KEY_SIZE		16
#define FS_AES_256_GCM_KEY_SIZE		32
#define FS_AES_256_CBC_KEY_SIZE		32
#define FS_AES_256_CTS_KEY_SIZE		32
#define FS_AES_256_XTS_KEY_SIZE		64

#define FS_KEY_DERIVATION_NONCE_SIZE		16
#define FSCRYPT_KEY_HASH_SIZE		16

/**
 * fscrypt_context - the encryption context for an inode
 *
 * Filesystems usually store this in an extended attribute.  It identifies the
 * encryption algorithm and key with which the file is encrypted.
 */
struct fscrypt_context {
	/* v1+ */

	/* Version of this structure */
	u8 version;

	/* Encryption mode for the contents of regular files */
	u8 contents_encryption_mode;

	/* Encryption mode for filenames in directories and symlink targets */
	u8 filenames_encryption_mode;

	/* Options that affect how encryption is done (e.g. padding amount) */
	u8 flags;

	/* Descriptor for this file's master key in the keyring */
	u8 master_key_descriptor[FS_KEY_DESCRIPTOR_SIZE];

	/*
	 * A unique value used in combination with the master key to derive the
	 * file's actual encryption key
	 */
	u8 nonce[FS_KEY_DERIVATION_NONCE_SIZE];

	/* v2+ */

	/* Cryptographically secure hash of the master key */
	u8 key_hash[FSCRYPT_KEY_HASH_SIZE];
};

#define FSCRYPT_CONTEXT_V1	1
#define FSCRYPT_CONTEXT_V1_SIZE	offsetof(struct fscrypt_context, key_hash)

#define FSCRYPT_CONTEXT_V2	2
#define FSCRYPT_CONTEXT_V2_SIZE	sizeof(struct fscrypt_context)

static inline int fscrypt_context_size(const struct fscrypt_context *ctx)
{
	switch (ctx->version) {
	case FSCRYPT_CONTEXT_V1:
		return FSCRYPT_CONTEXT_V1_SIZE;
	case FSCRYPT_CONTEXT_V2:
		return FSCRYPT_CONTEXT_V2_SIZE;
	}
	return 0;
}

static inline bool
fscrypt_valid_context_format(const struct fscrypt_context *ctx, int size)
{
	return size >= 1 && size == fscrypt_context_size(ctx);
}

/*
 * fscrypt_info - the "encryption key" for an inode
 *
 * When an encrypted file's key is made available, an instance of this struct is
 * allocated and stored in ->i_crypt_info.  Once created, it remains until the
 * inode is evicted.
 */
struct fscrypt_info {

	/* The actual crypto transforms needed for encryption and decryption */
	struct crypto_skcipher *ci_ctfm;
	struct crypto_cipher *ci_essiv_tfm;

	/*
	 * Cached fields from the fscrypt_context needed for encryption policy
	 * inheritance and enforcement
	 */
	u8 ci_context_version;
	u8 ci_data_mode;
	u8 ci_filename_mode;
	u8 ci_flags;
	u8 ci_master_key[FS_KEY_DESCRIPTOR_SIZE];
};

typedef enum {
	FS_DECRYPT = 0,
	FS_ENCRYPT,
} fscrypt_direction_t;

#define FS_CTX_REQUIRES_FREE_ENCRYPT_FL		0x00000001
#define FS_CTX_HAS_BOUNCE_BUFFER_FL		0x00000002

struct fscrypt_completion_result {
	struct completion completion;
	int res;
};

#define DECLARE_FS_COMPLETION_RESULT(ecr) \
	struct fscrypt_completion_result ecr = { \
		COMPLETION_INITIALIZER_ONSTACK((ecr).completion), 0 }


/* crypto.c */
extern int fscrypt_initialize(unsigned int cop_flags);
extern struct workqueue_struct *fscrypt_read_workqueue;
extern int fscrypt_do_page_crypto(const struct inode *inode,
				  fscrypt_direction_t rw, u64 lblk_num,
				  struct page *src_page,
				  struct page *dest_page,
				  unsigned int len, unsigned int offs,
				  gfp_t gfp_flags);
extern struct page *fscrypt_alloc_bounce_page(struct fscrypt_ctx *ctx,
					      gfp_t gfp_flags);

/* keyinfo.c */
extern void __exit fscrypt_essiv_cleanup(void);

#endif /* _FSCRYPT_PRIVATE_H */
