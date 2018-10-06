// SPDX-License-Identifier: GPL-2.0
/*
 * dynamic function support
 *
 * Copyright (C) 2018 VMware inc, Steven Rostedt <rostedt@goodmis.org>
 *
 */

#include <linux/jump_function.h>
#include <linux/memory.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/sort.h>
#include <linux/err.h>

#include <asm/sections.h>
#include <asm/text-patching.h>

#include <linux/uaccess.h>

static DEFINE_MUTEX(dynfunc_mutex);


////// The below should be in arch/x86/kernel

#define CALL_SIZE 5

union call_code_union {
	unsigned char code[CALL_SIZE];
	struct {
		unsigned char e9;
		int offset;
	} __attribute__((packed));
};

int arch_assign_dynamic_function(const struct dynfunc_struct *dynfunc,
				void *func)
{
	unsigned long dfunc = (unsigned long)dynfunc->dynfunc;
	union call_code_union code;

	/* Debug to see what we are replacing (remove this) */
	probe_kernel_read(code.code, (void *)dfunc, CALL_SIZE);
#if 0
	printk("old code = %02x %02x %02x %02x %02x %pS (%lx)\n",
		code.code[0], code.code[1], code.code[2], code.code[3], code.code[4],
	       (void *)(code.offset + dfunc + CALL_SIZE),
	       code.offset + dfunc + CALL_SIZE);
#endif

	code.e9 = 0xe9;
	code.offset = (int)((unsigned long)func - (dfunc + CALL_SIZE));

#if 0
	/* Debug to see what we are updating to (remove this) */
	printk("adding func %pS to %pS (%lx) %02x %02x %02x %02x %02x\n",
	       func, (void *)dfunc, (unsigned long)dfunc,
		code.code[0], code.code[1], code.code[2], code.code[3], code.code[4]);
#endif

	mutex_lock(&text_mutex);
	text_poke_bp((void *)dfunc, code.code, CALL_SIZE, func);
	mutex_unlock(&text_mutex);

	return 0;
}

////////////// The below can be in kernel/jump_function.c

int assign_dynamic_function(const struct dynfunc_struct *dynfunc, void *func)
{
	int ret;

	mutex_lock(&dynfunc_mutex);
	ret = arch_assign_dynamic_function(dynfunc, func);
	mutex_unlock(&dynfunc_mutex);

	return ret;
}

///////// The below is for testing. Can be added in sample code.

#include <linux/debugfs.h>

/*
 * The below creates a directory in debugfs called "jump_funcs" and
 * five files within that directory:
 *
 * func0, func1, func2, func3, func4.
 *
 * Each of those files trigger a dynamic function, with the number
 * of arguments that match the number in the file name. The
 * arguments are an "int", "long", "void *" and "char *" (for the defined
 * arguments of the dynmaic functions). The values used are:
 * "1", "2", "0xdeadbeef" and "random string".
 *
 * Reading the file causes a dynamic function to be called. The
 * functions assigned to the dynamic functions just prints its own
 * function name, followed by the parameters passed to it.
 *
 * Each dynamic function has 3 functions that can be assigned to it.
 * By echoing a "0" through "2" will change the function that is
 * assigned. By doing another read of that file, it should show that
 * the dynamic function has been updated.
 */
DECLARE_DYNAMIC_FUNCTION(myfunc0, PARAMS(void), ARGS());
DECLARE_DYNAMIC_FUNCTION(myfunc1, PARAMS(int a), ARGS(a));
DECLARE_DYNAMIC_FUNCTION(myfunc2, PARAMS(int a, long b), ARGS(a, b));
DECLARE_DYNAMIC_FUNCTION(myfunc3, PARAMS(int a, long b, void *c),
			 ARGS(a, b, c));
DECLARE_DYNAMIC_FUNCTION(myfunc4, PARAMS(int a, long b, void *c, char *d),
			 ARGS(a, b, c, d));

static int myfunc0_default(void)
{
	printk("%s\n", __func__);
	return 0;
}

static int myfunc1_default(int a)
{
	printk("%s %d\n", __func__, a);
	return 0;
}

static int myfunc2_default(int a, long b)
{
	printk("%s %d %ld\n", __func__, a, b);
	return 0;
}

static int myfunc3_default(int a, long b, void *c)
{
	printk("%s %d %ld %p\n", __func__, a, b, c);
	return 0;
}

static int myfunc4_default(int a, long b, void *c, char *d)
{
	printk("%s %d %ld %p %s\n", __func__, a, b, c, d);
	return 0;
}

DEFINE_DYNAMIC_FUNCTION(myfunc0, myfunc0_default, PARAMS(void));
DEFINE_DYNAMIC_FUNCTION(myfunc1, myfunc1_default, PARAMS(int a));
DEFINE_DYNAMIC_FUNCTION(myfunc2, myfunc2_default, PARAMS(int a, long b));
DEFINE_DYNAMIC_FUNCTION(myfunc3, myfunc3_default, PARAMS(int a, long b, void *c));
DEFINE_DYNAMIC_FUNCTION(myfunc4, myfunc4_default,
			PARAMS(int a, long b, void *c, char *d));

static int myfunc0_test1(void)
{
	printk("%s\n", __func__);
	return 1;
}

static int myfunc1_test1(int a)
{
	printk("%s %d\n", __func__, a);
	return 1;
}

static int myfunc2_test1(int a, long b)
{
	printk("%s %d %ld\n", __func__, a, b);
	return 1;
}

static int myfunc3_test1(int a, long b, void *c)
{
	printk("%s %d %ld %p\n", __func__, a, b, c);
	return 1;
}

static int myfunc4_test1(int a, long b, void *c, char *d)
{
	printk("%s %d %ld %p %s\n", __func__, a, b, c, d);
	return 1;
}

static int myfunc0_test2(void)
{
	printk("%s\n", __func__);
	return 2;
}

static int myfunc1_test2(int a)
{
	printk("%s %d\n", __func__, a);
	return 2;
}

static int myfunc2_test2(int a, long b)
{
	printk("%s %d %ld\n", __func__, a, b);
	return 2;
}

static int myfunc3_test2(int a, long b, void *c)
{
	printk("%s %d %ld %px\n", __func__, a, b, c);
	return 2;
}

static int myfunc4_test2(int a, long b, void *c, char *d)
{
	printk("%s %d %ld %px %s\n", __func__, a, b, c, d);
	return 2;
}

static int open_generic(struct inode *inode, struct file *filp)
{
	filp->private_data = inode->i_private;
	return 0;
}

static ssize_t
jump_func_write(struct file *filp, const char __user *ubuf,
	       size_t cnt, loff_t *ppos)
{
	long type = (long)filp->private_data;
	unsigned long val;
	int ret;

	ret = kstrtoul_from_user(ubuf, cnt, 10, &val);
	if (ret)
		return ret;

	switch (type) {
	case 0:
		switch(val) {
		case 0:
			assign_dynamic_function_myfunc0(myfunc0_default);
			break;
		case 1:
			assign_dynamic_function_myfunc0(myfunc0_test1);
			break;
		case 2:
			assign_dynamic_function_myfunc0(myfunc0_test2);
			break;
		}
		break;
	case 1:
		switch(val) {
		case 0:
			assign_dynamic_function_myfunc1(myfunc1_default);
			break;
		case 1:
			assign_dynamic_function_myfunc1(myfunc1_test1);
			break;
		case 2:
			assign_dynamic_function_myfunc1(myfunc1_test2);
			break;
		}
		break;
	case 2:
		switch(val) {
		case 0:
			assign_dynamic_function_myfunc2(myfunc2_default);
			break;
		case 1:
			assign_dynamic_function_myfunc2(myfunc2_test1);
			break;
		case 2:
			assign_dynamic_function_myfunc2(myfunc2_test2);
			break;
		}
		break;
	case 3:
		switch(val) {
		case 0:
			assign_dynamic_function_myfunc3(myfunc3_default);
			break;
		case 1:
			assign_dynamic_function_myfunc3(myfunc3_test1);
			break;
		case 2:
			assign_dynamic_function_myfunc3(myfunc3_test2);
			break;
		}
		break;
	case 4:
		switch(val) {
		case 0:
			assign_dynamic_function_myfunc4(myfunc4_default);
			break;
		case 1:
			assign_dynamic_function_myfunc4(myfunc4_test1);
			break;
		case 2:
			assign_dynamic_function_myfunc4(myfunc4_test2);
			break;
		}
		break;
	}
	return cnt;
}

static ssize_t
jump_func_read(struct file *filp, char __user *ubuf,
	       size_t count, loff_t *ppos)
{
	long type = (long)filp->private_data;
	int a = 1;
	long b = 2;
	void *c = (void *)0xdeadbeef;
	char *d = "random string";
	long ret;

	switch (type) {
	case 0:
		ret = dynfunc_myfunc0();
		printk("ret=%ld\n", ret);
		break;
	case 1:
		ret = dynfunc_myfunc1(a);
		printk("ret=%ld\n", ret);
		break;
	case 2:
		ret = dynfunc_myfunc2(a, b);
		printk("ret=%ld\n", ret);
		break;
	case 3:
		ret = dynfunc_myfunc3(a, b, c);
		printk("ret=%ld\n", ret);
		break;
	case 4:
		ret = dynfunc_myfunc4(a, b, c, d);
		printk("ret=%ld\n", ret);
		break;
	}

	*ppos += count;
	return 0;
}

static const struct file_operations jump_func_ops = {
	.open			= open_generic,
	.write			= jump_func_write,
	.read			= jump_func_read,
};


static __init int setup_test(void)
{
	struct dentry *top = debugfs_create_dir("jump_funcs", NULL);

	if (!top)
		return -ENOMEM;

	debugfs_create_file("func0", 0666, top, (void *)0,
			    &jump_func_ops);

	debugfs_create_file("func1", 0666, top, (void *)1,
			    &jump_func_ops);

	debugfs_create_file("func2", 0666, top, (void *)2,
			    &jump_func_ops);

	debugfs_create_file("func3", 0666, top, (void *)3,
			    &jump_func_ops);

	debugfs_create_file("func4", 0666, top, (void *)4,
			    &jump_func_ops);

	return 0;
}
__initcall(setup_test);
