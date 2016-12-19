/*
 * Definitions for the self-encrypting drive interface
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

#ifndef _UAPI_SED_H
#define _UAPI_SED_H

#include <linux/types.h>
#include "sed-opal.h"

enum sed_key_type {
	OPAL,
	OPAL_PW,
	OPAL_ACT_USR,
	OPAL_LR_SETUP,
	OPAL_LOCK_UNLOCK,
	OPAL_MBR_DATA,
};

struct sed_key {
	__u32 sed_type;
	union {
		struct opal_key            opal;
		struct opal_new_pw         opal_pw;
		struct opal_session_info   opal_session;
		struct opal_user_lr_setup  opal_lrs;
		struct opal_lock_unlock    opal_lk_unlk;
		struct opal_mbr_data       opal_mbr;
		/* additional command set key types */
	};
};

#define IOC_SED_SAVE		   _IOW('p', 220, struct sed_key)
#define IOC_SED_LOCK_UNLOCK	   _IOW('p', 221, struct sed_key)
#define IOC_SED_TAKE_OWNERSHIP	   _IOW('p', 222, struct sed_key)
#define IOC_SED_ACTIVATE_LSP       _IOW('p', 223, struct sed_key)
#define IOC_SED_SET_PW             _IOW('p', 224, struct sed_key)
#define IOC_SED_ACTIVATE_USR       _IOW('p', 225, struct sed_key)
#define IOC_SED_REVERT_TPR         _IOW('p', 226, struct sed_key)
#define IOC_SED_LR_SETUP           _IOW('p', 227, struct sed_key)
#define IOC_SED_ADD_USR_TO_LR      _IOW('p', 228, struct sed_key)
#define IOC_SED_ENABLE_DISABLE_MBR _IOW('p', 229, struct sed_key)
#define IOC_SED_ERASE_LR           _IOW('p', 230, struct sed_key)
#define IOC_SED_SECURE_ERASE_LR    _IOW('p', 231, struct sed_key)

static inline int is_sed_ioctl(unsigned int cmd)
{
	return (cmd >= IOC_SED_SAVE && cmd <= IOC_SED_SECURE_ERASE_LR);
}
#endif /* _UAPI_SED_H */
