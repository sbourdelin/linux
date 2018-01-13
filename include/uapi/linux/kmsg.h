#ifndef _LINUX_UAPI_KMSG_H
#define _LINUX_UAPI_KMSG_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct kmsg_ioctl_get_encrypted_key {
	void __user *output_buffer;
	__u64 buffer_size;
	__u64 key_size;
};

#define KMSG_IOCTL_BASE 'g'

#define KMSG_IOCTL__GET_ENCRYPTED_KEY  _IOWR(KMSG_IOCTL_BASE, 0x30, \
	    struct kmsg_ioctl_get_encrypted_key)

#endif /* _LINUX_DN_H */
