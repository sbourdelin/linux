/*
 * perf events self profiling example test case for hw breakpoints.
 *
 * Start an number of threads. In each thread setup a breakpoint with
 * a number of variables:
 * 1) number of times we loop over it
 * 2) read, write or read&write match
 * 3) exclude userspace
 * setup this breakpoint, then read and write the data a number of times.
 * Then check the output count from perf is as expected.
 *
 * Based on:
 *   http://ozlabs.org/~anton/junkcode/perf_events_example1.c
 *
 * Copyright (C) 2018 Michael Neuling, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <elf.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include "utils.h"

int max_loops;
int num_threads;
int fail = 0;
int arraytest;

#define DAWR_LENGTH_MAX ((0x3f + 1) * 8)

static inline int sys_perf_event_open(struct perf_event_attr *attr, pid_t pid,
				      int cpu, int group_fd,
				      unsigned long flags)
{
	attr->size = sizeof(*attr);
	return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static inline bool breakpoint_test(int len)
{
	struct perf_event_attr attr;
	int fd;

	/* setup counters */
	memset(&attr, 0, sizeof(attr));
	attr.disabled = 1;
	attr.type = PERF_TYPE_BREAKPOINT;
	attr.bp_type = HW_BREAKPOINT_R;
	/* bp_addr can point anywhere but needs to be aligned */
	attr.bp_addr = (__u64)(&attr) & 0xfffffffffffff800;
	attr.bp_len = len;
	fd = sys_perf_event_open(&attr, 0, -1, -1, 0);
	if (fd < 0)
		return false;
	close(fd);
	return true;
}

static inline bool perf_breakpoint_supported(void)
{
	return breakpoint_test(4);
}

static inline bool dawr_supported(void)
{
	return breakpoint_test(DAWR_LENGTH_MAX);
}

/*
 */
static void *runtestsingle(void *vptr_args)
{
	int i,j;
	struct perf_event_attr attr;
	size_t res;
	unsigned long long breaks, needed;
	int readint; /* random stacks will give diff addr here */
	int readintarraybig[2*DAWR_LENGTH_MAX/sizeof(int)];
	int *readintalign;
	volatile int *ptr;
	int break_fd;
	int loop_num = rand() % max_loops;
	int readwriteflag = (rand() % 3) + 1; /* needs to be 1-3 */
	int exclude_user = rand() % 2;
	volatile int *k;

	/* align to 0x400 boundary as required by DAWR */
	readintalign = (int *)(((unsigned long)readintarraybig + 0x7ff) & 0xfffffffffffff800); 

	ptr = &readint;
	if (arraytest)
		ptr = &readintalign[0];

	/* setup counters */
	memset(&attr, 0, sizeof(attr));
	attr.disabled = 1;
	attr.type = PERF_TYPE_BREAKPOINT;
	attr.bp_type = readwriteflag;
	attr.bp_addr = (__u64)ptr;
	attr.bp_len = sizeof(int);
	if (arraytest)
		attr.bp_len = DAWR_LENGTH_MAX;
	attr.exclude_user = exclude_user;
	break_fd = sys_perf_event_open(&attr, 0, -1, -1, 0);
	if (break_fd < 0) {
		perror("sys_perf_event_open");
		exit(1);
	}

	/* start counters */
	ioctl(break_fd, PERF_EVENT_IOC_ENABLE);

	/* Test a bunch of reads and writes */
	k = &readint;
	for (i = 0; i < loop_num; i++) {
		if (arraytest)
			k = &(readintalign[i % (DAWR_LENGTH_MAX/sizeof(int))]);

		j = *k;
		*k = j;
	}

	/* stop counters */
	ioctl(break_fd, PERF_EVENT_IOC_DISABLE);

	/* read and check counters */
	res = read(break_fd, &breaks, sizeof(unsigned long long));
	assert(res == sizeof(unsigned long long));
	/* we read and write each loop, so subtract the ones we are counting */
	needed = 0;
	if (readwriteflag & HW_BREAKPOINT_R)
		needed += loop_num;
	if (readwriteflag & HW_BREAKPOINT_W)
		needed += loop_num;
	needed = needed * (1 - exclude_user);
	if (breaks != needed) {
		printf("FAILED: 0x%lx brks:%lld needed:%lli %i %i %i\n\n",
		       (unsigned long int)ptr, breaks, needed, loop_num, readwriteflag, exclude_user);
		fail = 1;
	}
	close(break_fd);

	return NULL;
}

void runtest(void)
{
	pthread_t *threads;
	int i;

	if ((threads = malloc(num_threads * sizeof(pthread_t))) == NULL) {
		perror("pthread malloc");
	}

	for (i = 0; i < num_threads; i++){
		if (pthread_create(&threads[i], NULL, runtestsingle, NULL) != 0) {
			perror("pthreads_create");
			fail = 1;
		}
	}

	for (i = 0; i < num_threads; i++) {
		pthread_join(threads[i], NULL);
	}
}

int check_test(void)
{
	printf("threads=%i loops=%i %s test\n", num_threads, max_loops,
	       arraytest?"array":"scalar");

	return fail;
}

static int perf_hwbreak(void)
{
	srand ( time(NULL) );
	num_threads = sysconf(_SC_NPROCESSORS_ONLN) * 2;
	max_loops = 1048576;

	SKIP_IF(!perf_breakpoint_supported());

	fail = 0;
	arraytest = 0;
	runtest();
	if (check_test())
		return 1;


	if (!dawr_supported())
		return 0;
	fail = 0;
	arraytest = 1;
	runtest();
	return check_test();
}


int main(int argc, char *argv[], char **envp)
{
	return test_harness(perf_hwbreak, "perf_hwbreak");
}
