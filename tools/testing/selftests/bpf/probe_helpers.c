// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
#include <unistd.h>
#include <bpf/bpf.h>

#include "cgroup_helpers.h"
#include "bpf_util.h"
#include "../../../include/linux/filter.h"

bool bpf_prog_type_supported(enum bpf_prog_type prog_type)
{
	struct bpf_load_program_attr attr;
	struct bpf_insn insns[] = {
		BPF_MOV64_IMM(BPF_REG_0, 0),
		BPF_EXIT_INSN(),
	};
	int ret;

	if (prog_type == BPF_PROG_TYPE_UNSPEC)
		return true;

	memset(&attr, 0, sizeof(attr));
	attr.prog_type = prog_type;
	attr.insns = insns;
	attr.insns_cnt = ARRAY_SIZE(insns);
	attr.license = "GPL";

	ret = bpf_load_program_xattr(&attr, NULL, 0);
	if (ret < 0)
		return false;
	close(ret);

	return true;
}

bool bpf_map_type_supported(enum bpf_map_type map_type)
{
	int key_size, value_size, max_entries;
	int fd;

	key_size = sizeof(__u32);
	value_size = sizeof(__u32);
	max_entries = 1;

	/* limited set of maps for test_verifier.c and test_maps.c */
	switch (map_type) {
	case BPF_MAP_TYPE_SOCKMAP:
	case BPF_MAP_TYPE_SOCKHASH:
	case BPF_MAP_TYPE_XSKMAP:
		break;
	case BPF_MAP_TYPE_STACK_TRACE:
		value_size = sizeof(__u64);
	case BPF_MAP_TYPE_CGROUP_STORAGE:
	case BPF_MAP_TYPE_PERCPU_CGROUP_STORAGE:
		key_size = sizeof(struct bpf_cgroup_storage_key);
		value_size = sizeof(__u64);
		max_entries = 0;
		break;
	default:
		return true;
	}

	fd = bpf_create_map(map_type, key_size, value_size, max_entries, 0);
	if (fd < 0)
		return false;
	close(fd);

	return true;
}
