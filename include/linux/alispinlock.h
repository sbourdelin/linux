#ifndef ALI_SPINLOCK_H
#define ALI_SPINLOCK_H
/*
 * Acceleration from Lock Integration
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Copyright (C) 2015 Alibaba Group.
 *
 * Authors: Ma Ling <ling.ml@alibaba-inc.com>
 *
 */
typedef struct ali_spinlock {
	void  *lock_p;
} ali_spinlock_t;

struct ali_spinlock_info {
	struct ali_spinlock_info *next;
	int flags;
	int locked;
	void (*fn)(void *);
	void *para;
};

static __always_inline int ali_spin_is_completed(struct ali_spinlock_info *ali)
{
	return (READ_ONCE(ali->locked) == 0);
}

void alispinlock(struct ali_spinlock *lock, struct ali_spinlock_info *ali);

#define ALI_LOCK_FREE 1 
#endif /* ALI_SPINLOCK_H */
