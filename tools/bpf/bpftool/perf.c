// SPDX-License-Identifier: GPL-2.0+
// Copyright (C) 2018 Facebook
// Author: Yonghong Song <yhs@fb.com>

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ftw.h>

#include <bpf.h>

#include "main.h"

static void print_perf_json(int pid, __u32 prog_id, __u32 prog_info,
			    char *buf, __u64 probe_offset, __u64 probe_addr)
{
	jsonw_start_object(json_wtr);
	jsonw_int_field(json_wtr, "pid", pid);
	jsonw_uint_field(json_wtr, "prog_id", prog_id);
	switch (prog_info) {
	case BPF_PERF_INFO_TP_NAME:
		jsonw_string_field(json_wtr, "prog_info", "tracepoint");
		jsonw_string_field(json_wtr, "tracepoint", buf);
		break;
	case BPF_PERF_INFO_KPROBE:
		jsonw_string_field(json_wtr, "prog_info", "kprobe");
		if (buf[0] != '\0') {
			jsonw_string_field(json_wtr, "func", buf);
			jsonw_lluint_field(json_wtr, "offset", probe_offset);
		} else {
			jsonw_lluint_field(json_wtr, "addr", probe_addr);
		}
		break;
	case BPF_PERF_INFO_KRETPROBE:
		jsonw_string_field(json_wtr, "prog_info", "kretprobe");
		if (buf[0] != '\0') {
			jsonw_string_field(json_wtr, "func", buf);
			jsonw_lluint_field(json_wtr, "offset", probe_offset);
		} else {
			jsonw_lluint_field(json_wtr, "addr", probe_addr);
		}
		break;
	case BPF_PERF_INFO_UPROBE:
		jsonw_string_field(json_wtr, "prog_info", "uprobe");
		jsonw_string_field(json_wtr, "filename", buf);
		jsonw_lluint_field(json_wtr, "offset", probe_offset);
		break;
	case BPF_PERF_INFO_URETPROBE:
		jsonw_string_field(json_wtr, "prog_info", "uretprobe");
		jsonw_string_field(json_wtr, "filename", buf);
		jsonw_lluint_field(json_wtr, "offset", probe_offset);
		break;
	}
	jsonw_end_object(json_wtr);
}

static void print_perf_plain(int pid, __u32 prog_id, __u32 prog_info,
			    char *buf, __u64 probe_offset, __u64 probe_addr)
{
	printf("%d: prog_id %u ", pid, prog_id);
	switch (prog_info) {
	case BPF_PERF_INFO_TP_NAME:
		printf("tracepoint %s\n", buf);
		break;
	case BPF_PERF_INFO_KPROBE:
		if (buf[0] != '\0')
			printf("kprobe func %s offset %llu\n", buf,
			       probe_offset);
		else
			printf("kprobe addr %llu\n", probe_addr);
		break;
	case BPF_PERF_INFO_KRETPROBE:
		if (buf[0] != '\0')
			printf("kretprobe func %s offset %llu\n", buf,
			       probe_offset);
		else
			printf("kretprobe addr %llu\n", probe_addr);
		break;
	case BPF_PERF_INFO_UPROBE:
		printf("uprobe filename %s offset %llu\n", buf, probe_offset);
		break;
	case BPF_PERF_INFO_URETPROBE:
		printf("uretprobe filename %s offset %llu\n", buf,
		       probe_offset);
		break;
	}
}

static int show_proc(const char *fpath, const struct stat *sb,
		     int tflag, struct FTW *ftwbuf)
{
	__u64 probe_offset, probe_addr;
	__u32 prog_id, prog_info;
	int err, pid = 0, fd = 0;
	const char *pch;
	char buf[4096];

	/* prefix always /proc */
	pch = fpath + 5;
	if (*pch == '\0')
		return 0;

	/* pid should be all numbers */
	pch++;
	while (*pch >= '0' && *pch <= '9') {
		pid = pid * 10 + *pch - '0';
		pch++;
	}
	if (*pch == '\0')
		return 0;
	if (*pch != '/')
		return FTW_SKIP_SUBTREE;

	/* check /proc/<pid>/fd directory */
	pch++;
	if (*pch == '\0' || *pch != 'f')
		return FTW_SKIP_SUBTREE;
	pch++;
	if (*pch == '\0' || *pch != 'd')
		return FTW_SKIP_SUBTREE;
	pch++;
	if (*pch == '\0')
		return 0;
	if (*pch != '/')
		return FTW_SKIP_SUBTREE;

	/* check /proc/<pid>/fd/<fd_num> */
	pch++;
	while (*pch >= '0' && *pch <= '9') {
		fd = fd * 10 + *pch - '0';
		pch++;
	}
	if (*pch != '\0')
		return FTW_SKIP_SUBTREE;

	/* query (pid, fd) for potential perf events */
	err = bpf_trace_event_query(pid, fd, buf, sizeof(buf),
		&prog_id, &prog_info, &probe_offset, &probe_addr);
	if (err < 0)
		return 0;

	if (json_output)
		print_perf_json(pid, prog_id, prog_info, buf, probe_offset,
				probe_addr);
	else
		print_perf_plain(pid, prog_id, prog_info, buf, probe_offset,
				 probe_addr);

	return 0;
}

static int do_show(int argc, char **argv)
{
	int nopenfd = 16;
	int flags = FTW_ACTIONRETVAL | FTW_PHYS;

	if (nftw("/proc", show_proc, nopenfd, flags) == -1) {
		perror("nftw");
		return -1;
	}

	return 0;
}

static int do_help(int argc, char **argv)
{
	fprintf(stderr,
		"Usage: %s %s { show | help }\n"
		"",
		bin_name, argv[-2]);

	return 0;
}

static const struct cmd cmds[] = {
	{ "show",	do_show },
	{ "help",	do_help },
	{ 0 }
};

int do_perf(int argc, char **argv)
{
	return cmd_select(cmds, argc, argv, do_help);
}
