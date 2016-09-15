#include <linux/syscalls.h>
#include <os.h>

long compat_sys_arch_prctl(int code, unsigned long arg2)
{
	return -EINVAL;
}
