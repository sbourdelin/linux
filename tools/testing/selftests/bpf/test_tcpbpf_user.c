// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <linux/perf_event.h>
#include <linux/ptrace.h>
#include <linux/bpf.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include "bpf_util.h"
#include <linux/perf_event.h>

struct globals {
	__u32 event_map;
	__u32 total_retrans;
	__u32 data_segs_in;
	__u32 data_segs_out;
	__u64 bytes_received;
	__u64 bytes_acked;
};

static int bpf_find_map(const char *test, struct bpf_object *obj,
			const char *name)
{
	struct bpf_map *map;

	map = bpf_object__find_map_by_name(obj, name);
	if (!map) {
		printf("%s:FAIL:map '%s' not found\n", test, name);
		return -1;
	}
	return bpf_map__fd(map);
}

#define SYSTEM(CMD)						\
	do {							\
		if (system(CMD)) {				\
			printf("system(%s) FAILS!\n", CMD);	\
		}						\
	} while (0)

int main(int argc, char **argv)
{
	struct globals g = {0, 0, 0, 0, 0, 0};
	__u32 key = 0;
	int rv;
	int pid;
	int error = EXIT_FAILURE;
	int cg_fd, prog_fd, map_fd;
	char cmd[100], *dir;
	const char *file = "test_tcpbpf_kern.o";
	struct bpf_object *obj;
	struct stat buffer;

	dir = "/tmp/cgroupv2/foo";

	if (stat(dir, &buffer) != 0) {
		SYSTEM("mkdir -p /tmp/cgroupv2");
		SYSTEM("mount -t cgroup2 none /tmp/cgroupv2");
		SYSTEM("mkdir -p /tmp/cgroupv2/foo");
	}
	pid = (int) getpid();
	sprintf(cmd, "echo %d >> /tmp/cgroupv2/foo/cgroup.procs", pid);
	SYSTEM(cmd);

	cg_fd = open(dir, O_DIRECTORY, O_RDONLY);
	if (bpf_prog_load(file, BPF_PROG_TYPE_SOCK_OPS, &obj, &prog_fd)) {
//	if (load_bpf_file(prog)) {
		printf("FAILED: load_bpf_file failed for: %s\n", file);
//		printf("%s", bpf_log_buf);
		goto err;
	}

	rv = bpf_prog_attach(prog_fd, cg_fd, BPF_CGROUP_SOCK_OPS, 0);
	if (rv) {
		printf("FAILED: bpf_prog_attach: %d (%s)\n",
		       error, strerror(errno));
		goto err;
	}

	SYSTEM("./tcp_server.py");

	map_fd = bpf_find_map(__func__, obj, "global_map");
	if (map_fd < 0)
		goto err;

	rv = bpf_map_lookup_elem(map_fd, &key, &g);
	if (rv != 0) {
		printf("FAILED: bpf_map_lookup_elem returns %d\n", rv);
		goto err;
	}

	if (g.bytes_received != 501 || g.bytes_acked != 1002 ||
	    g.data_segs_in != 1 || g.data_segs_out != 1 ||
		g.event_map != 0x45e) {
		printf("FAILED: Wrong stats\n");
		goto err;
	}
	printf("PASSED!\n");
	error = 0;
err:
	bpf_prog_detach(cg_fd, BPF_CGROUP_SOCK_OPS);
	return error;
}
