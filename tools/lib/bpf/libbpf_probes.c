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

bool bpf_probe_map_type(enum bpf_map_type map_type, __u32 ifindex)
{
	int key_size, value_size, max_entries, map_flags;
	struct bpf_create_map_attr attr = {};
	int fd = -1, fd_inner;

	key_size	= sizeof(__u32);
	value_size	= sizeof(__u32);
	max_entries	= 1;
	map_flags	= 0;

	if (map_type == BPF_MAP_TYPE_LPM_TRIE) {
		key_size	= sizeof(__u64);
		value_size	= sizeof(__u64);
		map_flags	= BPF_F_NO_PREALLOC;
	} else if (map_type == BPF_MAP_TYPE_STACK_TRACE) {
		value_size	= sizeof(__u64);
	} else if (map_type == BPF_MAP_TYPE_CGROUP_STORAGE ||
		   map_type == BPF_MAP_TYPE_PERCPU_CGROUP_STORAGE) {
		key_size	= sizeof(struct bpf_cgroup_storage_key);
		value_size	= sizeof(__u64);
		max_entries	= 0;
	} else if (map_type == BPF_MAP_TYPE_QUEUE ||
		   map_type == BPF_MAP_TYPE_STACK) {
		key_size	= 0;
	}

	if (map_type == BPF_MAP_TYPE_ARRAY_OF_MAPS ||
	    map_type == BPF_MAP_TYPE_HASH_OF_MAPS) {
		/* TODO: probe for device, once libbpf has a function to create
		 * map-in-map for offload
		 */
		if (ifindex)
			return false;

		fd_inner = bpf_create_map(BPF_MAP_TYPE_HASH,
					  sizeof(__u32), sizeof(__u32), 1, 0);
		if (fd_inner < 0)
			return false;
		fd = bpf_create_map_in_map(map_type, NULL, sizeof(__u32),
					   fd_inner, 1, 0);
		close(fd_inner);
	} else {
		/* Note: No other restriction on map type probes for offload */
		attr.map_type = map_type;
		attr.key_size = key_size;
		attr.value_size = value_size;
		attr.max_entries = max_entries;
		attr.map_flags = map_flags;
		attr.map_ifindex = ifindex;

		fd = bpf_create_map_xattr(&attr);
	}
	if (fd >= 0)
		close(fd);

	return fd >= 0;
}
