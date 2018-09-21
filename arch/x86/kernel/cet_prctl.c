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
	unsigned long buf[3];

	if (current->thread.cet.shstk_enabled)
		features |= GNU_PROPERTY_X86_FEATURE_1_SHSTK;

	shstk_base = current->thread.cet.shstk_base;
	shstk_size = current->thread.cet.shstk_size;

	buf[0] = (unsigned long)features;
	buf[1] = shstk_base;
	buf[2] = shstk_size;
	return copy_to_user((unsigned long __user *)arg2, buf,
			    sizeof(buf));
}

static int handle_alloc_shstk(unsigned long arg2)
{
	int err = 0;
	unsigned long shstk_size = 0;

	if (get_user(shstk_size, (unsigned long __user *)arg2))
		return -EFAULT;

	err = cet_alloc_shstk(&shstk_size);
	if (err)
		return err;

	if (put_user(shstk_size, (unsigned long __user *)arg2))
		return -EFAULT;

	return 0;
}

int prctl_cet(int option, unsigned long arg2)
{
	if (!cpu_feature_enabled(X86_FEATURE_SHSTK))
		return -EINVAL;

	switch (option) {
	case ARCH_CET_STATUS:
		return handle_get_status(arg2);

	case ARCH_CET_DISABLE:
		if (current->thread.cet.locked)
			return -EPERM;
		if (arg2 & GNU_PROPERTY_X86_FEATURE_1_SHSTK)
			cet_disable_free_shstk(current);

		return 0;

	case ARCH_CET_LOCK:
		current->thread.cet.locked = 1;
		return 0;

	case ARCH_CET_ALLOC_SHSTK:
		return handle_alloc_shstk(arg2);

	default:
		return -EINVAL;
	}
}
