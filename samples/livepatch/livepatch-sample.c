/*
 * livepatch-sample.c - Kernel Live Patching Sample Module
 *
 * Copyright (C) 2014 Seth Jennings <sjenning@redhat.com>
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

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/livepatch.h>

/*
 * This (dumb) live patch overrides output from the following files
 * that provide information about the system:
 *
 *	/proc/cmdline
 *	/proc/uptime
 *	/proc/consoles
 *
 * and also output from sysfs entries created by the module kobject_example:
 *
 *	/sys/kernel/kobject_example/foo
 *	/sys/kernel/kobject_example/bar
 *	/sys/kernel/kobject_example/baz
 *
 * Example:
 *
 * $ cat /proc/cmdline
 * <your cmdline>
 *
 * $ insmod livepatch-sample.ko
 * $ cat /proc/cmdline
 * this has been live patched
 *
 * $ echo 0 > /sys/kernel/livepatch/livepatch_sample/enabled
 * $ cat /proc/cmdline
 * <your cmdline>
 */

#include <linux/seq_file.h>
#include <linux/kernel_stat.h>
#include <linux/cputime.h>
#include <linux/console.h>
#include <linux/tty_driver.h>

static int livepatch_cmdline_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n", "this has been live patched");
	return 0;
}

static int livepatch_uptime_proc_show(struct seq_file *m, void *v)
{
	struct timespec uptime;
	struct timespec idle;
	u64 idletime;
	u64 nsec;
	u32 rem;
	int i;

	idletime = 0;
	for_each_possible_cpu(i)
		idletime += (__force u64) kcpustat_cpu(i).cpustat[CPUTIME_IDLE];

	get_monotonic_boottime(&uptime);
	nsec = cputime64_to_jiffies64(idletime) * TICK_NSEC;
	idle.tv_sec = div_u64_rem(nsec, NSEC_PER_SEC, &rem);
	idle.tv_nsec = rem;
	seq_printf(m, "%s\n", "this has been live patched");
	seq_printf(m, "%lu.%02lu %lu.%02lu\n",
			(unsigned long) uptime.tv_sec,
			(uptime.tv_nsec / (NSEC_PER_SEC / 100)),
			(unsigned long) idle.tv_sec,
			(idle.tv_nsec / (NSEC_PER_SEC / 100)));
	return 0;
}

static int livepatch_show_console_dev(struct seq_file *m, void *v)
{
	static const struct {
		short flag;
		char name;
	} con_flags[] = {
		{ CON_ENABLED,		'E' },
		{ CON_CONSDEV,		'C' },
		{ CON_BOOT,		'B' },
		{ CON_PRINTBUFFER,	'p' },
		{ CON_BRL,		'b' },
		{ CON_ANYTIME,		'a' },
	};
	char flags[ARRAY_SIZE(con_flags) + 1];
	struct console *con = v;
	unsigned int a;
	dev_t dev = 0;

	seq_printf(m, "%s\n", "this has been live patched");

	if (con->device) {
		const struct tty_driver *driver;
		int index;

		driver = con->device(con, &index);
		if (driver) {
			dev = MKDEV(driver->major, driver->minor_start);
			dev += index;
		}
	}

	for (a = 0; a < ARRAY_SIZE(con_flags); a++)
		flags[a] = (con->flags & con_flags[a].flag) ?
			con_flags[a].name : ' ';
	flags[a] = 0;

	seq_setwidth(m, 21 - 1);
	seq_printf(m, "%s%d", con->name, con->index);
	seq_pad(m, ' ');
	seq_printf(m, "%c%c%c (%s)", con->read ? 'R' : '-',
			con->write ? 'W' : '-', con->unblank ? 'U' : '-',
			flags);
	if (dev)
		seq_printf(m, " %4d:%d", MAJOR(dev), MINOR(dev));

	seq_puts(m, "\n");

	return 0;
}

static ssize_t livepatch_foo_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "foo: this has been livepatched\n");
}

static ssize_t livepatch_b_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s: this has been livepatched\n", attr->attr.name);
}

static struct klp_patch *patch;

static int livepatch_init(void)
{
	struct klp_object *obj;
	int ret;

	/* create empty patch structure */
	patch = klp_create_patch_or_die(THIS_MODULE);

	/* add info about changes against vmlinux */
	obj = klp_add_object_or_die(patch, NULL);
	klp_add_func_or_die(patch, obj, "cmdline_proc_show",
			    livepatch_cmdline_proc_show, 0);
	klp_add_func_or_die(patch, obj, "uptime_proc_show",
			    livepatch_uptime_proc_show, 0);
	klp_add_func_or_die(patch, obj, "show_console_dev",
			    livepatch_show_console_dev, 0);

	/* add info about changes against the module kobject_example */
	obj = klp_add_object_or_die(patch, "kobject_example");
	klp_add_func_or_die(patch, obj, "foo_show", livepatch_foo_show, 0);
	klp_add_func_or_die(patch, obj, "b_show", livepatch_b_show, 0);

	ret = klp_register_patch(patch);
	if (ret) {
		WARN_ON(klp_release_patch(patch));
		return ret;
	}

	ret = klp_enable_patch(patch);
	if (ret) {
		WARN_ON(klp_release_patch(patch));
		return ret;
	}
	return 0;
}

static void livepatch_exit(void)
{
	WARN_ON(klp_disable_patch(patch));
	WARN_ON(klp_release_patch(patch));
}

module_init(livepatch_init);
module_exit(livepatch_exit);
MODULE_LICENSE("GPL");
MODULE_INFO(livepatch, "Y");
