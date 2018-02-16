// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <sys/uio.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include "include/uapi/linux/bpf.h"
#include <asm/unistd.h>
#include "bpfilter_mod.h"

extern long int syscall (long int __sysno, ...);

static inline int sys_bpf(enum bpf_cmd cmd, union bpf_attr *attr,
			  unsigned int size)
{
	return syscall(321, cmd, attr, size);
}

int pid;
int debug_fd;

int copy_from_user(void *dst, void *addr, int len)
{
	struct iovec local;
	struct iovec remote;

	local.iov_base = dst;
	local.iov_len = len;
	remote.iov_base = addr;
	remote.iov_len = len;
	return process_vm_readv(pid, &local, 1, &remote, 1, 0) != len;
}

int copy_to_user(void *addr, const void *src, int len)
{
	struct iovec local;
	struct iovec remote;

	local.iov_base = (void *) src;
	local.iov_len = len;
	remote.iov_base = addr;
	remote.iov_len = len;
	return process_vm_writev(pid, &local, 1, &remote, 1, 0) != len;
}

static int handle_cmd(struct bpfilter_get_cmd *cmd)
{
	pid = cmd->pid;
	switch (cmd->cmd) {
	case BPFILTER_IPT_SO_GET_INFO:
		return bpfilter_get_info((void *) (long) cmd->addr, cmd->len);
	case BPFILTER_IPT_SO_GET_ENTRIES:
		return bpfilter_get_entries((void *) (long) cmd->addr, cmd->len);
	default:
		break;
	}
	return -ENOPROTOOPT;
}

static void loop(void)
{
	bpfilter_tables_init();
	bpfilter_ipv4_init();

	while (1) {
		union bpf_attr get_cmd = {};
		union bpf_attr reply = {};
		struct bpfilter_get_cmd *cmd;

		sys_bpf(BPFILTER_GET_CMD, &get_cmd, sizeof(get_cmd));
		cmd = &get_cmd.bpfilter_get_cmd;

		dprintf(debug_fd, "pid %d cmd %d addr %llx len %d\n",
			cmd->pid, cmd->cmd, cmd->addr, cmd->len);

		reply.bpfilter_reply.status = handle_cmd(cmd);
		sys_bpf(BPFILTER_REPLY, &reply, sizeof(reply));
	}
}

int main(void)
{
	debug_fd = open("/tmp/aa", 00000002 | 00000100);
	loop();
	close(debug_fd);
	return 0;
}
