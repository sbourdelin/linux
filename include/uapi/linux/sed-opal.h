/*
 * Copyright © 2016 Intel Corporation
 *
 * Authors:
 *    Rafael Antognolli <rafael.antognolli@intel.com>
 *    Scott  Bauer      <scott.bauer@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef _UAPI_OPAL_H
#define _UAPI_OPAL_H

#include <linux/types.h>

#define OPAL_KEY_MAX 256

enum opal_mbr {
	OPAL_MBR_ENABLE,
	OPAL_MBR_DISABLE,
};

enum opal_user {
	OPAL_ADMIN1,
	OPAL_USER1,
	OPAL_USER2,
	OPAL_USER3,
	OPAL_USER4,
	OPAL_USER5,
	OPAL_USER6,
	OPAL_USER7,
	OPAL_USER8,
	OPAL_USER9,
};

enum opal_lock_state {
	OPAL_RO = 0x01, /* 0001 */
	OPAL_RW = 0x02, /* 0010 */
	OPAL_LK = 0x04, /* 0100 */
};

struct opal_key {
	__u8	lr;
	__u8	key_len;
	__u8	key[OPAL_KEY_MAX];
};

struct opal_session_info {
	bool SUM;
	struct opal_key opal_key;
	enum opal_user who;
};

struct opal_user_lr_setup {
	struct opal_session_info session;
	size_t range_start;
	size_t range_length;
	int    RLE; /* Read Lock enabled */
	int    WLE; /* Write Lock Enabled */
};

struct opal_lock_unlock {
	struct opal_session_info session;
	enum opal_lock_state l_state;
};

struct opal_new_pw {
	struct opal_session_info session;

	/* When we're not operating in SUM, and we first set
	 * passwords we need to set them via ADMIN authority.
	 * After passwords are changed, we can set them via,
	 * User authorities.
	 * Because of this restriction we need to know about
	 * Two different users. One in 'who' which we will use
	 * to start the session and user_for_pw as the user we're
	 * chaning the pw for.
	 */
	struct opal_session_info new_user_pw;
};

struct opal_mbr_data {
	u8 enable_disable;
	struct opal_key key;
};

#endif /* _UAPI_SED_H */
