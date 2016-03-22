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

#ifndef INCLUDE_KEYS_DERIVED_TYPE_H_
#define INCLUDE_KEYS_DERIVED_TYPE_H_

#include <linux/key.h>

extern struct key_type key_type_derived;

extern int derived_instantiate(struct key *key,
		struct key_preparsed_payload *prep);
extern int derived_update(struct key *key,
		struct key_preparsed_payload *prep);
extern long derived_read(const struct key *key,
		char __user *buffer, size_t buflen);
extern void derived_revoke(struct key *key);
extern void derived_destroy(struct key *key);

#endif /* INCLUDE_KEYS_DERIVED_TYPE_H_ */
