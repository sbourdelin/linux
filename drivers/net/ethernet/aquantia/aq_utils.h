/*
 * aQuantia Corporation Network Driver
 * Copyright (C) 2014-2016 aQuantia Corporation. All rights reserved
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

/* File aq_utils.h: Useful macro and structures used in all layers of driver. */

#ifndef AQ_UTILS_H
#define AQ_UTILS_H

#include "aq_common.h"

#ifndef MBIT
#define MBIT ((u64)1000000U)
#define GBIT ((u64)1000000000U)
#endif

#define AQ_IRQ_INVALID 0U
#define AQ_IRQ_LEGACY  1U
#define AQ_IRQ_MSI     2U
#define AQ_IRQ_MSIX    3U

#define AQ_DIMOF(_ARY_)  ARRAY_SIZE(_ARY_)

#define AQ_OBJ_HEADER spinlock_t lock; atomic_t flags; atomic_t busy_count

struct aq_obj_s {
	AQ_OBJ_HEADER;
};

#define AQ_OBJ_TST(_OBJ_, _FLAG_)  ((_FLAG_) & atomic_read(&(_OBJ_)->flags))

#define AQ_OBJ_SET(_OBJ_, _F_) \
{ unsigned long flags_old, flags_new; atomic_t *flags = &(_OBJ_)->flags; \
do { \
	flags_old = atomic_read(flags); \
	flags_new = flags_old | (_F_); \
} while (atomic_cmpxchg(flags, \
	flags_old, flags_new) != flags_old); }

#define AQ_OBJ_CLR(_OBJ_, _F_) \
{ unsigned long flags_old, flags_new; atomic_t *flags = &(_OBJ_)->flags; \
do { \
	flags_old = atomic_read(flags); \
	flags_new = flags_old & ~(_F_); \
} while (atomic_cmpxchg(flags, \
	flags_old, flags_new) != flags_old); }

#endif /* AQ_UTILS_H */
