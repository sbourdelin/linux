/*
 * livepatch-shadow-fix1.c - Shadow variables, livepatch demo
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

/*
 * Fixes the memory leak introduced in livepatch-shadow-mod through the
 * use of a shadow variable.  This fix demonstrates the "extending" of
 * short-lived data structures by patching its allocation and release
 * functions.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/livepatch.h>
#include <linux/slab.h>

/* Shadow variable enums */
#define SV_LEAK		1

#define T1_PERIOD 1			/* allocator thread */
#define T2_PERIOD (3 * T1_PERIOD)	/* cleanup thread */

struct dummy {
	struct list_head list;
	unsigned long jiffies_expire;
};

struct dummy *livepatch_fix1_dummy_alloc(void)
{
	struct dummy *d;
	void *leak;
	void **shadow_leak;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return NULL;

	/* Dummies live long enough to see a few t2 instances */
	d->jiffies_expire = jiffies + 1000 * 4 * T2_PERIOD;

	/*
	 * Patch: save the extra memory location into a SV_LEAK shadow
	 * variable.  A patched dummy_free routine can later fetch this
	 * pointer to handle resource release.
	 */
	leak = kzalloc(sizeof(int), GFP_KERNEL);
	shadow_leak =
		klp_shadow_attach(d, SV_LEAK, &leak, sizeof(leak), GFP_KERNEL);

	pr_info("%s: dummy @ %p, expires @ %lx\n",
		__func__, d, d->jiffies_expire);

	return d;
}

void livepatch_fix1_dummy_free(struct dummy *d)
{
	void **shadow_leak;

	/*
	 * Patch: fetch the saved SV_LEAK shadow variable, detach and
	 * free it.  Note: handle cases where this shadow variable does
	 * not exist (ie, dummy structures allocated before this livepatch
	 * was loaded.)
	 */
	shadow_leak = klp_shadow_get(d, SV_LEAK);
	if (shadow_leak) {
		klp_shadow_detach(d, SV_LEAK);
		kfree(*shadow_leak);
		pr_info("%s: dummy @ %p, prevented leak @ %p\n",
			 __func__, d, *shadow_leak);
	} else {
		pr_info("%s: dummy @ %p leaked!\n", __func__, d);
	}

	kfree(d);
}

static struct klp_func funcs[] = {
	{
		.old_name = "dummy_alloc",
		.new_func = livepatch_fix1_dummy_alloc,
	},
	{
		.old_name = "dummy_free",
		.new_func = livepatch_fix1_dummy_free,
	}, { }
};

static struct klp_object objs[] = {
	{
		.name = "livepatch_shadow_mod",
		.funcs = funcs,
	}, { }
};

static struct klp_patch patch = {
	.mod = THIS_MODULE,
	.objs = objs,
};

static int livepatch_shadow_fix1_init(void)
{
	int ret;

	if (!klp_have_reliable_stack() && !patch.immediate) {
		/*
		 * WARNING: Be very careful when using 'patch.immediate' in
		 * your patches.  It's ok to use it for simple patches like
		 * this, but for more complex patches which change function
		 * semantics, locking semantics, or data structures, it may not
		 * be safe.  Use of this option will also prevent removal of
		 * the patch.
		 *
		 * See Documentation/livepatch/livepatch.txt for more details.
		 */
		patch.immediate = true;
		pr_notice("The consistency model isn't supported for your architecture.  Bypassing safety mechanisms and applying the patch immediately.\n");
	}

	ret = klp_register_patch(&patch);
	if (ret)
		return ret;
	ret = klp_enable_patch(&patch);
	if (ret) {
		WARN_ON(klp_unregister_patch(&patch));
		return ret;
	}
	return 0;
}

static void livepatch_shadow_fix1_exit(void)
{
	/* Cleanup any existing SV_LEAK shadow variables */
	klp_shadow_detach_all(SV_LEAK);

	WARN_ON(klp_unregister_patch(&patch));
}

module_init(livepatch_shadow_fix1_init);
module_exit(livepatch_shadow_fix1_exit);
MODULE_LICENSE("GPL");
MODULE_INFO(livepatch, "Y");
