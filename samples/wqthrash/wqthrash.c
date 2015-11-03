/*
 * wqthrash.c -- thrash the delayed workqueue rescheduling code
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/sched.h>

#define	ITERATIONS	(102400)
#define	DELAY		(HZ)
#define	THRASHERS	(256)

static void
dummy_work(struct work_struct *unused)
{
	schedule_timeout_uninterruptible(1);
}

static DECLARE_DELAYED_WORK(wqthrash_delayed_work, dummy_work);

static void
wqthrash_workfunc(struct work_struct *unused)
{
	int	i;

	for (i = 0; i < ITERATIONS; ++i) {
		schedule_delayed_work(&wqthrash_delayed_work, DELAY);
		cond_resched();
		mod_delayed_work(system_wq, &wqthrash_delayed_work, DELAY);
		cond_resched();
	}
}

static struct workqueue_struct	*wq;
static struct work_struct	*tw;

static int
wqthrash_init(void)
{
	int			i;

	wq = alloc_workqueue("wqthrash", WQ_UNBOUND, 0);
	if (!wq)
		return -ENOMEM;

	tw = kcalloc(THRASHERS, sizeof(*tw), GFP_KERNEL);
	if (!tw) {
		destroy_workqueue(wq);
		return -ENOMEM;
	}

	for (i = 0; i < THRASHERS; ++i) {
		INIT_WORK(&tw[i], wqthrash_workfunc);
		queue_work(wq, &tw[i]);
	}
	return 0;
}

static void
wqthrash_exit(void)
{
	int			i;

	for (i = 0; i < THRASHERS; ++i)
		flush_work(&tw[i]);

	kfree(tw);
	destroy_workqueue(wq);
	cancel_delayed_work_sync(&wqthrash_delayed_work);
}

module_init(wqthrash_init);
module_exit(wqthrash_exit);
MODULE_LICENSE("GPL");
