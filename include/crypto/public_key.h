/* Asymmetric public-key algorithm definitions
 *
 * See Documentation/crypto/asymmetric-keys.txt
 *
 * Copyright (C) 2012 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

#ifndef _LINUX_PUBLIC_KEY_H
#define _LINUX_PUBLIC_KEY_H

/*
 * The use to which an asymmetric key is being put.
 */
enum key_being_used_for {
	VERIFYING_MODULE_SIGNATURE,
	VERIFYING_FIRMWARE_SIGNATURE,
	VERIFYING_KEXEC_PE_SIGNATURE,
	VERIFYING_KEY_SIGNATURE,
	VERIFYING_KEY_SELF_SIGNATURE,
	VERIFYING_UNSPECIFIED_SIGNATURE,
	NR__KEY_BEING_USED_FOR
};
extern const char *const key_being_used_for[NR__KEY_BEING_USED_FOR];

/*
 * Info indicating where a key is stored.
 * For instance if the key is stored in software, then it can be accessed
 * by SW. If the key is stored in hardware e.g. (TPM) then it can not be
 * directly accessed by SW.
 */
enum public_key_info_storage {
	KEY_INFO_STOR_HW,
	KEY_INFO_STOR_SW
};

/*
 * Information describing public_key.
 * Struct sotres information about the key i.e.
 * where key is stored, what operation it supports, etc.
 */
struct public_key_info {
	enum public_key_info_storage stored;
};

/*
 * Cryptographic data for the public-key subtype of the asymmetric key type.
 *
 * Note that this may include private part of the key as well as the public
 * part.
 */
struct public_key {
	void *key;
	u32 keylen;
	const char *id_type;
	const char *pkey_algo;
	struct public_key_info info;
};

extern void public_key_destroy(void *payload);

/*
 * Public key cryptography signature data
 */
struct public_key_signature {
	u8 *s;			/* Signature */
	u32 s_size;		/* Number of bytes in signature */
	u8 *digest;
	u8 digest_size;		/* Number of bytes in digest */
	const char *pkey_algo;
	const char *hash_algo;
};

static inline bool public_key_query_sw_key(struct public_key *pkey)
{
	return pkey->info.stored == KEY_INFO_STOR_SW;
}

static inline bool public_key_query_hw_key(struct public_key *pkey)
{
	return pkey->info.stored == KEY_INFO_STOR_HW;
}

extern struct asymmetric_key_subtype public_key_subtype;
struct key;
extern int verify_signature(const struct key *key,
			    const struct public_key_signature *sig);

struct asymmetric_key_id;
extern struct key *x509_request_asymmetric_key(struct key *keyring,
					       const struct asymmetric_key_id *id,
					       const struct asymmetric_key_id *skid,
					       bool partial);

int public_key_verify_signature(const struct public_key *pkey,
				const struct public_key_signature *sig);

#endif /* _LINUX_PUBLIC_KEY_H */
