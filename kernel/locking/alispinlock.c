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
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/alispinlock.h>
/*
 * Wire-latency(RC delay) dominate modern computer performance,
 * conventional serialized works cause cache line ping-pong seriously,
 * the process spend lots of time and power to complete.
 * specially on multi-core platform.
 * 
 * However if the serialized works are sent to one core and executed
 * when lock contention happens, that can save much time and power,
 * because all shared data are located in private cache of one core.
 * We call the mechanism as Acceleration from Lock Integration
 * (ali spinlock)
 * 
 * Usually when requests are queued, we have to wait work to submit 
 * one bye one, in order to improve the whole throughput further,
 * we introduce LOCK_FREE. So when requests are sent to lock owner,
 * requester may do other works in parallelism, then ali_spin_is_completed 
 * function could tell us whether the work is completed.
 *
 */
void alispinlock(struct ali_spinlock *lock, struct ali_spinlock_info *ali)
{
	struct ali_spinlock_info *next, *old;

	ali->next = NULL;
	ali->locked = 1;
	old = xchg(&lock->lock_p, ali);

	/* If NULL we are the first one */
	if (old) {
		WRITE_ONCE(old->next, ali);
		if(ali->flags & ALI_LOCK_FREE)
			return;
		while((READ_ONCE(ali->locked)))
			cpu_relax_lowlatency();
		return;
	}
	old = READ_ONCE(lock->lock_p);

	/* Handle all pending works */
repeat:	
	if(old == ali)
		goto end;

	while (!(next = READ_ONCE(ali->next)))
		cpu_relax();
	
	ali->fn(ali->para);
	ali->locked = 0;

	if(old != next) {
		while (!(ali = READ_ONCE(next->next)))
			cpu_relax();
		next->fn(next->para);
		next->locked = 0;
		goto repeat;
		
	} else
		ali = next;
end:
	ali->fn(ali->para);
	/* If we are the last one, clear lock and return */
	old = cmpxchg(&lock->lock_p, old, 0);

	if(old != ali) {
		/* There are still some works to do */
		while (!(next = READ_ONCE(ali->next)))
			cpu_relax();
		ali->locked = 0;
		ali = next;
		goto repeat;
	}

	ali->locked = 0;
	return;
}
