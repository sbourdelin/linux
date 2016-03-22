/*
 * Derived key type
 *
 * For details see
 * Documentation/security/keys-derived.txt
 *
 * Copyright (C) 2016
 * Written by Kirill Marinushkin (kmarinushkin@gmail.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/err.h>
#include <linux/parser.h>
#include <linux/key.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <keys/user-type.h>
#include <keys/derived-type.h>
#include <crypto/hash.h>
#include <crypto/rng.h>
#include "internal.h"

/* KERN_ERR prefix */
#define PREFIX	"derived: "

/* Limits */
#define ITER_MAX_VAL		0x000FFFFF
#define SALT_MAX_SIZE		1024
#define RAND_MAX_SIZE		1024

/* Default values */
#define ITER_DEFAULT		1
#define ALG_NAME_DEFAULT	"sha256"
#define RNG_NAME_DEFAULT	"stdrng"

/* Options */
enum {
	OPT_SHORT_SALT,
	OPT_LONG_SALT,
	OPT_SHORT_ITER,
	OPT_LONG_ITER,
	OPT_SHORT_ALG,
	OPT_LONG_ALG,
	OPT_SHORT_RNG,
	OPT_LONG_RNG,
	OPT_SHORT_KEY_F,
	OPT_LONG_KEY_F,
	OPT_SHORT_SALT_F,
	OPT_LONG_SALT_F
};

/* Options data formats */
enum derived_opt_format {
	OPT_FORMAT_ERR = -1,
	OPT_FORMAT_PLAIN,
	OPT_FORMAT_HEX,
	OPT_FORMAT_RAND
};

/* Options data index */
enum {
	OPT_IND_KEY = 0,
	OPT_IND_SALT,
	OPT_IND_NUM /* number of indexes */
};

struct derived_blob {
	u8 *data;
	size_t *lenp;
};

struct derived_f_blob {
	enum derived_opt_format format;
	struct derived_blob *b;
};

struct derived_key_payload {
	struct rcu_head rcu;	/* RCU destructor */
	char *alg_name;			/* null-terminated digest algorithm name */
	char *rng_name;			/* null-terminated random generator algorithm name */
	u64 iter;				/* number of iterations */
	unsigned int saltlen;	/* length of salt */
	unsigned char *salt;	/* salt */
	unsigned int datalen;	/* length of derived data */
	unsigned char *data;	/* derived data */
};

/* Get option data format specified by user */
static enum derived_opt_format get_opt_format(const char *arg)
{
	if (!strcmp(arg, "plain"))
		return OPT_FORMAT_PLAIN;
	if (!strcmp(arg, "hex"))
		return OPT_FORMAT_HEX;
	if (!strcmp(arg, "rand"))
		return OPT_FORMAT_RAND;
	return OPT_FORMAT_ERR;
}

/* Generate random data */
static int gen_random(const char *rnd_name, u8 *buf, unsigned int len)
{
	int ret = -EINVAL;
	struct crypto_rng *rng = NULL;

	rng = crypto_alloc_rng(rnd_name, 0, 0);
	if (IS_ERR(rng)) {
		pr_err(PREFIX "RNG alloc failed");
		return -EINVAL;
	}

	ret = crypto_rng_get_bytes(rng, buf, len);
	if (ret < 0) {
		pr_err(PREFIX "RNG get bytes failed");
		ret = -EFAULT;
	}

	if (rng)
		crypto_free_rng(rng);
	return ret;
}

/* Parse options specified by user */
static int parse_options(char **args_str,
		struct derived_key_payload *payload, struct derived_blob *ukey)
{
	int ret = -EINVAL;
	substring_t args[MAX_OPT_ARGS];
	char *p = *args_str;
	int token;
	int i;
	unsigned short templen;
	unsigned int tempu;
	u64 tempul;
	struct derived_blob usalt = {NULL};
	struct derived_f_blob v[OPT_IND_NUM] = {
			{OPT_FORMAT_PLAIN, NULL}
	};
	const match_table_t key_tokens = {
			{OPT_SHORT_SALT, "s=%s"},
			{OPT_LONG_SALT, "salt=%s"},
			{OPT_SHORT_ITER, "i=%u"},
			{OPT_LONG_ITER, "iterations=%u"},
			{OPT_SHORT_ALG, "a=%s"},
			{OPT_LONG_ALG, "algorithm=%s"},
			{OPT_SHORT_RNG, "r=%s"},
			{OPT_LONG_RNG, "rng=%s"},
			{OPT_SHORT_KEY_F, "kf=%s"},
			{OPT_LONG_KEY_F, "keyformat=%s"},
			{OPT_SHORT_SALT_F, "sf=%s"},
			{OPT_LONG_SALT_F, "saltformat=%s"}
	};

	/* set defaults */
	payload->iter = ITER_DEFAULT;
	payload->alg_name = kstrdup(ALG_NAME_DEFAULT, GFP_KERNEL);
	if (!payload->alg_name) {
		pr_err(PREFIX "default algorithm name alloc failed");
		return -ENOMEM;
	}
	payload->rng_name = kstrdup(RNG_NAME_DEFAULT, GFP_KERNEL);
	if (!payload->rng_name) {
		pr_err(PREFIX "default RNG name alloc failed");
		return -ENOMEM;
	}

	/* parse key */
	ukey->data = strsep(args_str, " \t");
	if (!ukey->data) {
		pr_err(PREFIX "input string separation failed");
		return -EINVAL;
	}
	ukey->lenp = kmalloc(sizeof(*ukey->lenp), GFP_KERNEL);
	if (!ukey->lenp) {
		pr_err(PREFIX "input key secret alloc failed");
		return -ENOMEM;
	}
	*ukey->lenp = strlen(ukey->data);

	/* prepare format blob array */
	v[OPT_IND_KEY].b = ukey;
	v[OPT_IND_SALT].b = &usalt;

	/* parse options */
	while ((p = strsep(args_str, " \t"))) {
		if (*p == '\0' || *p == ' ' || *p == '\t')
			continue;

		token = match_token(p, key_tokens, args);

		switch (token) {

		case OPT_SHORT_SALT: /* salt */
		case OPT_LONG_SALT:
			templen = args[0].to - args[0].from;
			if (templen < 0 || templen > SALT_MAX_SIZE) {
				pr_err(PREFIX "invalid salt length");
				return -EINVAL;
			}
			payload->salt = kstrndup(args[0].from, templen, GFP_KERNEL);
			if (!payload->salt) {
				pr_err(PREFIX "salt alloc failed");
				return -ENOMEM;
			}
			payload->saltlen = templen;
			usalt.data = payload->salt;
			usalt.lenp = &payload->saltlen;
			break;

		case OPT_SHORT_ITER: /* iterations */
		case OPT_LONG_ITER:
			if (kstrtou64(args[0].from, 0, &tempul)
					|| tempul == 0
					|| tempul > ITER_MAX_VAL) {
				pr_err(PREFIX "invalid iterations number");
				return -EINVAL;
			}
			payload->iter = tempul;
			break;

		case OPT_SHORT_ALG: /* alg name */
		case OPT_LONG_ALG:
			payload->alg_name = kstrdup(args[0].from, GFP_KERNEL);
			if (!payload->alg_name) {
				pr_err(PREFIX "algorithm name alloc failed");
				return -ENOMEM;
			}
			break;

		case OPT_SHORT_RNG: /* rng name */
		case OPT_LONG_RNG:
			payload->rng_name = kstrdup(args[0].from, GFP_KERNEL);
			if (!payload->rng_name) {
				pr_err(PREFIX "RNG name alloc failed");
				return -ENOMEM;
			}
			break;

		case OPT_SHORT_KEY_F: /* key format */
		case OPT_LONG_KEY_F:
			v[OPT_IND_KEY].format = get_opt_format(args[0].from);
			if (v[OPT_IND_KEY].format == OPT_FORMAT_ERR) {
				pr_err(PREFIX "invalid key format");
				return -EINVAL;
			}
			break;

		case OPT_SHORT_SALT_F: /* salt format */
		case OPT_LONG_SALT_F:
			v[OPT_IND_SALT].format = get_opt_format(args[0].from);
			if (v[OPT_IND_SALT].format == OPT_FORMAT_ERR) {
				pr_err(PREFIX "invalid salt format");
				return -EINVAL;
			}
			break;

		default:
			pr_err(PREFIX "unsupported option");
			return -EINVAL;
		}
	}

	/* modify options according to format */
	for (i = 0; i < OPT_IND_NUM; i++) {
		if (!v[i].b || !v[i].b->data)
			continue;

		switch (v[i].format) {

		case OPT_FORMAT_HEX:
			if (*v[i].b->lenp % 2) {
				pr_err(PREFIX "invalid hex string");
				return -EINVAL;
			}
			*v[i].b->lenp /= 2;
			ret = hex2bin(v[i].b->data, v[i].b->data, *v[i].b->lenp);
			if (ret) {
				pr_err(PREFIX "invalid hex string");
				return -EINVAL;
			}
			break;

		case OPT_FORMAT_RAND:
			if (kstrtouint(v[i].b->data, 0, &tempu)
					|| tempu == 0
					|| tempu > RAND_MAX_SIZE) {
				pr_err(PREFIX "invalid random size");
				return -EINVAL;
			}
			v[i].b->data = kmalloc(tempu, GFP_KERNEL);
			if (!v[i].b->data) {
				pr_err(PREFIX "random data alloc failed");
				return -ENOMEM;
			}
			*v[i].b->lenp = tempu;
			ret = gen_random(payload->rng_name, v[i].b->data, *v[i].b->lenp);
			if (ret)
				return ret;
			break;

		default:
			break;
		}
	}

	return 0;
}

/* Free and zero payload fields */
static void free_payload_content(struct derived_key_payload *payload)
{
	if (payload->alg_name)
		kzfree(payload->alg_name);
	if (payload->rng_name)
		kzfree(payload->rng_name);
	if (payload->data)
		kzfree(payload->data);
	if (payload->salt)
		kzfree(payload->salt);
}

/* Fill derived key payload with data specified by user */
static int fill_payload(struct derived_key_payload *payload,
		struct key_preparsed_payload *prep)
{
	int ret = -EINVAL;
	char *args_str = NULL;
	struct derived_blob ukey = {NULL};
	struct crypto_shash *sh = NULL;
	struct shash_desc *sdesc = NULL;
	unsigned int i;

	if (!payload || prep->datalen <= 0 || prep->datalen > 32767 || !prep->data) {
		pr_err(PREFIX "invalid data for payload");
		return -EINVAL;
	}

	args_str = kstrndup(prep->data, prep->datalen, GFP_KERNEL);
	if (!args_str) {
		pr_err(PREFIX "input arguments alloc failed");
		return -EINVAL;
	}

	ret = parse_options(&args_str, payload, &ukey);
	if (ret)
		return ret;
	if (!ukey.data || !ukey.lenp) {
		pr_err(PREFIX "invalid key input parsed");
		return -EINVAL;
	}

	/* start derivation */
	sh = crypto_alloc_shash(payload->alg_name, 0, 0);
	if (IS_ERR(sh)) {
		pr_err(PREFIX "shash alloc failed");
		ret = -EINVAL;
		goto out;
	}

	sdesc = kzalloc(sizeof(struct shash_desc) + crypto_shash_descsize(sh), GFP_KERNEL);
	if (!sdesc) {
		pr_err(PREFIX "sdesc alloc failed");
		ret = -ENOMEM;
		goto out;
	}

	sdesc->tfm = sh;
	sdesc->flags = 0;

	payload->datalen = crypto_shash_digestsize(sh);
	if (payload->data)
		kzfree(payload->data);
	payload->data = kmalloc(payload->datalen, GFP_KERNEL);
	if (!payload->data) {
		pr_err(PREFIX "payload data alloc failed");
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < payload->iter; i++) {
		ret = crypto_shash_init(sdesc);
		if (ret) {
			pr_err(PREFIX "shash init failed");
			goto out;
		}

		if (i == 0) {
			/* first iteration */
			ret = crypto_shash_update(sdesc, ukey.data, *ukey.lenp);
			if (ret) {
				pr_err(PREFIX "shash update failed");
				goto out;
			}

			ret = crypto_shash_update(sdesc, payload->salt, payload->saltlen);
			if (ret) {
				pr_err(PREFIX "shash update failed");
				goto out;
			}
		} else {
			/* next iterations */
			ret = crypto_shash_update(sdesc, payload->data, payload->datalen);
			if (ret) {
				pr_err(PREFIX "shash update failed");
				goto out;
			}
		}

		ret = crypto_shash_final(sdesc, payload->data);
		if (ret) {
			pr_err(PREFIX "shash final failed");
			goto out;
		}

	}

out:
	if (sdesc)
		kzfree(sdesc);
	if (!IS_ERR(sh))
		crypto_free_shash(sh);
	if (args_str)
		kzfree(args_str);
	return ret;
}

/* Reserve payload for derived key */
static int reserve_derived_payload(struct key *key,
		struct derived_key_payload *payload)
{
	return key_payload_reserve(key, sizeof(*payload)
			+ payload->datalen + payload->saltlen
			+ strlen(payload->alg_name) + strlen(payload->rng_name) + 2);
}

/* Derived key instantiate */
int derived_instantiate(struct key *key, struct key_preparsed_payload *prep)
{
	int ret = -EINVAL;
	struct derived_key_payload *payload = NULL;

	if (prep->datalen <= 0 || prep->datalen > 32767 || !prep->data) {
		pr_err(PREFIX "invalid input data");
		return -EINVAL;
	}

	payload = kzalloc(sizeof(*payload), GFP_KERNEL);
	if (!payload) {
		pr_err(PREFIX "payload alloc failed");
		return -ENOMEM;
	}

	/* fill payload */
	ret = fill_payload(payload, prep);
	if (!ret)
		ret = reserve_derived_payload(key, payload);

	/* assign key if succeed */
	if (!ret)
		rcu_assign_keypointer(key, payload);
	else
		kzfree(key->payload.data);

	return ret;
}
EXPORT_SYMBOL_GPL(derived_instantiate);

/* Derived key update */
int derived_update(struct key *key, struct key_preparsed_payload *prep)
{
	int ret  = -EINVAL;
	struct derived_key_payload *payload =
			(struct derived_key_payload *)key->payload.data;

	/* free current payload */
	free_payload_content(payload);
	memset(payload, 0x00, sizeof(*payload));

	ret = fill_payload(payload, prep);
	if (!ret)
		ret = reserve_derived_payload(key, payload);

	return ret;
}
EXPORT_SYMBOL_GPL(derived_update);

/* Derived key read */
long derived_read(const struct key *key, char __user *buffer, size_t buflen)
{
	long len = -1;
	struct derived_key_payload *payload = rcu_dereference_key(key);

	if (!payload) {
		pr_err(PREFIX "invalid key payload");
		return -EINVAL;
	}

	len = payload->datalen;
	if (buffer && buflen > 0) {
		/* copy to buffer */
		if (buflen < payload->datalen
				|| copy_to_user(buffer, payload->data, payload->datalen)) {
			pr_err(PREFIX "read key data failed");
			return -EFAULT;
		}
	} /* else return without copy */

	return len;
}
EXPORT_SYMBOL_GPL(derived_read);

/* Derived key revoke */
void derived_revoke(struct key *key)
{
	struct derived_key_payload *payload =
			(struct derived_key_payload *)key->payload.data;

	/* clear the quota */
	key_payload_reserve(key, 0);

	if (payload) {
		rcu_assign_keypointer(key, NULL);
		kfree_rcu(payload, rcu);
	}
}
EXPORT_SYMBOL(derived_revoke);

/* Derived key destroy */
void derived_destroy(struct key *key)
{
	struct derived_key_payload *payload =
			(struct derived_key_payload *)key->payload.data;

	if (!payload)
		return;

	free_payload_content(payload);

	kzfree(payload);
}
EXPORT_SYMBOL_GPL(derived_destroy);

struct key_type key_type_derived = {
	.name			= "derived",
	.instantiate	= derived_instantiate,
	.update			= derived_update,
	.destroy		= derived_destroy,
	.revoke			= derived_revoke,
	.describe		= user_describe,
	.read			= derived_read,
};
EXPORT_SYMBOL_GPL(key_type_derived);

static int __init init_derived(void)
{
	return register_key_type(&key_type_derived);
}

static void __exit cleanup_derived(void)
{
	unregister_key_type(&key_type_derived);
}

late_initcall(init_derived);
module_exit(cleanup_derived);

MODULE_LICENSE("GPL");
