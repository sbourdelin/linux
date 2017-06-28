/*
 * livepatch-shadow-mod.c - Shadow variables, buggy module demo
 *
 * Copyright (C) 2017 Joe Lawrence <joe.lawrence@redhat.com>
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
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/* Creates two running threads:
 *
 *   T1 - allocs a new dummy structure, sets a jiffie expiration time
 *        in the future, adds the new structure to a list
 *
 *   T2 - cleans up expired dummies on the list
 *
 * For the purposes of demonstrating a livepatch shadow variable fix,
 * the creation thread also allocates additional memory, but doesn't
 * save a pointer to it in the dummy structure.  The cleanup thread
 * then leaks the extra memory when it frees (only) the dummy
 * structure.
 *
 *
 * Usage
 * -----
 *
 * Load the buggy demonstration module:
 * $ insmod samples/livepatch/livepatch-shadow-mod.ko
 *
 * T1 allocator thread periodically wakes up and creates new dummy
 * structures allocating extra memory and set to expire some jiffie time
 * in the future:
 *
 * [  529.503048] dummy_alloc: dummy @ ffff880112921e38, expires @ 10003af60
 * [  530.527051] dummy_alloc: dummy @ ffff8801129219e8, expires @ 10003b360
 * [  531.551049] dummy_alloc: dummy @ ffff880112920458, expires @ 10003b760
 *
 * T2 cleanup thread wakes up, but doesn't find any expired dummies to
 * cleanup yet:
 *
 * [  531.551212] cleanup_thread:  jiffies = 100038880
 * [  532.575045] dummy_alloc: dummy @ ffff880112921708, expires @ 10003bb60
 * [  533.599051] dummy_alloc: dummy @ ffff880112920a18, expires @ 10003bf60
 * [  534.623052] dummy_alloc: dummy @ ffff8801129205c8, expires @ 10003c360
 * [  535.647056] dummy_alloc: dummy @ ffff880112921598, expires @ 10003c760
 * [  536.671032] dummy_alloc: dummy @ ffff880112920fd8, expires @ 10003cb60
 * [  537.567089] cleanup_thread:  jiffies = 10003a000
 * [  537.695043] dummy_alloc: dummy @ ffff880112920008, expires @ 10003cf60
 * [  538.719047] dummy_alloc: dummy @ ffff880112921878, expires @ 10003d360
 * [  539.743061] dummy_alloc: dummy @ ffff880112920cf8, expires @ 10003d760
 * [  540.767041] dummy_alloc: dummy @ ffff880116567148, expires @ 10003db60
 * [  541.791045] dummy_alloc: dummy @ ffff880116567598, expires @ 10003df60
 * [  542.815046] dummy_alloc: dummy @ ffff880116567cc8, expires @ 10003e360
 *
 * T2 cleanup thread eventually finds a few expired dummies, frees them,
 * and in the process leaks memory!
 *
 * [  543.711058] cleanup_thread:  jiffies = 10003b800
 * [  543.711366] dummy_free: dummy @ ffff880112920458, expired = 10003b760
 * [  543.711853] dummy_free: dummy @ ffff8801129219e8, expired = 10003b360
 * [  543.712294] dummy_free: dummy @ ffff880112921e38, expired = 10003af60
 * [  543.839046] dummy_alloc: dummy @ ffff8801165672b8, expires @ 10003e760
 * [  544.863054] dummy_alloc: dummy @ ffff8801165665c8, expires @ 10003eb60
 * [  545.887066] dummy_alloc: dummy @ ffff880119c42008, expires @ 10003ef60
 * [  546.911046] dummy_alloc: dummy @ ffff88011aa05b58, expires @ 10003f360
 * [  547.935048] dummy_alloc: dummy @ ffff8801150005c8, expires @ 10003f760
 * [  548.959062] dummy_alloc: dummy @ ffff880113ee9e38, expires @ 10003fb60
 *
 *
 * Fix the memory leak
 * -------------------
 *
 * One way to fix this memory leak is to attach a shadow variable
 * pointer to each dummy structure at its allocation point.  This
 * use-case demonstrates a livepatch/shadow variable fix for short-lived
 * data structures.
 *
 * In this example, existing dummy structures will unfortunately
 * continue to leak memory, however once all of the dummies that were
 * allocated before the live patch are retired, the memory leak will be
 * closed.
 *
 * Load the livepatch fix1:
 * $ insmod samples/livepatch/livepatch-shadow-fix1.ko
 *
 * T1 alloc thread wakes up and now calls a patched dummy_alloc() which
 * saves the extra memory into a shadow variable:
 *
 * [  564.147027] livepatch: enabling patch 'livepatch_shadow_fix1'
 * [  564.153922] livepatch: 'livepatch_shadow_fix1': patching...
 * [  564.319052] livepatch_shadow_fix1: livepatch_fix1_dummy_alloc: dummy @ ffff880112af88a8, expires @ 100043760
 * [  565.343060] livepatch_shadow_fix1: livepatch_fix1_dummy_alloc: dummy @ ffff880112af9708, expires @ 100043b60
 * [  565.727039] livepatch: 'livepatch_shadow_fix1': patching complete
 * [  566.367050] livepatch_shadow_fix1: livepatch_fix1_dummy_alloc: dummy @ ffff880112af8fd8, expires @ 100043f60
 * [  567.391047] livepatch_shadow_fix1: livepatch_fix1_dummy_alloc: dummy @ ffff880112af9878, expires @ 100044360
 *
 * T2 cleanup thread calls a patched dummy_free() routine which retrives
 * the shadow variable that saved the memory pointer.
 *
 * Note: Initially, memory will still be leaked as no shadow variables
 * are found for dummy structures already created:
 *
 * [  568.287070] cleanup_thread:  jiffies = 100041800
 * [  568.287492] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff880113ee8738 leaked!
 * [  568.288062] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff880113ee9598 leaked!
 * [  568.288644] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff880113ee8178 leaked!
 * [  568.289178] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff880113ee8b88 leaked!
 * [  568.289724] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff880113ee9b58 leaked!
 * [  568.290258] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff880113ee82e8 leaked!
 * [  568.415046] livepatch_shadow_fix1: livepatch_fix1_dummy_alloc: dummy @ ffff880112af8a18, expires @ 100044760
 * [  569.439050] livepatch_shadow_fix1: livepatch_fix1_dummy_alloc: dummy @ ffff880112af85c8, expires @ 100044b60
 * [  570.463049] livepatch_shadow_fix1: livepatch_fix1_dummy_alloc: dummy @ ffff880112af9148, expires @ 100044f60
 * [  571.487022] livepatch_shadow_fix1: livepatch_fix1_dummy_alloc: dummy @ ffff880112af8458, expires @ 100045360
 * [  572.511049] livepatch_shadow_fix1: livepatch_fix1_dummy_alloc: dummy @ ffff880112af99e8, expires @ 100045760
 * [  573.535036] livepatch_shadow_fix1: livepatch_fix1_dummy_alloc: dummy @ ffff88011650e008, expires @ 100045b60
 *
 * T2 cleanup thread will eventually begin to reap dummy structures that
 * do have an associated shadow variable.  Over time, this memory leak
 * will be closed completely as all dummy structures will have a
 * corresponding shadow variable tracking the extra allocated memory:
 *
 * [  580.575028] cleanup_thread:  jiffies = 100044800
 * [  580.575675] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff880112af8a18, prevented leak @ ffff880112b7c568
 * [  580.576607] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff880112af9878, prevented leak @ ffff880112b7d990
 * [  580.577545] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff880112af8fd8, prevented leak @ ffff880112b7c410
 * [  580.578483] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff880112af9708, prevented leak @ ffff880112b7dae8
 * [  580.579327] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff880112af88a8, prevented leak @ ffff880112b7c2b8
 * [  580.580324] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff8801151bf148 leaked!
 * ...
 * [  586.719022] cleanup_thread:  jiffies = 100046000
 * [  586.719648] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff88011650e738, prevented leak @ ffff880112b7c970
 * [  586.720948] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff88011650e008, prevented leak @ ffff880112b7d588
 * [  586.722215] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff880112af99e8, prevented leak @ ffff880112b7c818
 * [  586.723496] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff880112af8458, prevented leak @ ffff880112b7d6e0
 * [  586.724824] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff880112af9148, prevented leak @ ffff880112b7c6c0
 * [  586.726146] livepatch_shadow_fix1: livepatch_fix1_dummy_free: dummy @ ffff880112af85c8, prevented leak @ ffff880112b7d838
 *
 *
 * Extend functionality
 * --------------------
 *
 * Shadow variables can also be attached to in-flight dummy structures.
 * In the second livepatch, use a shadow variable counter to keep track
 * of the number of times a given dummy structure is inspected for
 * expiration.
 *
 * Load the livepatch fix2 (on top of fix1):
 * $ insmod samples/livepatch/livepatch-shadow-fix2.ko
 *
 * [  592.303611] livepatch: enabling patch 'livepatch_shadow_fix2'
 * [  592.307458] livepatch: 'livepatch_shadow_fix2': patching...
 *
 * T2 cleanup thread calls a patched dummy_check(), which keeps a shadow
 * variable counter of the number times it inspects a given dummy
 * structure.  The final count is reported by a patched dummy_free().
 *
 * Note: initially the final count will be 1, as soon-to-expire dummies
 * will only have had a shadow variable counter for a single pass
 * through the alloc - check - cleanup cycle.  Overtime, newer dummies
 * will have increased count values:
 *
 * [  592.863035] cleanup_thread:  jiffies = 100047800
 * [  592.863355] livepatch_shadow_fix2: livepatch_fix2_dummy_free: dummy @ ffff880129d89b58, prevented leak @ ffff880112b7c2b8
 * [  592.864100] livepatch_shadow_fix2: livepatch_fix2_dummy_free: dummy @ ffff880129d89b58, check counter = 1
 * ...
 * [  599.007047] cleanup_thread:  jiffies = 100049000
 * [  599.007368] livepatch_shadow_fix2: livepatch_fix2_dummy_free: dummy @ ffff880112920cf8, prevented leak @ ffff880112b7d838
 * [  599.008167] livepatch_shadow_fix2: livepatch_fix2_dummy_free: dummy @ ffff880112920cf8, check counter = 2
 * ...
 * [  605.151049] cleanup_thread:  jiffies = 10004a800
 * [  605.151464] livepatch_shadow_fix2: livepatch_fix2_dummy_free: dummy @ ffff880113ee9b58, prevented leak @ ffff88011643c818
 * [  605.152146] livepatch_shadow_fix2: livepatch_fix2_dummy_free: dummy @ ffff880113ee9b58, check counter = 2
 * [  605.152783] livepatch_shadow_fix2: livepatch_fix2_dummy_free: dummy @ ffff880112920fd8, prevented leak @ ffff880112b7c970
 * [  605.153490] livepatch_shadow_fix2: livepatch_fix2_dummy_free: dummy @ ffff880112920fd8, check counter = 3
 * ...
 *
 *
 * Cleanup
 * -------
 *
 * $ echo 0 > /sys/kernel/livepatch/livepatch_shadow_fix2/enabled
 * $ echo 0 > /sys/kernel/livepatch/livepatch_shadow_fix1/enabled
 * $ rmmod livepatch-shadow-fix2
 * $ rmmod livepatch-shadow-fix1
 * $ rmmod livepatch-shadow-mod
 */


#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/workqueue.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joe Lawrence <joe.lawrence@redhat.com>");
MODULE_DESCRIPTION("Buggy module for shadow variable demo");

#define T1_PERIOD 1			/* allocator thread */
#define T2_PERIOD (3 * T1_PERIOD)	/* cleanup thread */

LIST_HEAD(dummy_list);
DEFINE_MUTEX(dummy_list_mutex);

struct dummy {
	struct list_head list;
	unsigned long jiffies_expire;
};

noinline struct dummy *dummy_alloc(void)
{
	struct dummy *d;
	void *leak;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return NULL;

	/* Dummies live long enough to see a few t2 instances */
	d->jiffies_expire = jiffies + 1000 * 4 * T2_PERIOD;

	/* Oops, forgot to save leak! */
	leak = kzalloc(sizeof(int), GFP_KERNEL);

	pr_info("%s: dummy @ %p, expires @ %lx\n",
		__func__, d, d->jiffies_expire);

	return d;
}

noinline void dummy_free(struct dummy *d)
{
	pr_info("%s: dummy @ %p, expired = %lx\n",
		__func__, d, d->jiffies_expire);

	kfree(d);
}

noinline bool dummy_check(struct dummy *d, unsigned long jiffies)
{
	return time_after(jiffies, d->jiffies_expire);
}

/*
 * T1: alloc_thread allocates new dummy structures, allocates additional
 *     memory, aptly named "leak", but doesn't keep permanent record of it.
 */
struct workqueue_struct *alloc_wq;
struct delayed_work alloc_dwork;
static void alloc_thread(struct work_struct *work)
{
	struct dummy *d;

	d = dummy_alloc();
	if (!d)
		return;

	mutex_lock(&dummy_list_mutex);
	list_add(&d->list, &dummy_list);
	mutex_unlock(&dummy_list_mutex);

	queue_delayed_work(alloc_wq, &alloc_dwork,
		msecs_to_jiffies(1000 * T1_PERIOD));
}

/*
 * T2: cleanup_thread frees dummy structures.  Without knownledge of "leak",
 *     it leaks the additional memory that alloc_thread created.
 */
struct workqueue_struct *cleanup_wq;
struct delayed_work cleanup_dwork;
static void cleanup_thread(struct work_struct *work)
{
	struct dummy *d, *tmp;
	unsigned long j;

	j = jiffies;
	pr_info("%s: jiffies = %lx\n", __func__, j);

	mutex_lock(&dummy_list_mutex);
	list_for_each_entry_safe(d, tmp, &dummy_list, list) {

		/* Kick out and free any expired dummies */
		if (dummy_check(d, j)) {
			list_del(&d->list);
			dummy_free(d);
		}
	}
	mutex_unlock(&dummy_list_mutex);

	queue_delayed_work(cleanup_wq, &cleanup_dwork,
		msecs_to_jiffies(1000 * 2 * T2_PERIOD));
}

static int livepatch_shadow_mod_init(void)
{
	alloc_wq = create_singlethread_workqueue("klp_demo_alloc_wq");
	if (!alloc_wq)
		return -1;

	cleanup_wq = create_singlethread_workqueue("klp_demo_cleanup_wq");
	if (!cleanup_wq)
		goto exit_free_alloc;

	INIT_DELAYED_WORK(&alloc_dwork, alloc_thread);
	queue_delayed_work(alloc_wq, &alloc_dwork, 1000 * T1_PERIOD);

	INIT_DELAYED_WORK(&cleanup_dwork, cleanup_thread);
	queue_delayed_work(cleanup_wq, &cleanup_dwork,
		msecs_to_jiffies(1000 * T2_PERIOD));

	return 0;

exit_free_alloc:
	destroy_workqueue(alloc_wq);

	return -1;
}

static void livepatch_shadow_mod_exit(void)
{
	struct dummy *d, *tmp;

	/* Cleanup T1 */
	if (!cancel_delayed_work(&alloc_dwork))
		flush_workqueue(alloc_wq);
	destroy_workqueue(alloc_wq);

	/* Cleanup T2 */
	if (!cancel_delayed_work(&cleanup_dwork))
		flush_workqueue(cleanup_wq);
	destroy_workqueue(cleanup_wq);

	/* Cleanup residual dummies */
	list_for_each_entry_safe(d, tmp, &dummy_list, list) {
		list_del(&d->list);
		dummy_free(d);
	}
}

module_init(livepatch_shadow_mod_init);
module_exit(livepatch_shadow_mod_exit);
