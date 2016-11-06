#include <linux/syscalls.h>
#include <os.h>

SYSCALL_DEFINE2(arch_prctl, int, code, unsigned long, arg2)
{
	return -EINVAL;
}
