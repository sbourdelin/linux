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

#include <linux/blkdev.h>
#include <linux/sed.h>
#include <linux/sed-opal.h>
#include <asm/uaccess.h>

int sed_save(struct sed_context *sed_ctx, struct sed_key *key)
{
	switch (key->sed_type) {
	case OPAL_LOCK_UNLOCK:
		return opal_save(sed_ctx, key);
	}

	return -EOPNOTSUPP;
}

int sed_lock_unlock(struct sed_context *sed_ctx, struct sed_key *key)
{
	switch (key->sed_type) {
	case OPAL_LOCK_UNLOCK:
		return opal_lock_unlock(sed_ctx, key);
	}

	return -EOPNOTSUPP;
}

int sed_take_ownership(struct sed_context *sed_ctx, struct sed_key *key)
{

	switch (key->sed_type) {
	case OPAL:
		return opal_take_ownership(sed_ctx, key);
	}

	return -EOPNOTSUPP;
}

int sed_activate_lsp(struct sed_context *sed_ctx, struct sed_key *key)
{

	switch (key->sed_type) {
	case OPAL:
		return opal_activate_lsp(sed_ctx, key);
	}

	return -EOPNOTSUPP;
}

int sed_set_pw(struct sed_context *sed_ctx, struct sed_key *key)
{

	switch (key->sed_type) {
	case OPAL_PW:
		return opal_set_new_pw(sed_ctx, key);
	}

	return -EOPNOTSUPP;
}

int sed_activate_user(struct sed_context *sed_ctx, struct sed_key *key)
{

	switch (key->sed_type) {
	case OPAL_ACT_USR:
		return opal_activate_user(sed_ctx, key);
	}

	return -EOPNOTSUPP;
}

int sed_reverttper(struct sed_context *sed_ctx, struct sed_key *key)
{

	switch (key->sed_type) {
	case OPAL:
		return opal_reverttper(sed_ctx, key);
	}

	return -EOPNOTSUPP;
}

int sed_setup_locking_range(struct sed_context *sed_ctx, struct sed_key *key)
{

	switch (key->sed_type) {
	case OPAL_LR_SETUP:
		return opal_setup_locking_range(sed_ctx, key);
	}

	return -EOPNOTSUPP;
}

int sed_adduser_to_lr(struct sed_context *sed_ctx, struct sed_key *key)
{

	switch (key->sed_type) {
	case OPAL_LOCK_UNLOCK:
		return opal_add_user_to_lr(sed_ctx, key);
	}

	return -EOPNOTSUPP;
}

int sed_do_mbr(struct sed_context *sed_ctx, struct sed_key *key)
{

	switch (key->sed_type) {
	case OPAL_MBR_DATA:
		return opal_enable_disable_shadow_mbr(sed_ctx, key);
	}

	return -EOPNOTSUPP;
}

int sed_erase_lr(struct sed_context *sed_ctx, struct sed_key *key)
{

	switch (key->sed_type) {
	case OPAL:
		return opal_erase_locking_range(sed_ctx, key);
	}

	return -EOPNOTSUPP;
}

int sed_secure_erase_lr(struct sed_context *sed_ctx, struct sed_key *key)
{
	switch (key->sed_type) {
	case OPAL_ACT_USR:
		return opal_secure_erase_locking_range(sed_ctx, key);

	}
	return -EOPNOTSUPP;
}

int fdev_sed_ioctl(struct file *filep, unsigned int cmd,
		   unsigned long arg)
{
	struct sed_key key;
	struct sed_context *sed_ctx;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	if (!filep->f_sedctx || !filep->f_sedctx->ops || !filep->f_sedctx->dev)
		return -ENODEV;

	sed_ctx = filep->f_sedctx;

	if (copy_from_user(&key, (void __user *)arg, sizeof(key)))
		return -EFAULT;

	switch (cmd) {
	case IOC_SED_SAVE:
		return sed_save(sed_ctx, &key);
	case IOC_SED_LOCK_UNLOCK:
		return sed_lock_unlock(sed_ctx, &key);
	case IOC_SED_TAKE_OWNERSHIP:
		return sed_take_ownership(sed_ctx, &key);
	case IOC_SED_ACTIVATE_LSP:
		return sed_activate_lsp(sed_ctx, &key);
	case IOC_SED_SET_PW:
		return sed_set_pw(sed_ctx, &key);
	case IOC_SED_ACTIVATE_USR:
		return sed_activate_user(sed_ctx, &key);
	case IOC_SED_REVERT_TPR:
		return sed_reverttper(sed_ctx, &key);
	case IOC_SED_LR_SETUP:
		return sed_setup_locking_range(sed_ctx, &key);
	case IOC_SED_ADD_USR_TO_LR:
		return sed_adduser_to_lr(sed_ctx, &key);
	case IOC_SED_ENABLE_DISABLE_MBR:
		return sed_do_mbr(sed_ctx, &key);
	case IOC_SED_ERASE_LR:
		return sed_erase_lr(sed_ctx, &key);
	case IOC_SED_SECURE_ERASE_LR:
		return sed_secure_erase_lr(sed_ctx, &key);
	}
	return -ENOTTY;
}
EXPORT_SYMBOL_GPL(fdev_sed_ioctl);
