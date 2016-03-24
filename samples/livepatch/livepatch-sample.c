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
 * This (dumb) live patch overrides the function that prints the
 * kernel boot cmdline when /proc/cmdline is read.
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

static struct seq_file *cmdline_seq_file = NULL;

int func_with_lots_of_args(int a, int b, int c, int d, int e, int f, int g,
			   int h, int i, int j, int k, int l);

int func_with_nested_func(int a, int b, int c);

static int livepatch_cmdline_proc_show(struct seq_file *m, void *v)
{
	int i, j;

	cmdline_seq_file = m;

	i = func_with_lots_of_args(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
	j = func_with_nested_func(8, 9, 10);

	seq_printf(m, "%s %p i = %d j = %d\n", "this has been live patched", m, i, j);

	return 0;
}

static void livepatch_seq_printf(struct seq_file *m, const char *f, ...)
{
	va_list args;

	va_start(args, f);
	seq_vprintf(m, f, args);
	va_end(args);

	if (m == cmdline_seq_file) {
		printk("livepatch: patched seq_printf() called\n");
		dump_stack();
		m = NULL;
	}
}

static int livepatch_func_with_lots_of_args(int a, int b, int c, int d, int e,
					    int f, int g, int h, int i, int j,
					    int k, int l)
{
	printk("%s: %d %d %d %d %d %d %d %d %d %d %d %d\n",
	       __func__, a, b, c, d, e, f, g, h, i, j, k, l);

	return 1 + a + b + c + d + e + f + g + h + i + j + k + l;
}

struct scsi_lun {
	__u8 scsi_lun[8];
};

static void livepatch_int_to_scsilun(u64 lun, struct scsi_lun *scsilun)
{
	int i;

	memset(scsilun->scsi_lun, 0, sizeof(scsilun->scsi_lun));

	for (i = 0; i < sizeof(lun); i += 2) {
		scsilun->scsi_lun[i] = (lun >> 8) & 0xFF;
		scsilun->scsi_lun[i+1] = lun & 0xFF;
		lun = lun >> 16;
	}

	printk("livepatch: patched int_to_scsilun()\n");
}

static struct klp_func funcs[] = {
	{
		.old_name = "cmdline_proc_show",
		.new_func = livepatch_cmdline_proc_show,
	},
	{
		.old_name = "seq_printf",
		.new_func = livepatch_seq_printf,
	},
	{
		.old_name = "func_with_lots_of_args",
		.new_func = livepatch_func_with_lots_of_args,
	},
	{ }
};

static struct klp_func scsi_funcs[] = {
	{
		.old_name = "int_to_scsilun",
		.new_func = livepatch_int_to_scsilun,
	},
	{ }
};

static struct klp_object objs[] = {
	{
		/* name being NULL means vmlinux */
		.funcs = funcs,
	},
	{
#if IS_MODULE(CONFIG_SCSI)
		.name = "scsi_mod",
#endif
		.funcs = scsi_funcs,
	},
	{ }
};

static struct klp_patch patch = {
	.mod = THIS_MODULE,
	.objs = objs,
};

static int livepatch_init(void)
{
	int ret;

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

static void livepatch_exit(void)
{
	WARN_ON(klp_disable_patch(&patch));
	WARN_ON(klp_unregister_patch(&patch));
}

module_init(livepatch_init);
module_exit(livepatch_exit);
MODULE_LICENSE("GPL");
