// SPDX-License-Identifier: GPL-2.0

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "libbpf.h"
#include "bpf_load.h"
#include "bpf_util.h"
#include "perf-sys.h"
#include "trace_helpers.h"

#define CHECK_PERROR_RET(condition) ({			\
	int __ret = !!(condition);			\
	if (__ret) {					\
		printf("FAIL: %s:\n", __func__);	\
		perror("    ");			\
		return -1;				\
	}						\
})

#define CHECK_AND_RET(condition) ({			\
	int __ret = !!(condition);			\
	if (__ret)					\
		return -1;				\
})

static __u64 ptr_to_u64(void *ptr)
{
	return (__u64) (unsigned long) ptr;
}

#define PMU_TYPE_FILE "/sys/bus/event_source/devices/%s/type"
static int bpf_find_probe_type(const char *event_type)
{
	char buf[256];
	int fd, ret;

	ret = snprintf(buf, sizeof(buf), PMU_TYPE_FILE, event_type);
	CHECK_PERROR_RET(ret < 0 || ret >= sizeof(buf));

	fd = open(buf, O_RDONLY);
	CHECK_PERROR_RET(fd < 0);

	ret = read(fd, buf, sizeof(buf));
	close(fd);
	CHECK_PERROR_RET(ret < 0 || ret >= sizeof(buf));

	errno = 0;
	ret = (int)strtol(buf, NULL, 10);
	CHECK_PERROR_RET(errno);
	return ret;
}

#define PMU_RETPROBE_FILE "/sys/bus/event_source/devices/%s/format/retprobe"
static int bpf_get_retprobe_bit(const char *event_type)
{
	char buf[256];
	int fd, ret;

	ret = snprintf(buf, sizeof(buf), PMU_RETPROBE_FILE, event_type);
	CHECK_PERROR_RET(ret < 0 || ret >= sizeof(buf));

	fd = open(buf, O_RDONLY);
	CHECK_PERROR_RET(fd < 0);

	ret = read(fd, buf, sizeof(buf));
	close(fd);
	CHECK_PERROR_RET(ret < 0 || ret >= sizeof(buf));
	CHECK_PERROR_RET(strlen(buf) < strlen("config:"));

	errno = 0;
	ret = (int)strtol(buf + strlen("config:"), NULL, 10);
	CHECK_PERROR_RET(errno);
	return ret;
}

static int test_debug_fs_kprobe(int fd_idx, const char *fn_name,
				__u32 expected_prog_info)
{
	__u64 probe_offset, probe_addr;
	__u32 prog_id, prog_info;
	char buf[256];
	int err;

	err = bpf_trace_event_query(getpid(), event_fd[fd_idx], buf, 256,
		&prog_id, &prog_info, &probe_offset, &probe_addr);
	if (err < 0) {
		printf("FAIL: %s, for event_fd idx %d, fn_name %s\n",
		       __func__, fd_idx, fn_name);
		perror("    :");
		return -1;
	}
	if (strcmp(buf, fn_name) != 0 ||
	    prog_info != expected_prog_info ||
	    probe_offset != 0x0 || probe_addr != 0x0) {
		printf("FAIL: bpf_trace_event_query(event_fd[1]):\n");
		printf("buf: %s, prog_info: %u, probe_offset: 0x%llx,"
		       " probe_addr: 0x%llx\n",
		       buf, prog_info, probe_offset, probe_addr);
		return -1;
	}
	return 0;
}

static int test_nondebug_fs_kuprobe_common(const char *event_type,
	const char *name, __u64 offset, __u64 addr, bool is_return,
	char *buf, int buf_len, __u32 *prog_id, __u32 *prog_info,
	__u64 *probe_offset, __u64 *probe_addr)
{
	int is_return_bit = bpf_get_retprobe_bit(event_type);
	int type = bpf_find_probe_type(event_type);
	struct perf_event_attr attr = {};
	int fd;

	if (type < 0 || is_return_bit < 0) {
		printf("FAIL: %s incorrect type (%d) or is_return_bit (%d)\n",
			__func__, type, is_return_bit);
		return -1;
	}

	attr.sample_period = 1;
	attr.wakeup_events = 1;
	if (is_return)
		attr.config |= 1 << is_return_bit;

	if (name) {
		attr.config1 = ptr_to_u64((void *)name);
		attr.config2 = offset;
	} else {
		attr.config1 = 0;
		attr.config2 = addr;
	}
	attr.size = sizeof(attr);
	attr.type = type;

	fd = sys_perf_event_open(&attr, -1, 0, -1, 0);
	CHECK_PERROR_RET(fd < 0);

	CHECK_PERROR_RET(ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) < 0);
	CHECK_PERROR_RET(ioctl(fd, PERF_EVENT_IOC_SET_BPF, prog_fd[0]) < 0);
	CHECK_PERROR_RET(bpf_trace_event_query(getpid(), fd, buf, buf_len,
		prog_id, prog_info, probe_offset, probe_addr) < 0);

	return 0;
}

static int test_nondebug_fs_probe(const char *event_type, const char *name,
				  __u64 offset, __u64 addr, bool is_return,
				  __u32 expected_prog_info,
				  __u32 expected_ret_prog_info,
				  char *buf, int buf_len)
{
	__u64 probe_offset, probe_addr;
	__u32 prog_id, prog_info;
	int err;

	err = test_nondebug_fs_kuprobe_common(event_type, name,
		offset, addr, is_return,
		buf, buf_len, &prog_id, &prog_info, &probe_offset,
		&probe_addr);
	if (err < 0) {
		printf("FAIL: %s, "
		       "for name %s, offset 0x%llx, addr 0x%llx, is_return %d\n",
		       __func__, name ? name : "", offset, addr, is_return);
		perror("    :");
		return -1;
	}
	if ((is_return && prog_info != expected_ret_prog_info) ||
	    (!is_return && prog_info != expected_prog_info)) {
		printf("FAIL: %s, incorrect prog_info %u\n",
		       __func__, prog_info);
		return -1;
	}
	if (name) {
		if (strcmp(name, buf) != 0) {
			printf("FAIL: %s, incorrect buf %s\n", __func__, buf);
			return -1;
		}
		if (probe_offset != offset) {
			printf("FAIL: %s, incorrect probe_offset 0x%llx\n",
			       __func__, probe_offset);
			return -1;
		}
	} else {
		if (buf && buf[0] != '\0') {
			printf("FAIL: %s, incorrect buf %p\n",
			       __func__, buf);
			return -1;
		}

		if (probe_addr != addr) {
			printf("FAIL: %s, incorrect probe_addr 0x%llx\n",
			       __func__, probe_addr);
			return -1;
		}
	}
	return 0;
}

static int test_debug_fs_uprobe(char *binary_path, long offset, bool is_return)
{
	struct perf_event_attr attr = {};
	const char *event_type = "uprobe";
	char buf[256], event_alias[256];
	__u64 probe_offset, probe_addr;
	__u32 prog_id, prog_info;
	int err, res, kfd, efd;
	ssize_t bytes;

	snprintf(buf, sizeof(buf), "/sys/kernel/debug/tracing/%s_events",
		 event_type);
	kfd = open(buf, O_WRONLY | O_APPEND, 0);
	CHECK_PERROR_RET(kfd < 0);

	res = snprintf(event_alias, sizeof(event_alias), "test_%d", getpid());
	CHECK_PERROR_RET(res < 0 || res >= sizeof(event_alias));

	res = snprintf(buf, sizeof(buf), "%c:%ss/%s %s:0x%lx",
		       is_return ? 'r' : 'p', event_type, event_alias,
		       binary_path, offset);
	CHECK_PERROR_RET(res < 0 || res >= sizeof(buf));
	CHECK_PERROR_RET(write(kfd, buf, strlen(buf)) < 0);

	close(kfd);
	kfd = -1;

	snprintf(buf, sizeof(buf), "/sys/kernel/debug/tracing/events/%ss/%s/id",
		 event_type, event_alias);
	efd = open(buf, O_RDONLY, 0);
	CHECK_PERROR_RET(efd < 0);

	bytes = read(efd, buf, sizeof(buf));
	CHECK_PERROR_RET(bytes <= 0 || bytes >= sizeof(buf));
	close(efd);
	buf[bytes] = '\0';

	attr.config = strtol(buf, NULL, 0);
	attr.type = PERF_TYPE_TRACEPOINT;
	attr.sample_period = 1;
	attr.wakeup_events = 1;
	kfd = sys_perf_event_open(&attr, -1, 0, -1, PERF_FLAG_FD_CLOEXEC);
	CHECK_PERROR_RET(kfd < 0);
	CHECK_PERROR_RET(ioctl(kfd, PERF_EVENT_IOC_SET_BPF, prog_fd[0]) < 0);
	CHECK_PERROR_RET(ioctl(kfd, PERF_EVENT_IOC_ENABLE, 0) < 0);

	err = bpf_trace_event_query(getpid(), kfd, buf, 256,
		&prog_id, &prog_info, &probe_offset, &probe_addr);
	if (err < 0) {
		printf("FAIL: %s, binary_path %s\n", __func__, binary_path);
		perror("    :");
		return -1;
	}
	if ((is_return && prog_info != BPF_PERF_INFO_URETPROBE) ||
	    (!is_return && prog_info != BPF_PERF_INFO_UPROBE)) {
		printf("FAIL: %s, incorrect prog_info %u\n", __func__,
		       prog_info);
		return -1;
	}
	if (strcmp(binary_path, buf) != 0) {
		printf("FAIL: %s, incorrect buf %s\n", __func__, buf);
		return -1;
	}
	if (probe_offset != offset) {
		printf("FAIL: %s, incorrect probe_offset 0x%llx\n", __func__,
		       probe_offset);
		return -1;
	}

	close(kfd);
	return 0;
}

int main(int argc, char **argv)
{
	struct rlimit r = {1024*1024, RLIM_INFINITY};
	extern char __executable_start;
	__u64 uprobe_file_offset;
	char filename[256], buf[256];

	snprintf(filename, sizeof(filename), "%s_kern.o", argv[0]);
	if (setrlimit(RLIMIT_MEMLOCK, &r)) {
		perror("setrlimit(RLIMIT_MEMLOCK)");
		return 1;
	}

	if (load_kallsyms()) {
		printf("failed to process /proc/kallsyms\n");
		return 1;
	}

	if (load_bpf_file(filename)) {
		printf("%s", bpf_log_buf);
		return 1;
	}

	/* test two functions in the corresponding *_kern.c file */
	CHECK_AND_RET(test_debug_fs_kprobe(0, "blk_start_request",
					   BPF_PERF_INFO_KPROBE));
	CHECK_AND_RET(test_debug_fs_kprobe(1, "blk_account_io_completion",
					   BPF_PERF_INFO_KRETPROBE));

	/* test nondebug fs kprobe */
	CHECK_AND_RET(test_nondebug_fs_probe("kprobe", "bpf_check", 0x0, 0x0,
					     false, BPF_PERF_INFO_KPROBE,
					     BPF_PERF_INFO_KRETPROBE,
					     buf, sizeof(buf)));
#ifdef __x86_64__
	/* set a kprobe on "bpf_check + 0x5", which is x64 specific */
	CHECK_AND_RET(test_nondebug_fs_probe("kprobe", "bpf_check", 0x5, 0x0,
					     false, BPF_PERF_INFO_KPROBE,
					     BPF_PERF_INFO_KRETPROBE,
					     buf, sizeof(buf)));
#endif
	CHECK_AND_RET(test_nondebug_fs_probe("kprobe", "bpf_check", 0x0, 0x0,
					     true, BPF_PERF_INFO_KPROBE,
					     BPF_PERF_INFO_KRETPROBE,
					     buf, sizeof(buf)));
	CHECK_AND_RET(test_nondebug_fs_probe("kprobe", NULL, 0x0,
					     ksym_get_addr("bpf_check"), false,
					     BPF_PERF_INFO_KPROBE,
					     BPF_PERF_INFO_KRETPROBE,
					     buf, sizeof(buf)));
	CHECK_AND_RET(test_nondebug_fs_probe("kprobe", NULL, 0x0,
					     ksym_get_addr("bpf_check"), false,
					     BPF_PERF_INFO_KPROBE,
					     BPF_PERF_INFO_KRETPROBE,
					     NULL, 0));
	CHECK_AND_RET(test_nondebug_fs_probe("kprobe", NULL, 0x0,
					     ksym_get_addr("bpf_check"), true,
					     BPF_PERF_INFO_KPROBE,
					     BPF_PERF_INFO_KRETPROBE,
					     buf, sizeof(buf)));
	CHECK_AND_RET(test_nondebug_fs_probe("kprobe", NULL, 0x0,
					     ksym_get_addr("bpf_check"), true,
					     BPF_PERF_INFO_KPROBE,
					     BPF_PERF_INFO_KRETPROBE,
					     0, 0));

	/* test nondebug fs uprobe */
	/* the calculation of uprobe file offset is based on gcc 7.3.1 on x64
	 * and the default linker script, which defines __executable_start as
	 * the start of the .text section. The calculation could be different
	 * on different systems with different compilers. The right way is
	 * to parse the ELF file. We took a shortcut here.
	 */
	uprobe_file_offset = (__u64)main - (__u64)&__executable_start;
	CHECK_AND_RET(test_nondebug_fs_probe("uprobe", (char *)argv[0],
					     uprobe_file_offset, 0x0, false,
					     BPF_PERF_INFO_UPROBE,
					     BPF_PERF_INFO_URETPROBE,
					     buf, sizeof(buf)));
	CHECK_AND_RET(test_nondebug_fs_probe("uprobe", (char *)argv[0],
					     uprobe_file_offset, 0x0, true,
					     BPF_PERF_INFO_UPROBE,
					     BPF_PERF_INFO_URETPROBE,
					     buf, sizeof(buf)));

	/* test debug fs uprobe */
	CHECK_AND_RET(test_debug_fs_uprobe((char *)argv[0], uprobe_file_offset,
					   false));
	CHECK_AND_RET(test_debug_fs_uprobe((char *)argv[0], uprobe_file_offset,
					   true));

	return 0;
}
