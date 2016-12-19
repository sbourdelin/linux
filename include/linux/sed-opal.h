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

#ifndef LINUX_OPAL_H
#define LINUX_OPAL_H

#include <linux/sed.h>
#include <linux/kernel.h>

int opal_save(struct sed_context *sedc, struct sed_key *key);
int opal_lock_unlock(struct sed_context *sedc, struct sed_key *key);
int opal_take_ownership(struct sed_context *sedc, struct sed_key *key);
int opal_activate_lsp(struct sed_context *sedc, struct sed_key *key);
int opal_set_new_pw(struct sed_context *sedc, struct sed_key *key);
int opal_activate_user(struct sed_context *sedc, struct sed_key *key);
int opal_reverttper(struct sed_context *sedc, struct sed_key *key);
int opal_setup_locking_range(struct sed_context *sedc, struct sed_key *key);
int opal_add_user_to_lr(struct sed_context *sedc, struct sed_key *key);
int opal_enable_disable_shadow_mbr(struct sed_context *sedc, struct sed_key *key);
int opal_erase_locking_range(struct sed_context *sedc, struct sed_key *key);
int opal_secure_erase_locking_range(struct sed_context *sedc, struct sed_key *key);
int opal_unlock_from_suspend(struct sed_context *sedc);
struct opal_dev *alloc_opal_dev(struct request_queue *q);
#endif /* LINUX_OPAL_H */
