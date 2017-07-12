/*
 * key management facility for FS encryption support.
 *
 * Copyright (C) 2015, Google, Inc.
 *
 * This contains encryption key functions.
 *
 * Written by Michael Halcrow, Ildar Muslukhov, and Uday Savagaonkar, 2015.
 * HKDF support added by Eric Biggers, 2017.
 *
 * The implementation and usage of HKDF should conform to RFC-5869 ("HMAC-based
 * Extract-and-Expand Key Derivation Function").
 */

#include <keys/user-type.h>
#include <linux/scatterlist.h>
#include <linux/ratelimit.h>
#include <crypto/aes.h>
#include <crypto/hash.h>
#include <crypto/sha.h>
#include "fscrypt_private.h"

static struct crypto_shash *essiv_hash_tfm;

/*
 * Any unkeyed cryptographic hash algorithm can be used with HKDF, but we use
 * SHA-512 because it is reasonably secure and efficient; and since it produces
 * a 64-byte digest, deriving an AES-256-XTS key preserves all 64 bytes of
 * entropy from the master key and requires only one iteration of HKDF-Expand.
 */
#define HKDF_HMAC_ALG		"hmac(sha512)"
#define HKDF_HASHLEN		SHA512_DIGEST_SIZE

/*
 * The list of contexts in which we use HKDF to derive additional keys from a
 * master key.  The values in this list are used as the first byte of the
 * application-specific info string to guarantee that info strings are never
 * repeated between contexts.
 *
 * Keys derived with different info strings are cryptographically isolated from
 * each other --- knowledge of one derived key doesn't reveal any others.
 * (This property is particularly important for the derived key used as the
 * "key hash", as that is stored in the clear.)
 */
#define HKDF_CONTEXT_PER_FILE_KEY	1
#define HKDF_CONTEXT_KEY_HASH		2

/*
 * HKDF consists of two steps:
 *
 * 1. HKDF-Extract: extract a fixed-length pseudorandom key from the
 *    input keying material and optional salt.
 * 2. HDKF-Expand: expand the pseudorandom key into output keying material of
 *    any length, parameterized by an application-specific info string.
 *
 * HKDF-Extract can be skipped if the input is already a good pseudorandom key
 * that is at least as long as the hash.  While the fscrypt master keys should
 * already be good pseudorandom keys, when using encryption algorithms that use
 * short keys (e.g. AES-128-CBC) we'd like to permit the master key to be
 * shorter than HKDF_HASHLEN bytes.  Thus, we still must do HKDF-Extract.
 *
 * Ideally, HKDF-Extract would be passed a random salt for each distinct input
 * key.  Details about the advantages of a random salt can be found in the HKDF
 * paper (Krawczyk, 2010; "Cryptographic Extraction and Key Derivation: The HKDF
 * Scheme").  However, we do not have the ability to store a salt on a
 * per-master-key basis.  Thus, we have to use a fixed salt.  This is sufficient
 * as long as the master keys are already pseudorandom and are long enough to
 * make dictionary attacks infeasible.  This should be the case if userspace
 * used a cryptographically secure random number generator, e.g. /dev/urandom,
 * to generate the master keys.
 *
 * For the fixed salt we use "fscrypt_hkdf_salt" rather than default of all 0's
 * defined by RFC-5869.  This is only to be slightly more robust against
 * userspace (unwisely) reusing the master keys for different purposes.
 * Logically, it's more likely that the keys would be passed to unsalted
 * HKDF-SHA512 than specifically to "fscrypt_hkdf_salt"-salted HKDF-SHA512.
 * (Of course, a random salt would be better for this purpose.)
 */

#define HKDF_SALT		"fscrypt_hkdf_salt"
#define HKDF_SALT_LEN		(sizeof(HKDF_SALT) - 1)

/*
 * HKDF-Extract (RFC-5869 section 2.2).  This extracts a pseudorandom key 'prk'
 * from the input key material 'ikm' and a salt.  (See explanation above for why
 * we use a fixed salt.)
 */
static int hkdf_extract(struct crypto_shash *hmac,
			const u8 *ikm, unsigned int ikmlen,
			u8 prk[HKDF_HASHLEN])
{
	SHASH_DESC_ON_STACK(desc, hmac);
	int err;

	desc->tfm = hmac;
	desc->flags = 0;

	err = crypto_shash_setkey(hmac, HKDF_SALT, HKDF_SALT_LEN);
	if (err)
		goto out;

	err = crypto_shash_digest(desc, ikm, ikmlen, prk);
out:
	shash_desc_zero(desc);
	return err;
}

/*
 * HKDF-Expand (RFC-5869 section 2.3).  This expands the pseudorandom key, which
 * has already been keyed into 'hmac', into 'okmlen' bytes of output keying
 * material, parameterized by the application-specific information string of
 * 'info' prefixed with the 'context' byte.  ('context' isn't part of the HKDF
 * specification; it's just a prefix we add to our application-specific info
 * strings to guarantee that we don't accidentally repeat an info string when
 * using HKDF for different purposes.)
 */
static int hkdf_expand(struct crypto_shash *hmac, u8 context,
		       const u8 *info, unsigned int infolen,
		       u8 *okm, unsigned int okmlen)
{
	SHASH_DESC_ON_STACK(desc, hmac);
	int err;
	const u8 *prev = NULL;
	unsigned int i;
	u8 counter = 1;
	u8 tmp[HKDF_HASHLEN];

	desc->tfm = hmac;
	desc->flags = 0;

	if (unlikely(okmlen > 255 * HKDF_HASHLEN))
		return -EINVAL;

	for (i = 0; i < okmlen; i += HKDF_HASHLEN) {

		err = crypto_shash_init(desc);
		if (err)
			goto out;

		if (prev) {
			err = crypto_shash_update(desc, prev, HKDF_HASHLEN);
			if (err)
				goto out;
		}

		err = crypto_shash_update(desc, &context, 1);
		if (err)
			goto out;

		err = crypto_shash_update(desc, info, infolen);
		if (err)
			goto out;

		if (okmlen - i < HKDF_HASHLEN) {
			err = crypto_shash_finup(desc, &counter, 1, tmp);
			if (err)
				goto out;
			memcpy(&okm[i], tmp, okmlen - i);
			memzero_explicit(tmp, sizeof(tmp));
		} else {
			err = crypto_shash_finup(desc, &counter, 1, &okm[i]);
			if (err)
				goto out;
		}
		counter++;
		prev = &okm[i];
	}
	err = 0;
out:
	shash_desc_zero(desc);
	return err;
}

static void put_master_key(struct fscrypt_master_key *k)
{
	if (!k)
		return;

	crypto_free_shash(k->mk_hmac);
	kzfree(k);
}

/*
 * Allocate a fscrypt_master_key, given the keyring key payload.  This includes
 * allocating and keying an HMAC transform so that we can efficiently derive
 * the per-inode encryption keys with HKDF-Expand later.
 */
static struct fscrypt_master_key *
alloc_master_key(const struct fscrypt_key *payload)
{
	struct fscrypt_master_key *k;
	int err;
	u8 prk[HKDF_HASHLEN];

	k = kzalloc(sizeof(*k), GFP_NOFS);
	if (!k)
		return ERR_PTR(-ENOMEM);
	k->mk_size = payload->size;

	k->mk_hmac = crypto_alloc_shash(HKDF_HMAC_ALG, 0, 0);
	if (IS_ERR(k->mk_hmac)) {
		err = PTR_ERR(k->mk_hmac);
		k->mk_hmac = NULL;
		pr_warn("fscrypt: error allocating " HKDF_HMAC_ALG ": %d\n",
			err);
		goto fail;
	}

	BUG_ON(crypto_shash_digestsize(k->mk_hmac) != sizeof(prk));

	err = hkdf_extract(k->mk_hmac, payload->raw, payload->size, prk);
	if (err)
		goto fail;

	err = crypto_shash_setkey(k->mk_hmac, prk, sizeof(prk));
	if (err)
		goto fail;

	/* Calculate the "key hash" */
	err = hkdf_expand(k->mk_hmac, HKDF_CONTEXT_KEY_HASH, NULL, 0,
			  k->mk_hash, FSCRYPT_KEY_HASH_SIZE);
	if (err)
		goto fail;
out:
	memzero_explicit(prk, sizeof(prk));
	return k;

fail:
	put_master_key(k);
	k = ERR_PTR(err);
	goto out;
}

static void release_keyring_key(struct key *keyring_key)
{
	up_read(&keyring_key->sem);
	key_put(keyring_key);
}

/*
 * Find, lock, and validate the master key with the keyring description
 * prefix:descriptor.  It must be released with release_keyring_key() later.
 */
static struct key *
find_and_lock_keyring_key(const char *prefix,
			  const u8 descriptor[FS_KEY_DESCRIPTOR_SIZE],
			  unsigned int min_keysize,
			  const struct fscrypt_key **payload_ret)
{
	char *description;
	struct key *keyring_key;
	const struct user_key_payload *ukp;
	const struct fscrypt_key *payload;

	description = kasprintf(GFP_NOFS, "%s%*phN", prefix,
				FS_KEY_DESCRIPTOR_SIZE, descriptor);
	if (!description)
		return ERR_PTR(-ENOMEM);

	keyring_key = request_key(&key_type_logon, description, NULL);
	if (IS_ERR(keyring_key))
		goto out;

	down_read(&keyring_key->sem);
	ukp = user_key_payload_locked(keyring_key);
	payload = (const struct fscrypt_key *)ukp->data;

	if (ukp->datalen != sizeof(struct fscrypt_key) ||
	    payload->size == 0 || payload->size > FS_MAX_KEY_SIZE) {
		pr_warn_ratelimited("fscrypt: key '%s' has invalid payload\n",
				    description);
		goto invalid;
	}

	/*
	 * With the legacy AES-based KDF the master key must be at least as long
	 * as the derived key.  With HKDF we could accept a shorter master key;
	 * however, that would mean the derived key wouldn't contain as much
	 * entropy as intended.  So don't allow it in either case.
	 */
	if (payload->size < min_keysize) {
		pr_warn_ratelimited("fscrypt: key '%s' is too short (got %u bytes, wanted %u+ bytes)\n",
				    description, payload->size, min_keysize);
		goto invalid;
	}

	*payload_ret = payload;
out:
	kfree(description);
	return keyring_key;

invalid:
	release_keyring_key(keyring_key);
	keyring_key = ERR_PTR(-ENOKEY);
	goto out;
}

static struct fscrypt_master_key *
load_master_key_from_keyring(const struct inode *inode,
			     const u8 descriptor[FS_KEY_DESCRIPTOR_SIZE],
			     unsigned int min_keysize)
{
	struct key *keyring_key;
	const struct fscrypt_key *payload;
	struct fscrypt_master_key *master_key;

	keyring_key = find_and_lock_keyring_key(FS_KEY_DESC_PREFIX, descriptor,
						min_keysize, &payload);
	if (keyring_key == ERR_PTR(-ENOKEY) && inode->i_sb->s_cop->key_prefix) {
		keyring_key = find_and_lock_keyring_key(
					inode->i_sb->s_cop->key_prefix,
					descriptor, min_keysize, &payload);
	}
	if (IS_ERR(keyring_key))
		return ERR_CAST(keyring_key);

	master_key = alloc_master_key(payload);

	release_keyring_key(keyring_key);

	return master_key;
}

static void derive_crypt_complete(struct crypto_async_request *req, int rc)
{
	struct fscrypt_completion_result *ecr = req->data;

	if (rc == -EINPROGRESS)
		return;

	ecr->res = rc;
	complete(&ecr->completion);
}

/*
 * Legacy key derivation function.  This generates the derived key by encrypting
 * the master key with AES-128-ECB using the nonce as the AES key.  This
 * provides a unique derived key for each inode, but it's nonstandard, isn't
 * very extensible, and has the weakness that it's trivially reversible: an
 * attacker who compromises a derived key, e.g. with a side channel attack, can
 * "decrypt" it to get back to the master key, then derive any other key.
 */
static int derive_key_aes(const struct fscrypt_key *master_key,
			  const struct fscrypt_context *ctx,
			  u8 *derived_key, unsigned int derived_keysize)
{
	int err;
	struct skcipher_request *req = NULL;
	DECLARE_FS_COMPLETION_RESULT(ecr);
	struct scatterlist src_sg, dst_sg;
	struct crypto_skcipher *tfm;

	tfm = crypto_alloc_skcipher("ecb(aes)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	crypto_skcipher_set_flags(tfm, CRYPTO_TFM_REQ_WEAK_KEY);
	req = skcipher_request_alloc(tfm, GFP_NOFS);
	if (!req) {
		err = -ENOMEM;
		goto out;
	}
	skcipher_request_set_callback(req,
			CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
			derive_crypt_complete, &ecr);

	BUILD_BUG_ON(sizeof(ctx->nonce) != FS_AES_128_ECB_KEY_SIZE);
	err = crypto_skcipher_setkey(tfm, ctx->nonce, sizeof(ctx->nonce));
	if (err)
		goto out;

	sg_init_one(&src_sg, master_key->raw, derived_keysize);
	sg_init_one(&dst_sg, derived_key, derived_keysize);
	skcipher_request_set_crypt(req, &src_sg, &dst_sg, derived_keysize,
				   NULL);
	err = crypto_skcipher_encrypt(req);
	if (err == -EINPROGRESS || err == -EBUSY) {
		wait_for_completion(&ecr.completion);
		err = ecr.res;
	}
out:
	skcipher_request_free(req);
	crypto_free_skcipher(tfm);
	return err;
}

/*
 * HKDF-based key derivation function.  This uses HKDF-SHA512 to derive a unique
 * encryption key for each inode, using the inode's nonce prefixed with a
 * context byte as the application-specific information string.  This is more
 * flexible than the legacy AES-based KDF and has the advantage that it's
 * non-reversible: an attacker who compromises a derived key cannot calculate
 * the master key or any other derived keys.
 */
static int derive_key_hkdf(const struct fscrypt_master_key *master_key,
			   const struct fscrypt_context *ctx,
			   u8 *derived_key, unsigned int derived_keysize)
{
	return hkdf_expand(master_key->mk_hmac, HKDF_CONTEXT_PER_FILE_KEY,
			   ctx->nonce, sizeof(ctx->nonce),
			   derived_key, derived_keysize);
}

static int find_and_derive_key_v1(const struct inode *inode,
				  const struct fscrypt_context *ctx,
				  u8 *derived_key, unsigned int derived_keysize)
{
	struct key *keyring_key;
	const struct fscrypt_key *payload;
	int err;

	keyring_key = find_and_lock_keyring_key(FS_KEY_DESC_PREFIX,
						ctx->master_key_descriptor,
						derived_keysize, &payload);
	if (keyring_key == ERR_PTR(-ENOKEY) && inode->i_sb->s_cop->key_prefix) {
		keyring_key = find_and_lock_keyring_key(
					inode->i_sb->s_cop->key_prefix,
					ctx->master_key_descriptor,
					derived_keysize, &payload);
	}
	if (IS_ERR(keyring_key))
		return PTR_ERR(keyring_key);

	err = derive_key_aes(payload, ctx, derived_key, derived_keysize);

	release_keyring_key(keyring_key);

	return err;
}

static const struct {
	const char *cipher_str;
	int keysize;
} available_modes[] = {
	[FS_ENCRYPTION_MODE_AES_256_XTS] = { "xts(aes)",
					     FS_AES_256_XTS_KEY_SIZE },
	[FS_ENCRYPTION_MODE_AES_256_CTS] = { "cts(cbc(aes))",
					     FS_AES_256_CTS_KEY_SIZE },
	[FS_ENCRYPTION_MODE_AES_128_CBC] = { "cbc(aes)",
					     FS_AES_128_CBC_KEY_SIZE },
	[FS_ENCRYPTION_MODE_AES_128_CTS] = { "cts(cbc(aes))",
					     FS_AES_128_CTS_KEY_SIZE },
};

static int determine_cipher_type(struct fscrypt_info *ci, struct inode *inode,
				 const char **cipher_str_ret, int *keysize_ret)
{
	u32 mode;

	if (!fscrypt_valid_enc_modes(ci->ci_data_mode, ci->ci_filename_mode)) {
		pr_warn_ratelimited("fscrypt: inode %lu uses unsupported encryption modes (contents mode %d, filenames mode %d)\n",
				    inode->i_ino,
				    ci->ci_data_mode, ci->ci_filename_mode);
		return -EINVAL;
	}

	if (S_ISREG(inode->i_mode)) {
		mode = ci->ci_data_mode;
	} else if (S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode)) {
		mode = ci->ci_filename_mode;
	} else {
		WARN_ONCE(1, "fscrypt: filesystem tried to load encryption info for inode %lu, which is not encryptable (file type %d)\n",
			  inode->i_ino, (inode->i_mode & S_IFMT));
		return -EINVAL;
	}

	*cipher_str_ret = available_modes[mode].cipher_str;
	*keysize_ret = available_modes[mode].keysize;
	return 0;
}

static void put_crypt_info(struct fscrypt_info *ci)
{
	if (!ci)
		return;

	crypto_free_skcipher(ci->ci_ctfm);
	crypto_free_cipher(ci->ci_essiv_tfm);
	put_master_key(ci->ci_master_key);
	kmem_cache_free(fscrypt_info_cachep, ci);
}

static int derive_essiv_salt(const u8 *key, int keysize, u8 *salt)
{
	struct crypto_shash *tfm = READ_ONCE(essiv_hash_tfm);

	/* init hash transform on demand */
	if (unlikely(!tfm)) {
		struct crypto_shash *prev_tfm;

		tfm = crypto_alloc_shash("sha256", 0, 0);
		if (IS_ERR(tfm)) {
			pr_warn_ratelimited("fscrypt: error allocating SHA-256 transform: %ld\n",
					    PTR_ERR(tfm));
			return PTR_ERR(tfm);
		}
		prev_tfm = cmpxchg(&essiv_hash_tfm, NULL, tfm);
		if (prev_tfm) {
			crypto_free_shash(tfm);
			tfm = prev_tfm;
		}
	}

	{
		SHASH_DESC_ON_STACK(desc, tfm);
		desc->tfm = tfm;
		desc->flags = 0;

		return crypto_shash_digest(desc, key, keysize, salt);
	}
}

static int init_essiv_generator(struct fscrypt_info *ci, const u8 *raw_key,
				int keysize)
{
	int err;
	struct crypto_cipher *essiv_tfm;
	u8 salt[SHA256_DIGEST_SIZE];

	essiv_tfm = crypto_alloc_cipher("aes", 0, 0);
	if (IS_ERR(essiv_tfm))
		return PTR_ERR(essiv_tfm);

	ci->ci_essiv_tfm = essiv_tfm;

	err = derive_essiv_salt(raw_key, keysize, salt);
	if (err)
		goto out;

	/*
	 * Using SHA256 to derive the salt/key will result in AES-256 being
	 * used for IV generation. File contents encryption will still use the
	 * configured keysize (AES-128) nevertheless.
	 */
	err = crypto_cipher_setkey(essiv_tfm, salt, sizeof(salt));
	if (err)
		goto out;

out:
	memzero_explicit(salt, sizeof(salt));
	return err;
}

void __exit fscrypt_essiv_cleanup(void)
{
	crypto_free_shash(essiv_hash_tfm);
}

int fscrypt_compute_key_hash(const struct inode *inode,
			     const struct fscrypt_policy *policy,
			     u8 hash[FSCRYPT_KEY_HASH_SIZE])
{
	struct fscrypt_master_key *k;
	unsigned int min_keysize;

	/*
	 * Require that the master key be long enough for both the
	 * contents and filenames encryption modes.
	 */
	min_keysize =
		max(available_modes[policy->contents_encryption_mode].keysize,
		    available_modes[policy->filenames_encryption_mode].keysize);

	k = load_master_key_from_keyring(inode, policy->master_key_descriptor,
					 min_keysize);
	if (IS_ERR(k))
		return PTR_ERR(k);

	memcpy(hash, k->mk_hash, FSCRYPT_KEY_HASH_SIZE);
	put_master_key(k);
	return 0;
}

int fscrypt_get_encryption_info(struct inode *inode)
{
	struct fscrypt_info *crypt_info;
	struct fscrypt_context ctx;
	struct crypto_skcipher *ctfm;
	const char *cipher_str;
	unsigned int derived_keysize;
	u8 *derived_key = NULL;
	int res;

	if (inode->i_crypt_info)
		return 0;

	res = fscrypt_initialize(inode->i_sb->s_cop->flags);
	if (res)
		return res;

	res = inode->i_sb->s_cop->get_context(inode, &ctx, sizeof(ctx));
	if (res < 0) {
		if (!fscrypt_dummy_context_enabled(inode) ||
		    inode->i_sb->s_cop->is_encrypted(inode))
			return res;
		/* Fake up a context for an unencrypted directory */
		memset(&ctx, 0, sizeof(ctx));
		ctx.version = FSCRYPT_CONTEXT_V1;
		ctx.contents_encryption_mode = FS_ENCRYPTION_MODE_AES_256_XTS;
		ctx.filenames_encryption_mode = FS_ENCRYPTION_MODE_AES_256_CTS;
		memset(ctx.master_key_descriptor, 0x42, FS_KEY_DESCRIPTOR_SIZE);
		res = FSCRYPT_CONTEXT_V1_SIZE;
	}

	if (!fscrypt_valid_context_format(&ctx, res))
		return -EINVAL;

	if (ctx.flags & ~FS_POLICY_FLAGS_VALID)
		return -EINVAL;

	crypt_info = kmem_cache_zalloc(fscrypt_info_cachep, GFP_NOFS);
	if (!crypt_info)
		return -ENOMEM;

	crypt_info->ci_context_version = ctx.version;
	crypt_info->ci_data_mode = ctx.contents_encryption_mode;
	crypt_info->ci_filename_mode = ctx.filenames_encryption_mode;
	crypt_info->ci_flags = ctx.flags;
	memcpy(crypt_info->ci_master_key_descriptor, ctx.master_key_descriptor,
	       FS_KEY_DESCRIPTOR_SIZE);

	res = determine_cipher_type(crypt_info, inode, &cipher_str,
				    &derived_keysize);
	if (res)
		goto out;

	/*
	 * This cannot be a stack buffer because it may be passed to the
	 * scatterlist crypto API during key derivation.
	 */
	res = -ENOMEM;
	derived_key = kmalloc(FS_MAX_KEY_SIZE, GFP_NOFS);
	if (!derived_key)
		goto out;

	if (ctx.version == FSCRYPT_CONTEXT_V1) {
		res = find_and_derive_key_v1(inode, &ctx, derived_key,
					     derived_keysize);
	} else {
		crypt_info->ci_master_key =
			load_master_key_from_keyring(inode,
						     ctx.master_key_descriptor,
						     derived_keysize);
		if (IS_ERR(crypt_info->ci_master_key)) {
			res = PTR_ERR(crypt_info->ci_master_key);
			crypt_info->ci_master_key = NULL;
			goto out;
		}

		/*
		 * Make sure the master key we found has the correct hash.
		 * Buggy or malicious userspace may provide the wrong key.
		 */
		if (memcmp(crypt_info->ci_master_key->mk_hash, ctx.key_hash,
			   FSCRYPT_KEY_HASH_SIZE)) {
			pr_warn_ratelimited("fscrypt: wrong encryption key supplied for inode %lu\n",
					    inode->i_ino);
			res = -ENOKEY;
			goto out;
		}

		res = derive_key_hkdf(crypt_info->ci_master_key, &ctx,
				      derived_key, derived_keysize);
	}
	if (res)
		goto out;

	ctfm = crypto_alloc_skcipher(cipher_str, 0, 0);
	if (!ctfm || IS_ERR(ctfm)) {
		res = ctfm ? PTR_ERR(ctfm) : -ENOMEM;
		pr_debug("%s: error %d (inode %lu) allocating crypto tfm\n",
			 __func__, res, inode->i_ino);
		goto out;
	}
	crypt_info->ci_ctfm = ctfm;
	crypto_skcipher_clear_flags(ctfm, ~0);
	crypto_skcipher_set_flags(ctfm, CRYPTO_TFM_REQ_WEAK_KEY);
	res = crypto_skcipher_setkey(ctfm, derived_key, derived_keysize);
	if (res)
		goto out;

	if (S_ISREG(inode->i_mode) &&
	    crypt_info->ci_data_mode == FS_ENCRYPTION_MODE_AES_128_CBC) {
		res = init_essiv_generator(crypt_info, derived_key,
					   derived_keysize);
		if (res) {
			pr_debug("%s: error %d (inode %lu) allocating essiv tfm\n",
				 __func__, res, inode->i_ino);
			goto out;
		}
	}
	if (cmpxchg(&inode->i_crypt_info, NULL, crypt_info) == NULL)
		crypt_info = NULL;
out:
	if (res == -ENOKEY)
		res = 0;
	put_crypt_info(crypt_info);
	kzfree(derived_key);
	return res;
}
EXPORT_SYMBOL(fscrypt_get_encryption_info);

void fscrypt_put_encryption_info(struct inode *inode, struct fscrypt_info *ci)
{
	struct fscrypt_info *prev;

	if (ci == NULL)
		ci = ACCESS_ONCE(inode->i_crypt_info);
	if (ci == NULL)
		return;

	prev = cmpxchg(&inode->i_crypt_info, ci, NULL);
	if (prev != ci)
		return;

	put_crypt_info(ci);
}
EXPORT_SYMBOL(fscrypt_put_encryption_info);
