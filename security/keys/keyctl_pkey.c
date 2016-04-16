/* Public-key operation keyctls
 *
 * Copyright (C) 2016 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

#include <linux/slab.h>
#include <linux/key.h>
#include <linux/keyctl.h>
#include <linux/parser.h>
#include <crypto/public_key.h>
#include <keys/asymmetric-type.h>
#include <keys/user-type.h>
#include <asm/uaccess.h>
#include "internal.h"

/*
 * Query information about an asymmetric key.
 */
long keyctl_pkey_query(key_serial_t id,  struct keyctl_pkey_query __user *_res)
{
	struct kernel_pkey_query res;
	struct key *key;
	key_ref_t key_ref;
	long ret;

	key_ref = lookup_user_key(id, 0, KEY_NEED_READ);
	if (IS_ERR(key_ref))
		return PTR_ERR(key_ref);

	key = key_ref_to_ptr(key_ref);

	ret = query_asymmetric_key(key, &res);
	if (ret < 0)
		goto error_key;

	ret = -EFAULT;
	if (copy_to_user(_res, &res, sizeof(res)) == 0 &&
	    clear_user(_res->__spare, sizeof(_res->__spare)) == 0)
		ret = 0;

error_key:
	key_put(key);
	return ret;
}

static void keyctl_pkey_params_free(struct kernel_pkey_params *params)
{
	kfree(params->info);
	key_put(params->key);
	key_put(params->password);
}

enum {
	Opt_err = -1,
	Opt_enc,		/* "enc=<encoding>" eg. "enc=oaep" */
	Opt_hash,		/* "hash=<digest-name>" eg. "hash=sha1" */
};

static const match_table_t param_keys = {
	{ Opt_enc,	"enc=%s" },
	{ Opt_hash,	"hash=%s" },
	{ Opt_err,	NULL }
};

/*
 * Parse the information string which consists of key=val pairs.
 */
static int keyctl_pkey_params_parse(struct kernel_pkey_params *params)
{
	unsigned long token_mask = 0;
	substring_t args[MAX_OPT_ARGS];
	char *c = params->info, *p, *q;
	int token;

	while ((p = strsep(&c, " \t"))) {
		if (*p == '\0' || *p == ' ' || *p == '\t')
			continue;
		token = match_token(p, param_keys, args);
		if (test_and_set_bit(token, &token_mask))
			return -EINVAL;
		q = args[0].from;
		if (q[0])
			return -EINVAL;

		switch (token) {
		case Opt_enc:
			params->encoding = q;
			break;

		case Opt_hash:
			params->hash_algo = q;
			break;

		default:
			return -EINVAL;
		}
	}

	return 0;
}

/*
 * Get parameters from userspace.
 */
static int keyctl_pkey_params_get(const struct keyctl_pkey_params __user *_params,
				  const char __user *_info,
				  int op,
				  struct kernel_pkey_params *params)
{
	struct keyctl_pkey_params uparams;
	struct kernel_pkey_query info;
	key_ref_t key_ref;
	void *p;
	int ret;

	memset(params, 0, sizeof(*params));
	params->encoding = "raw";

	if (copy_from_user(&uparams, _params, sizeof(uparams)) != 0)
		return -EFAULT;

	p = strndup_user(_info, PAGE_SIZE);
	if (IS_ERR(p))
		return PTR_ERR(p);
	params->info = p;

	ret = keyctl_pkey_params_parse(params);

	key_ref = lookup_user_key(uparams.key_id, 0, KEY_NEED_READ);
	if (IS_ERR(key_ref))
		return PTR_ERR(key_ref);
	params->key = key_ref_to_ptr(key_ref);

	ret = query_asymmetric_key(params->key, &info);
	if (ret < 0)
		goto error;

	if (uparams.password_id) {
		key_ref = lookup_user_key(uparams.password_id, 0,
					  KEY_NEED_READ);
		if (IS_ERR(key_ref)) {
			ret = PTR_ERR(key_ref);
			goto error;
		}
		params->password = key_ref_to_ptr(key_ref);
		ret = -EINVAL;
		if (params->password->type != &key_type_logon)
			goto error;
	}

	ret = -EINVAL;
	switch (op) {
	case KEYCTL_PKEY_ENCRYPT:
	case KEYCTL_PKEY_DECRYPT:
		if (uparams.enc_len  > info.max_enc_size ||
		    uparams.data_len > info.max_dec_size)
			goto error;
		break;
	case KEYCTL_PKEY_SIGN:
	case KEYCTL_PKEY_VERIFY:
		if (uparams.enc_len  > info.max_sig_size ||
		    uparams.data_len > info.max_data_size)
			goto error;
		break;
	default:
		BUG();
	}

	params->enc_len  = uparams.enc_len;
	params->data_len = uparams.data_len;
	return 0;

error:
	keyctl_pkey_params_free(params);
	return ret;
}

/*
 * Encrypt/decrypt/sign
 *
 * Encrypt data, decrypt data or sign data using a public key.
 *
 * _info is a string of supplementary information in key=val format.  For
 * instance, it might contain:
 *
 *	"enc=pkcs1 hash=sha256"
 *
 * where enc= specifies the encoding and hash= selects the OID to go in that
 * particular encoding if required.  If enc= isn't supplied, it's assumed that
 * the caller is supplying raw values.
 *
 * If needed, a password may be provided to unlock the private key in a logon
 * key whose serial number is in _params->password_id.
 *
 * If successful, 0 is returned.
 */
long keyctl_pkey_e_d_s(int op,
		       const struct keyctl_pkey_params __user *_params,
		       const char __user *_info,
		       const void __user *_in,
		       void __user *_out)
{
	int (*func)(struct kernel_pkey_params *, const void *, void *);
	struct kernel_pkey_params params;
	u16 in_len, out_len;
	void *in, *out;
	long ret;

	ret = keyctl_pkey_params_get(_params, _info, op, &params);
	if (ret < 0)
		return ret;

	switch (op) {
	case KEYCTL_PKEY_ENCRYPT:
		func = encrypt_blob;
		in_len = params.data_len;
		out_len = params.enc_len;
		break;
	case KEYCTL_PKEY_DECRYPT:
		func = decrypt_blob;
		in_len = params.enc_len;
		out_len = params.data_len;
		break;
	case KEYCTL_PKEY_SIGN:
		func = create_signature;
		in_len = params.data_len;
		out_len = params.enc_len;
		break;
	default:
		BUG();
	}

	in = memdup_user(_in, in_len);
	if (IS_ERR(in)) {
		ret = PTR_ERR(in);
		goto error_params;
	}

	ret = -ENOMEM;
	out = kzalloc(out_len, GFP_KERNEL);
	if (!out)
		goto error_in;

	ret = func(&params, in, out);
	if (ret < 0)
		goto error_out;

	if (copy_to_user(_out, out, out_len) != 0)
		ret = -EFAULT;

error_out:
	kfree(out);
error_in:
	kfree(in);
error_params:
	keyctl_pkey_params_free(&params);
	return ret;
}

/*
 * Verify a signature.
 *
 * Verify a public key signature using the given key, or if not given, search
 * for a matching key.
 *
 * _info is a string of supplementary information in key=val format.  For
 * instance, it might contain:
 *
 *	"enc=pkcs1 hash=sha256"
 *
 * where enc= specifies the signature blob encoding and hash= selects the OID
 * to go in that particular encoding.  If enc= isn't supplied, it's assumed
 * that the caller is supplying raw values.
 *
 * If successful, 0 is returned.
 */
long keyctl_pkey_verify(const struct keyctl_pkey_params __user *_params,
			const char __user *_info,
			const void __user *_sig,
			const void __user *_data)
{
	struct kernel_pkey_params params;
	struct public_key_signature sig;
	void *p;
	long ret;

	ret = keyctl_pkey_params_get(_params, _info, KEYCTL_PKEY_VERIFY,
				     &params);
	if (ret < 0)
		return ret;

	memset(&sig, 0, sizeof(sig));
	sig.s_size	= params.enc_len;
	sig.digest_size	= params.data_len;
	sig.encoding	= params.encoding;
	sig.hash_algo	= params.hash_algo;

	p = memdup_user(_sig, params.enc_len);
	if (IS_ERR(p)) {
		ret = PTR_ERR(p);
		goto error_params;
	}
	sig.s = p;

	p = memdup_user(_data, params.data_len);
	if (IS_ERR(p)) {
		ret = PTR_ERR(p);
		goto error_sig;
	}
	sig.digest = p;

	ret = verify_signature(params.key, &sig);

error_sig:
	public_key_signature_free(&sig);
error_params:
	keyctl_pkey_params_free(&params);
	return ret;
}
