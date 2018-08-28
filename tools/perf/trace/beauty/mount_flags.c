// SPDX-License-Identifier: GPL-2.0
#include <sys/mount.h>

static size_t syscall_arg__scnprintf_mount_flags(char *bf, size_t size, struct syscall_arg *arg)
{
	size_t printed = 0;
	int flags = arg->val;

	if ((flags & MS_MGC_MSK) == MS_MGC_VAL)
		flags &= ~MS_MGC_MSK;

#define	P_FLAG(n) \
	if (flags & MS_##n) { \
		printed += scnprintf(bf + printed, size - printed, "%s%s", printed ? "|" : "", #n); \
		flags &= ~MS_##n; \
	}

	P_FLAG(RDONLY);
	P_FLAG(NOSUID);
	P_FLAG(NODEV);
	P_FLAG(NOEXEC);
	P_FLAG(SYNCHRONOUS);
	P_FLAG(REMOUNT);
	P_FLAG(MANDLOCK);
	P_FLAG(DIRSYNC);
	P_FLAG(NOATIME);
	P_FLAG(NODIRATIME);
	P_FLAG(BIND);
	P_FLAG(MOVE);
	P_FLAG(REC);
	P_FLAG(SILENT);
	P_FLAG(POSIXACL);
	P_FLAG(UNBINDABLE);
	P_FLAG(PRIVATE);
	P_FLAG(SLAVE);
	P_FLAG(SHARED);
	P_FLAG(RELATIME);
	P_FLAG(KERNMOUNT);
	P_FLAG(I_VERSION);
	P_FLAG(STRICTATIME);
	P_FLAG(LAZYTIME);
	P_FLAG(ACTIVE);
	P_FLAG(NOUSER);

#undef P_FLAG

	if (flags)
		printed += scnprintf(bf + printed, size - printed, "%s%#x", printed ? "|" : "", flags);

	return printed;
}

#define SCA_MOUNT_FLAGS syscall_arg__scnprintf_mount_flags

static size_t syscall_arg__scnprintf_umount_flags(char *bf, size_t size, struct syscall_arg *arg)
{
	size_t printed = 0;
	int flags = arg->val;

#define	P_FLAG(n) \
	if (flags & n) { \
		printed += scnprintf(bf + printed, size - printed, "%s%s", printed ? "|" : "", #n); \
		flags &= ~n; \
	}

	P_FLAG(MNT_FORCE);
	P_FLAG(MNT_DETACH);
	P_FLAG(MNT_EXPIRE);
	P_FLAG(UMOUNT_NOFOLLOW);

#undef P_FLAG

	if (flags)
		printed += scnprintf(bf + printed, size - printed, "%s%#x", printed ? "|" : "", flags);

	return printed;
}

#define SCA_UMOUNT_FLAGS syscall_arg__scnprintf_umount_flags
