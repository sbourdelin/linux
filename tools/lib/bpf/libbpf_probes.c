// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)

/* Copyright (c) 2018 Netronome Systems, Inc. */

#include <errno.h>
#include <unistd.h>

#include <linux/filter.h>
#include <linux/kernel.h>

#include "bpf.h"
#include "libbpf.h"

static void
prog_load(enum bpf_prog_type prog_type, const struct bpf_insn *insns,
	  size_t insns_cnt, int kernel_version, char *buf, size_t buf_len,
	  __u32 ifindex)
{
	struct bpf_load_program_attr xattr = {};
	int fd;

	/* Some prog type require an expected_attach_type */
	if (prog_type == BPF_PROG_TYPE_CGROUP_SOCK_ADDR)
		xattr.expected_attach_type = BPF_CGROUP_INET4_CONNECT;

	xattr.prog_type = prog_type;
	xattr.insns = insns;
	xattr.insns_cnt = insns_cnt;
	xattr.license = "GPL";
	xattr.kern_version = kernel_version;
	xattr.prog_ifindex = ifindex;

	fd = bpf_load_program_xattr(&xattr, buf, buf_len);
	if (fd >= 0)
		close(fd);
}

bool bpf_probe_prog_type(enum bpf_prog_type prog_type, int kernel_version,
			 __u32 ifindex)
{
	struct bpf_insn insns[2] = {
		BPF_MOV64_IMM(BPF_REG_0, 0),
		BPF_EXIT_INSN()
	};

	if (ifindex && prog_type == BPF_PROG_TYPE_SCHED_CLS)
		/* nfp returns -EINVAL on exit(0) with TC offload */
		insns[0].imm = 2;

	errno = 0;
	prog_load(prog_type, insns, ARRAY_SIZE(insns), kernel_version,
		  NULL, 0, ifindex);

	return errno != EINVAL && errno != EOPNOTSUPP;
}
