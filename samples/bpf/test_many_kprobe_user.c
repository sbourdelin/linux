/* Copyright (c) 2017 Facebook
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <libelf.h>
#include <gelf.h>
#include <linux/version.h>
#include <errno.h>
#include <stdbool.h>
#include <time.h>
#include "libbpf.h"
#include "bpf_load.h"
#include "perf-sys.h"

#define MAX_KPROBES 1000

#define DEBUGFS "/sys/kernel/debug/tracing/"

int kprobes[MAX_KPROBES] = {0};
int kprobe_count;
int perf_event_fds[MAX_KPROBES];
const char license[] = "GPL";

static __u64 time_get_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int kprobe_api(char *func, void *addr, bool use_new_api)
{
	int efd;
	struct perf_event_attr attr = {};
	struct probe_desc pd;
	char buf[256];
	int err, id;

	attr.sample_type = PERF_SAMPLE_RAW;
	attr.sample_period = 1;
	attr.wakeup_events = 1;

	if (use_new_api) {
		attr.type = PERF_TYPE_PROBE;
		if (func) {
			pd.func = ptr_to_u64(func);
			pd.offset = 0;
		} else {
			pd.func = 0;
			pd.offset = ptr_to_u64(addr);
		}

		attr.probe_desc = ptr_to_u64(&pd);
	} else {
		attr.type = PERF_TYPE_TRACEPOINT;
		snprintf(buf, sizeof(buf),
			 "echo 'p:%s %s' >> /sys/kernel/debug/tracing/kprobe_events",
			 func, func);
		err = system(buf);
		if (err < 0) {
			printf("failed to create kprobe '%s' error '%s'\n",
			       func, strerror(errno));
			return -1;
		}

		strcpy(buf, DEBUGFS);
		strcat(buf, "events/kprobes/");
		strcat(buf, func);
		strcat(buf, "/id");
		efd = open(buf, O_RDONLY, 0);
		if (efd < 0) {
			printf("failed to open event %s\n", func);
			return -1;
		}

		err = read(efd, buf, sizeof(buf));
		if (err < 0 || err >= sizeof(buf)) {
			printf("read from '%s' failed '%s'\n", func,
			       strerror(errno));
			return -1;
		}

		close(efd);
		buf[err] = 0;
		id = atoi(buf);
		attr.config = id;
	}

	efd = sys_perf_event_open(&attr, -1/*pid*/, 0/*cpu*/,
				  -1/*group_fd*/, 0);

	return efd;
}

static int select_kprobes(void)
{
	int fd;
	int i;

	load_kallsyms();

	kprobe_count = 0;
	for (i = 0; i < sym_cnt; i++) {
		if (strstr(syms[i].name, "."))
			continue;
		fd = kprobe_api(syms[i].name, NULL, true);
		if (fd < 0)
			continue;
		close(fd);
		kprobes[kprobe_count] = i;
		if (++kprobe_count >= MAX_KPROBES)
			break;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int i;
	__u64 start_time;

	select_kprobes();

	/* clean all trace_kprobe */
	i = system("echo \"\" > /sys/kernel/debug/tracing/kprobe_events");

	/* test text based API */
	start_time = time_get_ns();
	for (i = 0; i < kprobe_count; i++)
		perf_event_fds[i] = kprobe_api(syms[kprobes[i]].name,
					       NULL, false);
	printf("Creating %d kprobes with text-based API takes %f seconds\n",
	       kprobe_count, (time_get_ns() - start_time) / 1000000000.0);

	start_time = time_get_ns();
	for (i = 0; i < kprobe_count; i++)
		if (perf_event_fds[i] > 0)
			close(perf_event_fds[i]);
	i = system("echo \"\" > /sys/kernel/debug/tracing/kprobe_events");
	printf("Cleaning %d kprobes with text-based API takes %f seconds\n",
	       kprobe_count, (time_get_ns() - start_time) / 1000000000.0);

	/* test PERF_TYPE_PROBE API, with function names */
	start_time = time_get_ns();
	for (i = 0; i < kprobe_count; i++)
		perf_event_fds[i] = kprobe_api(syms[kprobes[i]].name,
					       NULL, true);
	printf("Creating %d kprobes with PERF_TYPE_PROBE (function name) takes %f seconds\n",
	       kprobe_count, (time_get_ns() - start_time) / 1000000000.0);

	start_time = time_get_ns();
	for (i = 0; i < kprobe_count; i++)
		if (perf_event_fds[i] > 0)
			close(perf_event_fds[i]);
	printf("Cleaning %d kprobes with PERF_TYPE_PROBE (function name) takes %f seconds\n",
	       kprobe_count, (time_get_ns() - start_time) / 1000000000.0);

	/* test PERF_TYPE_PROBE API, with function address */
	start_time = time_get_ns();
	for (i = 0; i < kprobe_count; i++)
		perf_event_fds[i] = kprobe_api(
			NULL, (void *)(syms[kprobes[i]].addr), true);
	printf("Creating %d kprobes with PERF_TYPE_PROBE (function addr) takes %f seconds\n",
	       kprobe_count, (time_get_ns() - start_time) / 1000000000.0);

	start_time = time_get_ns();
	for (i = 0; i < kprobe_count; i++)
		if (perf_event_fds[i] > 0)
			close(perf_event_fds[i]);
	printf("Cleaning %d kprobes with PERF_TYPE_PROBE (function addr) takes %f seconds\n",
	       kprobe_count, (time_get_ns() - start_time) / 1000000000.0);
	return 0;
}
