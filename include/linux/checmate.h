#ifndef _LINUX_CHECMATE_H_
#define _LINUX_CHECMATE_H_ 1
#include <uapi/linux/checmate.h>
#include <linux/security.h>

/* Miscellanious contexts */
struct checmate_file_open_ctx {
	struct file *file;
	const struct cred *cred;
};

struct checmate_task_create_ctx {
	unsigned long clone_flags;
};

struct checmate_task_free_ctx {
	struct task_struct *task;
};

struct checmate_socket_connect_ctx {
	struct socket *sock;
	struct sockaddr *address;
	int addrlen;
};

struct checmate_ctx {
	int hook;
	union {
		/* Miscellanious contexts */
		struct checmate_file_open_ctx			file_open_ctx;
		struct checmate_task_create_ctx			task_create_ctx;
		struct checmate_task_free_ctx			task_free_ctx;
		/* CONFIG_SECURITY_NET contexts */
		struct checmate_socket_connect_ctx		socket_connect_ctx;
	};
};

#endif
