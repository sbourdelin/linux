/* System hash blacklist.
 *
 * Copyright (C) 2016 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

#define pr_fmt(fmt) "blacklist: "fmt
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/key.h>
#include <linux/key-type.h>
#include <linux/sched.h>
#include <linux/ctype.h>
#include <linux/err.h>
#include <linux/seq_file.h>
#include <crypto/hash.h>
#include <keys/system_keyring.h>
#include "blacklist.h"

struct blacklist_hash {
	struct blacklist_hash *next;
	u8 name_len;
	char name[];
};

static struct key *blacklist_keyring;
static struct blacklist_hash blacklist_sha256 = { NULL, 6, "sha256" };
static struct blacklist_hash *blacklist_hash_types = &blacklist_sha256;
static DEFINE_SPINLOCK(blacklist_hash_types_lock);

static const struct blacklist_hash *blacklist_hash_type(const char *hash_algo,
							size_t name_len)
{
	const struct blacklist_hash *bhash;

	bhash = blacklist_hash_types;
	smp_rmb(); /* Content after pointer.  List tail is immutable */
	for (; bhash; bhash = bhash->next)
		if (name_len == bhash->name_len &&
		    memcmp(hash_algo, bhash->name, name_len) == 0)
			return bhash;
	return NULL;
}

/*
 * The description must be a type prefix, a colon and then an even number of
 * hex digits then optionally another colon and the hash type.  If the hash
 * type isn't specified, it's assumed to be SHAnnn where nnn is the number of
 * bits in the hash.
 *
 * The hash data is kept in the description.
 */
static int blacklist_vet_description(const char *desc)
{
	int n = 0;

	if (*desc == ':')
		return -EINVAL;
	for (; *desc; desc++)
		if (*desc == ':')
			goto found_colon;
	return -EINVAL;

found_colon:
	desc++;
	for (; *desc; desc++) {
		if (!isxdigit(*desc))
			goto found_hash_algo;
		n++;
	}

	if (n == 0 || n & 1)
		return -EINVAL;
	return 0;

found_hash_algo:
	if (*desc != ':')
		return -EINVAL;
	return 0;
}

/*
 * The hash to be blacklisted is expected to be in the description.  There will
 * be no payload.
 */
static int blacklist_preparse(struct key_preparsed_payload *prep)
{
	if (prep->datalen > 0)
		return -EINVAL;
	return 0;
}

static void blacklist_free_preparse(struct key_preparsed_payload *prep)
{
}

static void blacklist_describe(const struct key *key, struct seq_file *m)
{
	seq_puts(m, key->description);
}

static struct key_type key_type_blacklist = {
	.name			= "blacklist",
	.vet_description	= blacklist_vet_description,
	.preparse		= blacklist_preparse,
	.free_preparse		= blacklist_free_preparse,
	.instantiate		= generic_key_instantiate,
	.describe		= blacklist_describe,
};

/*
 * Extract the type.
 */
static const char *blacklist_extract_type(const char *desc, size_t *_len)
{
	const char *h, *algo;
	size_t len;

	/* Prepare a hash record if this is a new hash.  It may be discarded
	 * during instantiation if we find we raced with someone else.
	 */
	h = strchr(desc, ':');
	if (!h)
		return NULL;
	h++;
	algo = strchr(h, ':');
	if (algo) {
		algo++;
		len = strlen(algo);
		if (len <= 0 || len > 255)
			return NULL;
	} else {
		/* The hash wasn't specified - assume it to be the SHA with the
		 * same number of bits as the hash data.
		 */
		len = strlen(h) * 4;
		switch (len) {
		case 160:
			algo = "sha1";
			break;
		case 224:
			algo = "sha224";
			break;
		case 256:
			algo = "sha256";
			break;
		case 384:
			algo = "sha384";
			break;
		case 512:
			algo = "sha512";
			break;
		default:
			return NULL;
		}
	}

	*_len = strlen(algo);
	return algo;
}

/*
 * Make sure the type is listed.
 */
static int blacklist_add_type(const char *desc)
{
	struct blacklist_hash *bhash;
	const char *algo;
	size_t len;

	algo = blacklist_extract_type(desc, &len);
	if (!algo)
		return -EINVAL;

	if (blacklist_hash_type(algo, len))
		return 0;

	bhash = kmalloc(sizeof(*bhash) + len + 1, GFP_KERNEL);
	if (!bhash)
		return -ENOMEM;
	memcpy(bhash->name, algo, len);
	bhash->name[len] = 0;

	spin_lock(&blacklist_hash_types_lock);
	if (!blacklist_hash_type(bhash->name, bhash->name_len)) {
		bhash->next = blacklist_hash_types;
		smp_wmb(); /* Content before pointer */
		blacklist_hash_types = bhash;
		bhash = NULL;
	}
	spin_unlock(&blacklist_hash_types_lock);

	kfree(bhash);
	return 0;
}

/**
 * mark_hash_blacklisted - Add a hash to the system blacklist
 * @hash - The hash as a formatted string.
 *
 * The hash string is formatted as:
 *
 *	"<type>:<hash-as-hex>[:<algo>]"
 *
 * Where <algo> is optional and defaults to shaNNN where NNN is the number of
 * bits in hash-as-hex, eg.:
 *
 *	"tbs:23aa429783:foohash"
 *
 * The hash must have all leading zeros present.
 */
int mark_hash_blacklisted(const char *hash)
{
	key_ref_t key;
	int ret;

	ret = blacklist_add_type(hash);
	if (ret < 0)
		return ret;

	key = key_create_or_update(make_key_ref(blacklist_keyring, true),
				   "blacklist",
				   hash,
				   NULL,
				   0,
				   ((KEY_POS_ALL & ~KEY_POS_SETATTR) |
				    KEY_USR_VIEW),
				   KEY_ALLOC_NOT_IN_QUOTA |
				   KEY_ALLOC_BUILT_IN);
	if (IS_ERR(key)) {
		pr_err("Problem blacklisting hash (%ld)\n", PTR_ERR(key));
		return PTR_ERR(key);
	}
	return 0;
}

/**
 * is_hash_blacklisted - Determine if a hash is blacklisted
 * @hash: The hash to be checked as a binary blob
 * @hash_len: The length of the binary hash
 * @type: Type of hash
 * @hash_algo: Hash algorithm used
 */
int is_hash_blacklisted(const u8 *hash, size_t hash_len, const char *type,
			const char *hash_algo)
{
	key_ref_t kref;
	size_t type_len = strlen(type), hash_algo_len = strlen(hash);
	char *buffer, *p;
	int ret = 0;

	buffer = kmalloc(type_len + 1 + hash_len * 2 + 1 + hash_algo_len + 1,
			 GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	p = memcpy(buffer, type, type_len);
	p += type_len;
	*p++ = ':';
	bin2hex(p, hash, hash_len);
	p += hash_len * 2;
	*p++ = ':';
	p = memcpy(p, hash_algo, hash_algo_len);
	p[hash_algo_len] = 0;

	kref = keyring_search(make_key_ref(blacklist_keyring, true),
			      &key_type_blacklist, buffer);
	if (!IS_ERR(kref))
		goto black;

	/* For SHA hashes, the hash type is optional. */
	if (hash_algo[0] == 's' &&
	    hash_algo[1] == 'h' &&
	    hash_algo[2] == 'a') {
		p[-1] = 0;

		kref = keyring_search(make_key_ref(blacklist_keyring, true),
				      &key_type_blacklist, buffer);
		if (!IS_ERR(kref))
			goto black;
	}

out:
	kfree(buffer);
	return ret;

black:
	key_ref_put(kref);
	ret = -EKEYREJECTED;
	goto out;
}
EXPORT_SYMBOL_GPL(is_hash_blacklisted);

/*
 * Test the blacklistedness of one combination of data and hash.
 */
static int blacklist_test_one(const void *data, size_t data_len,
			      const char *type, const char *hash_algo)
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	size_t digest_size, desc_size;
	u8 *digest;
	int ret;

	/* Allocate the hashing algorithm we're going to need and find out how
	 * big the hash operational data will be.  We skip any hash type for
	 * which we don't have a crypto module available.
	 */
	tfm = crypto_alloc_shash(hash_algo, 0, 0);
	if (IS_ERR(tfm))
		return (PTR_ERR(tfm) == -ENOENT) ? 0 : PTR_ERR(tfm);

	desc_size = crypto_shash_descsize(tfm) + sizeof(*desc);
	digest_size = crypto_shash_digestsize(tfm);

	ret = -ENOMEM;
	digest = kmalloc(digest_size, GFP_KERNEL);
	if (!digest)
		goto error_tfm;

	desc = kzalloc(desc_size, GFP_KERNEL);
	if (!desc)
		goto error_digest;

	desc->tfm   = tfm;
	desc->flags = CRYPTO_TFM_REQ_MAY_SLEEP;

	/* Digest the message [RFC2315 9.3] */
	ret = crypto_shash_init(desc);
	if (ret < 0)
		goto error_desc;
	ret = crypto_shash_finup(desc, data, data_len, digest);
	if (ret < 0)
		goto error_desc;

	ret = is_hash_blacklisted(digest, digest_size, type, hash_algo);
error_desc:
	kfree(desc);
error_digest:
	kfree(digest);
error_tfm:
	crypto_free_shash(tfm);
	return ret;
}

/**
 * is_data_blacklisted - Determine if a data blob is blacklisted
 * @data: The data to check.
 * @data_len: The amount of data.
 * @type: The type of object.
 * @skip_hash: A hash type to skip
 *
 * Iterate through all the types of hash for which we have blacklisted hashes
 * and generate a hash for each and check it against the blacklist.
 *
 * If the caller has a precomputed hash, they can call is_hash_blacklisted() on
 * it and then call this function with @skip_hash set to the hash type to skip.
 */
int is_data_blacklisted(const void *data, size_t data_len,
			const char *type, const char *skip_hash)
{
	const struct blacklist_hash *bhash;
	int ret = 0;

	bhash = blacklist_hash_types;
	smp_rmb(); /* Content after pointer.  List tail is immutable */

	for (; bhash; bhash = bhash->next) {
		if (strcmp(skip_hash, bhash->name) == 0)
			continue;
		ret = blacklist_test_one(data, data_len, type, bhash->name);
		if (ret < 0)
			return ret;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(is_data_blacklisted);

/*
 * Initialise the blacklist
 */
static int __init blacklist_init(void)
{
	const char *const *bl;

	if (register_key_type(&key_type_blacklist) < 0)
		panic("Can't allocate system blacklist key type\n");

	blacklist_keyring =
		keyring_alloc(".blacklist",
			      KUIDT_INIT(0), KGIDT_INIT(0),
			      current_cred(),
			      (KEY_POS_ALL & ~KEY_POS_SETATTR) |
			      KEY_USR_VIEW | KEY_USR_READ |
			      KEY_USR_SEARCH,
			      KEY_ALLOC_NOT_IN_QUOTA |
			      KEY_FLAG_KEEP,
			      NULL, NULL);
	if (IS_ERR(blacklist_keyring))
		panic("Can't allocate system blacklist keyring\n");

	for (bl = blacklist_hashes; *bl; bl++)
		if (mark_hash_blacklisted(*bl) < 0)
			pr_err("- blacklisting failed\n");
	return 0;
}

/*
 * Must be initialised before we try and load the keys into the keyring.
 */
device_initcall(blacklist_init);
