/*
 * Adaptive Lock Integration
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
#include <asm/processor.h>
#include <asm/cmpxchg.h>
#include <linux/aliworkqueue.h>
/*
 * Wire-latency(RC delay) dominate modern computer performance,
 * conventional serialized works cause cache line ping-pong seriously,
 * the process spend lots of time and power to complete.
 * specially on multi-core latform.
 * 
 * However if the serialized works are sent to one core and executed
 * ONLY when contention happens, that can save much time and power,
 * because all shared data are located in private cache of one core.
 * We call the mechanism as Adaptive Lock Integration.
 * (ali workqueue)
 * 
 */
void aliworkqueue(struct ali_workqueue *ali_wq, struct ali_workqueue_info *ali)
{
	struct ali_workqueue_info *next, *old;

	ali->next = NULL;
	ali->pending = 1;
	old = xchg(&ali_wq->wq, ali);

	/* If NULL we are the first one */
	if (old) {
		/*Append self into work queue */
		WRITE_ONCE(old->next, ali);

		/*Waiting until work complete */
		while((READ_ONCE(ali->pending)))
			cpu_relax_lowlatency();
		return;
	}
	old = READ_ONCE(ali_wq->wq);

	/* Handle all pending works */
repeat:	
	if(old == ali)
		goto end;

	while (!(next = READ_ONCE(ali->next)))
		cpu_relax_lowlatency();
	
	ali->fn(ali->para);
	ali->pending = 0;

	if(old != next) {
		while (!(ali = READ_ONCE(next->next)))
			cpu_relax_lowlatency();
		next->fn(next->para);
		next->pending = 0;
		goto repeat;
		
	} else
		ali = next;
end:
	ali->fn(ali->para);
	/* If we are the last one, clear workqueue and return */
	old = cmpxchg(&ali_wq->wq, old, 0);

	if(old != ali) {
		/* There are still some works to do */
		while (!(next = READ_ONCE(ali->next)))
			cpu_relax_lowlatency();
		ali->pending = 0;
		ali = next;
		goto repeat;
	}

	ali->pending = 0;
	return;
}

/* Init ali work queue */
void ali_workqueue_init(struct ali_workqueue *ali_wq)
{
	WRITE_ONCE(ali_wq->wq, NULL);
}
