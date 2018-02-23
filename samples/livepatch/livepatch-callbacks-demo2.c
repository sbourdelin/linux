/*
 * Copyright (C) 2018 Joe Lawrence <joe.lawrence@redhat.com>
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
 * livepatch-callbacks-demo2.c - (un)patching callbacks livepatch demo
 *
 *
 * Purpose
 * -------
 *
 * Demonstration of registering livepatch (un)patching callbacks and
 * their behavior in cumulative patches.
 *
 *
 * Usage
 * -----
 *
 * Step 1 - load two livepatch callback demos (default behavior)
 *
 *   insmod samples/livepatch/livepatch-callbacks-demo.ko
 *   insmod samples/livepatch/livepatch-callbacks-demo2.ko replace=0
 *   echo 0 > /sys/kernel/livepatch/livepatch_callbacks_demo2/enabled
 *   echo 0 > /sys/kernel/livepatch/livepatch_callbacks_demo/enabled
 *
 * Watch dmesg output to see pre and post (un)patch callbacks made for
 * both livepatch-callbacks-demo and livepatch-callbacks-demo2.
 *
 * Remove the modules to prepare for the next step:
 *
 *   rmmod samples/livepatch/livepatch-callbacks-demo2.ko
 *   rmmod samples/livepatch/livepatch-callbacks-demo.ko
 *
 * Step 1 - load two livepatch callback demos (cumulative behavior)
 *
 *   insmod samples/livepatch/livepatch-callbacks-demo.ko
 *   insmod samples/livepatch/livepatch-callbacks-demo2.ko replace=1
 *   echo 0 > /sys/kernel/livepatch/livepatch_callbacks_demo2/enabled
 *   echo 0 > /sys/kernel/livepatch/livepatch_callbacks_demo/enabled
 *
 * Check dmesg output again and notice that when a cumulative patch is
 * loaded, only its pre and post unpatch callbacks are executed.
 *
 * Final cleanup:
 *
 *   rmmod samples/livepatch/livepatch-callbacks-demo2.ko
 *   rmmod samples/livepatch/livepatch-callbacks-demo.ko
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/livepatch.h>

static int replace;
module_param(replace, int, 0644);
MODULE_PARM_DESC(replace, "replace (default=0)");

static const char *const module_state[] = {
	[MODULE_STATE_LIVE]	= "[MODULE_STATE_LIVE] Normal state",
	[MODULE_STATE_COMING]	= "[MODULE_STATE_COMING] Full formed, running module_init",
	[MODULE_STATE_GOING]	= "[MODULE_STATE_GOING] Going away",
	[MODULE_STATE_UNFORMED]	= "[MODULE_STATE_UNFORMED] Still setting it up",
};

static void callback_info(const char *callback, struct klp_object *obj)
{
	if (obj->mod)
		pr_info("%s: %s -> %s\n", callback, obj->mod->name,
			module_state[obj->mod->state]);
	else
		pr_info("%s: vmlinux\n", callback);
}

/* Executed on object patching (ie, patch enablement) */
static int pre_patch_callback(struct klp_object *obj)
{
	callback_info(__func__, obj);
	return 0;
}

/* Executed on object unpatching (ie, patch disablement) */
static void post_patch_callback(struct klp_object *obj)
{
	callback_info(__func__, obj);
}

/* Executed on object unpatching (ie, patch disablement) */
static void pre_unpatch_callback(struct klp_object *obj)
{
	callback_info(__func__, obj);
}

/* Executed on object unpatching (ie, patch disablement) */
static void post_unpatch_callback(struct klp_object *obj)
{
	callback_info(__func__, obj);
}

static struct klp_func no_funcs[] = {
	{ }
};

static struct klp_object objs[] = {
	{
		.name = NULL,	/* vmlinux */
		.funcs = no_funcs,
		.callbacks = {
			.pre_patch = pre_patch_callback,
			.post_patch = post_patch_callback,
			.pre_unpatch = pre_unpatch_callback,
			.post_unpatch = post_unpatch_callback,
		},
	}, { }
};

static struct klp_patch patch = {
	.mod = THIS_MODULE,
	.objs = objs,
};

static int livepatch_callbacks_demo2_init(void)
{
	int ret;

	patch.replace = replace;

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

static void livepatch_callbacks_demo2_exit(void)
{
	WARN_ON(klp_unregister_patch(&patch));
}

module_init(livepatch_callbacks_demo2_init);
module_exit(livepatch_callbacks_demo2_exit);
MODULE_LICENSE("GPL");
MODULE_INFO(livepatch, "Y");
