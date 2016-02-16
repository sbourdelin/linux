/*
 * BHRB filter test (HW & SW)
 *
 * Copyright (C) 2015 Anshuman Khandual, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <poll.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include "bhrb_filters.h"
#include "utils.h"
#include "../event.h"
#include "../lib.h"

/* Memory barriers */
#define	smp_mb()	{ asm volatile ("sync" : : : "memory"); }

/* Fetched address counts */
#define ALL_MAX		32
#define CALL_MAX	12
#define RET_MAX		10
#define COND_MAX	8
#define IND_MAX		4

/* Test tunables */
#define LOOP_COUNT	10
#define SAMPLE_PERIOD	10000

static int branch_test_set[] = {
		PERF_SAMPLE_BRANCH_ANY_CALL,		/* Single filters */
		PERF_SAMPLE_BRANCH_ANY_RETURN,
		PERF_SAMPLE_BRANCH_COND,
		PERF_SAMPLE_BRANCH_IND_CALL,
		PERF_SAMPLE_BRANCH_ANY,

		PERF_SAMPLE_BRANCH_ANY_CALL |		/* Double filters */
		PERF_SAMPLE_BRANCH_ANY_RETURN,
		PERF_SAMPLE_BRANCH_ANY_CALL |
		PERF_SAMPLE_BRANCH_COND,
		PERF_SAMPLE_BRANCH_ANY_CALL |
		PERF_SAMPLE_BRANCH_IND_CALL,
		PERF_SAMPLE_BRANCH_ANY_CALL |
		PERF_SAMPLE_BRANCH_ANY,

		PERF_SAMPLE_BRANCH_ANY_RETURN |
		PERF_SAMPLE_BRANCH_COND,
		PERF_SAMPLE_BRANCH_ANY_RETURN |
		PERF_SAMPLE_BRANCH_IND_CALL,
		PERF_SAMPLE_BRANCH_ANY_RETURN |
		PERF_SAMPLE_BRANCH_ANY,

		PERF_SAMPLE_BRANCH_COND |
		PERF_SAMPLE_BRANCH_IND_CALL,
		PERF_SAMPLE_BRANCH_COND |
		PERF_SAMPLE_BRANCH_ANY,

		PERF_SAMPLE_BRANCH_IND_CALL |
		PERF_SAMPLE_BRANCH_ANY,

		PERF_SAMPLE_BRANCH_ANY_CALL |		/* Triple filters */
		PERF_SAMPLE_BRANCH_ANY_RETURN |
		PERF_SAMPLE_BRANCH_COND,

		PERF_SAMPLE_BRANCH_ANY_CALL |
		PERF_SAMPLE_BRANCH_ANY_RETURN |
		PERF_SAMPLE_BRANCH_IND_CALL,

		PERF_SAMPLE_BRANCH_ANY_CALL |
		PERF_SAMPLE_BRANCH_ANY_RETURN |
		PERF_SAMPLE_BRANCH_ANY,

		PERF_SAMPLE_BRANCH_ANY_RETURN |
		PERF_SAMPLE_BRANCH_COND |
		PERF_SAMPLE_BRANCH_IND_CALL,

		PERF_SAMPLE_BRANCH_ANY_RETURN |
		PERF_SAMPLE_BRANCH_COND |
		PERF_SAMPLE_BRANCH_ANY,

		PERF_SAMPLE_BRANCH_COND |
		PERF_SAMPLE_BRANCH_IND_CALL |
		PERF_SAMPLE_BRANCH_ANY,

		PERF_SAMPLE_BRANCH_ANY_CALL |		/* Quadruple filters */
		PERF_SAMPLE_BRANCH_ANY_RETURN |
		PERF_SAMPLE_BRANCH_COND |
		PERF_SAMPLE_BRANCH_IND_CALL,

		PERF_SAMPLE_BRANCH_ANY_CALL |
		PERF_SAMPLE_BRANCH_ANY_RETURN |
		PERF_SAMPLE_BRANCH_COND |
		PERF_SAMPLE_BRANCH_ANY,

		PERF_SAMPLE_BRANCH_ANY_CALL |
		PERF_SAMPLE_BRANCH_ANY_RETURN |
		PERF_SAMPLE_BRANCH_IND_CALL |
		PERF_SAMPLE_BRANCH_ANY,

		PERF_SAMPLE_BRANCH_ANY_CALL |
		PERF_SAMPLE_BRANCH_COND |
		PERF_SAMPLE_BRANCH_IND_CALL |
		PERF_SAMPLE_BRANCH_ANY,

		PERF_SAMPLE_BRANCH_ANY_RETURN |
		PERF_SAMPLE_BRANCH_COND |
		PERF_SAMPLE_BRANCH_IND_CALL |
		PERF_SAMPLE_BRANCH_ANY,

		PERF_SAMPLE_BRANCH_ANY_CALL |		/* All filters */
		PERF_SAMPLE_BRANCH_ANY_RETURN |
		PERF_SAMPLE_BRANCH_COND |
		PERF_SAMPLE_BRANCH_IND_CALL |
		PERF_SAMPLE_BRANCH_ANY };

static unsigned int all_set[ALL_MAX], call_set[CALL_MAX];
static unsigned int ret_set[RET_MAX], cond_set[COND_MAX], ind_set[IND_MAX];

static bool has_failed;
static unsigned long branch_any_call;
static unsigned long branch_any_return;
static unsigned long branch_cond;
static unsigned long branch_ind_call;
static unsigned long branch_any;
static unsigned long branch_total;

static void init_branch_stats(void)
{
	branch_any_call = 0;
	branch_any_return = 0;
	branch_cond = 0;
	branch_ind_call = 0;
	branch_any = 0;
	branch_total = 0;
}

static void show_branch_stats(void)
{
	printf("BRANCH STATS\n");
	printf("ANY_CALL:	%ld\n", branch_any_call);
	printf("ANY_RETURN:	%ld\n", branch_any_return);
	printf("COND:		%ld\n", branch_cond);
	printf("IND_CALL:	%ld\n", branch_ind_call);
	printf("ANY:		%ld\n", branch_any);
	printf("TOTAL:		%ld\n", branch_total);

}

static void fetch_branches(void)
{
	int i;

	/* Clear */
	memset(all_set, 0, sizeof(all_set));
	memset(call_set, 0, sizeof(call_set));
	memset(ret_set, 0, sizeof(ret_set));
	memset(cond_set, 0, sizeof(cond_set));
	memset(ind_set, 0, sizeof(ind_set));

	/* Fetch */
	fetch_all_branches(all_set);
	fetch_all_calls(call_set);
	fetch_all_rets(ret_set);
	fetch_all_conds(cond_set);
	fetch_all_inds(ind_set);

	/* Display */
	printf("ANY branches\n");
	for (i = 0; i < ALL_MAX; i += 2)
		printf("%x ---> %x\n", all_set[i], all_set[i + 1]);

	printf("ANY_CALL branches\n");
	for (i = 0; i < CALL_MAX; i += 2)
		printf("%x ---> %x\n", call_set[i], call_set[i + 1]);

	printf("ANY_RETURN branches\n");
	for (i = 0; i < RET_MAX; i += 2)
		printf("%x ---> %x\n", ret_set[i], ret_set[i + 1]);

	printf("COND branches\n");
	for (i = 0; i < COND_MAX; i += 2)
		printf("%x ---> %x\n", cond_set[i], cond_set[i + 1]);

	printf("IND_CALL branches\n");
	for (i = 0; i < IND_MAX; i += 2)
		printf("%x ---> %x\n", ind_set[i], ind_set[i + 1]);

}

/* Perf mmap stats */
static unsigned long record_sample;
static unsigned long record_mmap;
static unsigned long record_lost;
static unsigned long record_throttle;
static unsigned long record_unthrottle;
static unsigned long record_overlap;

static void init_perf_mmap_stats(void)
{
	record_sample = 0;
	record_mmap = 0;
	record_lost = 0;
	record_throttle = 0;
	record_unthrottle = 0;
	record_overlap = 0;
}

static void show_perf_mmap_stats(void)
{
	printf("PERF STATS\n");
	printf("OVERLAP:		%ld\n", record_overlap);
	printf("RECORD_SAMPLE:		%ld\n", record_sample);
	printf("RECORD_MAP:		%ld\n", record_mmap);
	printf("RECORD_LOST:		%ld\n", record_lost);
	printf("RECORD_THROTTLE:	%ld\n", record_throttle);
	printf("RECORD_UNTHROTTLE:	%ld\n", record_unthrottle);
}

static bool search_all_set(unsigned long from, unsigned long to)
{
	int i;

	for (i = 0; i < ALL_MAX; i += 2) {
		if ((all_set[i] == from) && (all_set[i+1] == to))
			return true;
	}
	return false;
}

static bool search_call_set(unsigned long from, unsigned long to)
{
	int i;

	for (i = 0; i < CALL_MAX; i += 2) {
		if ((call_set[i] == from) && (call_set[i+1] == to))
			return true;
	}
	return false;
}

static bool search_ret_set(unsigned long from, unsigned long to)
{
	int i;

	for (i = 0; i < RET_MAX; i += 2) {
		if ((ret_set[i] == from) && (ret_set[i+1] == to))
			return true;
	}
	return false;
}

static bool search_cond_set(unsigned long from, unsigned long to)
{
	int i;

	for (i = 0; i < COND_MAX; i += 2) {
		if ((cond_set[i] == from) && (cond_set[i+1] == to))
			return true;
	}
	return false;
}

static bool search_ind_set(unsigned long from, unsigned long to)
{
	int i;

	for (i = 0; i < IND_MAX; i += 2) {
		if ((ind_set[i] == from) && (ind_set[i+1] == to))
			return true;
	}
	return false;
}

static int check_branch(unsigned long from, unsigned long to,
					int branch_sample_type)
{
	bool result = false;

	if (branch_sample_type & PERF_SAMPLE_BRANCH_ANY_CALL) {
		if (search_call_set(from, to)) {
			branch_any_call++;
			result = true;
		}
	}

	if (branch_sample_type & PERF_SAMPLE_BRANCH_ANY_RETURN) {
		if (search_ret_set(from, to)) {
			branch_any_return++;
			result = true;
		}
	}

	if (branch_sample_type & PERF_SAMPLE_BRANCH_COND) {
		if (search_cond_set(from, to)) {
			branch_cond++;
			result = true;
		}
	}

	if (branch_sample_type & PERF_SAMPLE_BRANCH_IND_CALL) {
		if (search_ind_set(from, to)) {
			branch_ind_call++;
			result = true;
		}
	}

	if (branch_sample_type & PERF_SAMPLE_BRANCH_ANY) {
		if (search_all_set(from, to)) {
			branch_any++;
			result = true;
		}
	}

	branch_total++;
	return result;
}

static int64_t *ring_buffer_offset(struct ring_buffer *r, void *p)
{
	unsigned long l = (unsigned long)p;

	return (int64_t *)(r->ring_base + ((l - r->ring_base) & r->mask));
}

static void dump_sample(struct perf_event_header *hdr,
			struct ring_buffer *r, int branch_sample_type)
{
	int64_t from, to, flag;
	int64_t *v, nr;
	int i;

	/* NR Branches */
	v = ring_buffer_offset(r, hdr + 1);

	nr = *v;

	/* Branches */
	for (i = 0; i < nr; i++) {
		v = ring_buffer_offset(r, v + 1);
		from = *v;

		v = ring_buffer_offset(r, v + 1);
		to = *v;

		v = ring_buffer_offset(r, v + 1);
		flag = *v;

		/* Skip incomplete branch records */
		if (!from || !to)
			continue;

		if (!check_branch(from, to, branch_sample_type)) {
			has_failed = true;
			printf("[Filter: %d] From: %lx To: %lx Flags: %lx\n",
					branch_sample_type, from, to, flag);
		}
	}
}

/*
 * XXX: Both the memory barriers used here are as per the
 * directive mentioned in the header include/uapi/linux/
 * perf_event.h while describing the perf_event_mmap_page
 * structure.
 */
static void read_ring_buffer(struct event *e, int branch_sample_type)
{
	struct ring_buffer *r = &e->ring_buffer;
	struct perf_event_header *hdr;
	int tail, head;

	head = r->page->data_head & r->mask;

	/* XXX: perf kernel interface requires read barrier */
	smp_mb();

	tail = r->page->data_tail & r->mask;

	while (tail != head) {
		hdr = (struct perf_event_header *)(r->ring_base + tail);

		if ((tail & r->mask) + hdr->size !=
					((tail + hdr->size) & r->mask))
			++record_overlap;

		if (hdr->type == PERF_RECORD_SAMPLE) {
			++record_sample;
			dump_sample(hdr, r, branch_sample_type);
		}

		if (hdr->type == PERF_RECORD_MMAP)
			++record_mmap;

		if (hdr->type == PERF_RECORD_LOST)
			++record_lost;

		if (hdr->type == PERF_RECORD_THROTTLE)
			++record_throttle;

		if (hdr->type == PERF_RECORD_UNTHROTTLE)
			++record_unthrottle;

		tail = (tail + hdr->size) & r->mask;
	}

	/* XXX: perf kernel interface requires read and write barrier */
	smp_mb();
	r->page->data_tail = tail;
}

static void event_mmap(struct event *e)
{
	struct ring_buffer *r = &e->ring_buffer;

	r->page = mmap(NULL, 9 * getpagesize(), PROT_READ |
					PROT_WRITE, MAP_SHARED, e->fd, 0);

	if (r->page == MAP_FAILED) {
		r->page = NULL;
		perror("mmap() failed");
	}

	r->mask = (8 * getpagesize()) - 1;
	r->ring_base = (unsigned long)r->page + getpagesize();

}

static int filter_test(int branch_sample_type)
{
	struct pollfd pollfd;
	struct event ebhrb;
	pid_t pid;
	int ret, loop = 0;

	has_failed = false;
	pid = fork();
	if (pid == -1) {
		perror("fork() failed");
		return 1;
	}

	/* Run child */
	if (pid == 0) {
		start_loop();
		exit(0);
	}

	/* Prepare event */
	event_init_opts(&ebhrb, PERF_COUNT_HW_INSTRUCTIONS,
				PERF_TYPE_HARDWARE, "instructions");
	ebhrb.attr.sample_type = PERF_SAMPLE_BRANCH_STACK;
	ebhrb.attr.disabled = 1;
	ebhrb.attr.mmap = 1;
	ebhrb.attr.mmap_data = 1;
	ebhrb.attr.sample_period = SAMPLE_PERIOD;
	ebhrb.attr.exclude_user = 0;
	ebhrb.attr.exclude_kernel = 1;
	ebhrb.attr.exclude_hv = 1;
	ebhrb.attr.branch_sample_type = branch_sample_type;

	/* Open event */
	event_open_with_pid(&ebhrb, pid);

	/* Mmap ring buffer and enable event */
	event_mmap(&ebhrb);
	FAIL_IF(event_enable(&ebhrb));

	/* Prepare polling */
	pollfd.fd = ebhrb.fd;
	pollfd.events = POLLIN;

	for (loop = 0; loop < LOOP_COUNT; loop++) {
		ret = poll(&pollfd, 1, -1);
		if (ret == -1) {
			perror("poll() failed");
			goto error;
		}
		if (ret == 0) {
			perror("poll() timeout");
			goto error;
		}
		read_ring_buffer(&ebhrb, branch_sample_type);
		if (has_failed)
			goto error;
	}

	/* Disable and close event */
	FAIL_IF(event_disable(&ebhrb));
	event_close(&ebhrb);

	/* Terminate child */
	kill(pid, SIGKILL);
	return 0;

error:
	/* Disable and close event */
	FAIL_IF(event_disable(&ebhrb));
	event_close(&ebhrb);

	/* Terminate child */
	kill(pid, SIGKILL);
	return 1;
}

static int  bhrb_filters_test(void)
{
	int branch_sample_type;
	int i;

	/* Fetch branches */
	fetch_branches();
	init_branch_stats();
	init_perf_mmap_stats();

	for (i = 0; i < sizeof(branch_test_set)/sizeof(int); i++) {
		branch_sample_type = branch_test_set[i];
		if (filter_test(branch_sample_type))
			return 1;
	}

	/* Show stats */
	show_branch_stats();
	show_perf_mmap_stats();
	return 0;
}

int main(int argc, char *argv[])
{
	return test_harness(bhrb_filters_test, "bhrb_filters");
}
