// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/uio.h>
#include <sys/socket.h>

#include <asm/unistd.h>

#include "include/uapi/linux/bpf.h"

#include "bpfilter_mod.h"

extern long int syscall (long int __sysno, ...);

int sys_bpf(int cmd, union bpf_attr *attr, unsigned int size)
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

	local.iov_base = (void *)src;
	local.iov_len = len;
	remote.iov_base = addr;
	remote.iov_len = len;
	return process_vm_writev(pid, &local, 1, &remote, 1, 0) != len;
}

static int handle_get_cmd(struct bpf_mbox_request *cmd)
{
	pid = cmd->pid;
	switch (cmd->cmd) {
	case BPFILTER_IPT_SO_GET_INFO:
		return bpfilter_get_info((void *)(long)cmd->addr, cmd->len);
	case BPFILTER_IPT_SO_GET_ENTRIES:
		return bpfilter_get_entries((void *)(long)cmd->addr, cmd->len);
	default:
		break;
	}
	return -ENOPROTOOPT;
}

static int handle_set_cmd(struct bpf_mbox_request *cmd)
{
	pid = cmd->pid;
	switch (cmd->cmd) {
	case BPFILTER_IPT_SO_SET_REPLACE:
		return bpfilter_set_replace((void *)(long)cmd->addr, cmd->len);
	case BPFILTER_IPT_SO_SET_ADD_COUNTERS:
		return bpfilter_set_add_counters((void *)(long)cmd->addr, cmd->len);
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
		union bpf_attr req = {};
		union bpf_attr rep = {};
		struct bpf_mbox_request *cmd;

		req.mbox_request.subsys = BPF_MBOX_SUBSYS_BPFILTER;
		sys_bpf(BPF_MBOX_REQUEST, &req, sizeof(req));
		cmd = &req.mbox_request;
		rep.mbox_reply.subsys = BPF_MBOX_SUBSYS_BPFILTER;
		rep.mbox_reply.status = cmd->kind == BPF_MBOX_KIND_SET ?
					handle_set_cmd(cmd) :
					handle_get_cmd(cmd);
		sys_bpf(BPF_MBOX_REPLY, &rep, sizeof(rep));
	}
}

int main(void)
{
	debug_fd = open("/dev/pts/1" /* /tmp/aa */, 00000002 | 00000100);
	loop();
	close(debug_fd);
	return 0;
}
