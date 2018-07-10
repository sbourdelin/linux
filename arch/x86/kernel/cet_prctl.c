/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/prctl.h>
#include <linux/compat.h>
#include <asm/processor.h>
#include <asm/prctl.h>
#include <asm/elf.h>
#include <asm/elf_property.h>
#include <asm/cet.h>

/* See Documentation/x86/intel_cet.txt. */

static int handle_get_status(unsigned long arg2)
{
	unsigned int features = 0;
	unsigned long shstk_base, shstk_size;

	if (current->thread.cet.shstk_enabled)
		features |= GNU_PROPERTY_X86_FEATURE_1_SHSTK;
	if (current->thread.cet.ibt_enabled)
		features |= GNU_PROPERTY_X86_FEATURE_1_IBT;

	shstk_base = current->thread.cet.shstk_base;
	shstk_size = current->thread.cet.shstk_size;

	if (in_ia32_syscall()) {
		unsigned int buf[3];

		buf[0] = features;
		buf[1] = (unsigned int)shstk_base;
		buf[2] = (unsigned int)shstk_size;
		return copy_to_user((unsigned int __user *)arg2, buf,
				    sizeof(buf));
	} else {
		unsigned long buf[3];

		buf[0] = (unsigned long)features;
		buf[1] = shstk_base;
		buf[2] = shstk_size;
		return copy_to_user((unsigned long __user *)arg2, buf,
				    sizeof(buf));
	}
}

static int handle_alloc_shstk(unsigned long arg2)
{
	int err = 0;
	unsigned long shstk_size = 0;

	if (in_ia32_syscall()) {
		unsigned int size;

		err = get_user(size, (unsigned int __user *)arg2);
		if (!err)
			shstk_size = size;
	} else {
		err = get_user(shstk_size, (unsigned long __user *)arg2);
	}

	if (err)
		return -EFAULT;

	err = cet_alloc_shstk(&shstk_size);
	if (err)
		return -err;

	if (in_ia32_syscall()) {
		if (put_user(shstk_size, (unsigned int __user *)arg2))
			return -EFAULT;
	} else {
		if (put_user(shstk_size, (unsigned long __user *)arg2))
			return -EFAULT;
	}
	return 0;
}

static int handle_bitmap(unsigned long arg2)
{
	unsigned long addr, size;

	if (current->thread.cet.ibt_enabled) {
		if (!current->thread.cet.ibt_bitmap_addr)
			cet_setup_ibt_bitmap();
		addr = current->thread.cet.ibt_bitmap_addr;
		size = current->thread.cet.ibt_bitmap_size;
	} else {
		addr = 0;
		size = 0;
	}

	if (in_compat_syscall()) {
		if (put_user(addr, (unsigned int __user *)arg2) ||
		    put_user(size, (unsigned int __user *)arg2 + 1))
			return -EFAULT;
	} else {
		if (put_user(addr, (unsigned long __user *)arg2) ||
		    put_user(size, (unsigned long __user *)arg2 + 1))
		return -EFAULT;
	}
	return 0;
}

int prctl_cet(int option, unsigned long arg2)
{
	if (!cpu_feature_enabled(X86_FEATURE_SHSTK) &&
	    !cpu_feature_enabled(X86_FEATURE_IBT))
		return -EINVAL;

	switch (option) {
	case ARCH_CET_STATUS:
		return handle_get_status(arg2);

	case ARCH_CET_DISABLE:
		if (current->thread.cet.locked)
			return -EPERM;
		if (arg2 & GNU_PROPERTY_X86_FEATURE_1_SHSTK)
			cet_disable_free_shstk(current);
		if (arg2 & GNU_PROPERTY_X86_FEATURE_1_IBT)
			cet_disable_ibt();

		return 0;

	case ARCH_CET_LOCK:
		current->thread.cet.locked = 1;
		return 0;

	case ARCH_CET_ALLOC_SHSTK:
		return handle_alloc_shstk(arg2);

	/*
	 * Allocate legacy bitmap and return address & size to user.
	 */
	case ARCH_CET_LEGACY_BITMAP:
		return handle_bitmap(arg2);

	default:
		return -EINVAL;
	}
}
