#ifndef __LINUX_NSFS_H
#define __LINUX_NSFS_H

#include <linux/ioctl.h>

#define NSIO	0xb7

/* Returns a file descriptor that refers to an owning user namespace */
#define NS_GET_USERNS		_IO(NSIO, 0x1)
/* Returns a file descriptor that refers to a parent namespace */
#define NS_GET_PARENT		_IO(NSIO, 0x2)
/* Returns the type of namespace (CLONE_NEW* value) referred to by
   file descriptor */
#define NS_GET_NSTYPE		_IO(NSIO, 0x3)
/* Get owner UID (in the caller's user namespace) for a user namespace */
#define NS_GET_OWNER_UID	_IO(NSIO, 0x4)
/* Set a vector of ns_last_pid for a pid namespace stack */
#define NS_SET_LAST_PID_VEC	_IO(NSIO, 0x5)

struct ns_ioc_pid_vec {
	__u32	nr;
	pid_t	pid[0];
};

#endif /* __LINUX_NSFS_H */
