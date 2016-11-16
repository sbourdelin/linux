#include <linux/kernel.h>
#include <linux/abi_spec.h>
#include <linux/limits.h>
#include <linux/uaccess.h>

void abispec_check_pre(const struct syscall_spec *s, ...)
{
}
EXPORT_SYMBOL_GPL(abispec_check_pre);

void abispec_check_post(const struct syscall_spec *s, long retval, ...)
{
}
EXPORT_SYMBOL_GPL(abispec_check_post);

