/*
 * Copyright 2016, Anshuman Khandual, IBM Corp.
 * Licensed under GPLv2.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "../pmu/event.c"

#define ADDR_INPUT	0xa0000000000UL
#define HPAGE_SIZE	0x1000000
#define PSIZE_64K	0x10000
#define PSIZE_4K	0x1000

#define MAX_MM_EVENTS	8

struct event mm_events[MAX_MM_EVENTS];

static void setup_event(struct event *e, u64 config, char *name)
{
	event_init_opts(e, config, PERF_TYPE_SOFTWARE, name);
	e->attr.disabled = 1;
	e->attr.exclude_kernel = 1;
	e->attr.exclude_hv = 1;
	e->attr.exclude_idle = 1;
}

static void setup_event_tr(struct event *e, u64 config, char *name)
{
	memset(e, 0, sizeof(*e));

	e->name = name;
	e->attr.type = PERF_TYPE_TRACEPOINT;
	e->attr.config = config;
	e->attr.size = sizeof(e->attr);
	e->attr.sample_period = PERF_SAMPLE_IDENTIFIER;
	e->attr.inherit = 1;
	e->attr.enable_on_exec = 1;
	e->attr.exclude_guest = 1;

	/* This has to match the structure layout in the header */
	e->attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | \
				PERF_FORMAT_TOTAL_TIME_RUNNING;
	e->attr.disabled = 1;
}


static void prepare_events(void)
{
	int i;

	for (i = 0; i < MAX_MM_EVENTS; i++)
		event_reset(&mm_events[i]);

	for (i = 0; i < MAX_MM_EVENTS; i++)
		event_enable(&mm_events[i]);
}

static void close_events(void)
{
	int i;

	for (i = 0; i < MAX_MM_EVENTS; i++)
		event_close(&mm_events[i]);
}

static void display_events(void)
{
	int i;

	for (i = 0; i < MAX_MM_EVENTS; i++)
		event_disable(&mm_events[i]);

	for (i = 0; i < MAX_MM_EVENTS; i++)
		event_read(&mm_events[i]);

	for (i = 0; i < MAX_MM_EVENTS; i++)
		printf("[%20s]: \t %llu\n", mm_events[i].name, mm_events[i].result.value);
}

static void setup_events(void)
{
	setup_event(&mm_events[0], PERF_COUNT_SW_PAGE_FAULTS, "faults");
	setup_event(&mm_events[1], PERF_COUNT_SW_PAGE_FAULTS_MAJ, "major-faults");
	setup_event(&mm_events[2], PERF_COUNT_SW_PAGE_FAULTS_MIN, "minor-faults");

	setup_event_tr(&mm_events[3], 22 , "hash_faults");
	setup_event_tr(&mm_events[4], 20 , "hash_faults_thp");
	setup_event_tr(&mm_events[5], 19 , "hash_faults_64K");
	setup_event_tr(&mm_events[6], 18 , "hash_faults_4K");
	setup_event_tr(&mm_events[7], 21 , "hash_faults_hugetlb");
}

static void open_events()
{
	int i;

	for (i = 0; i < MAX_MM_EVENTS; i++) {
		if (event_open(&mm_events[i]))
			perror("event_open() failed");
	}
}

static void subpage_prot_change(char *ptr, unsigned long size)
{
	unsigned int *map;
	unsigned long npages, i, err;

	npages = size / PSIZE_64K;
	map = malloc(sizeof(unsigned int) * npages);
	if (!map)  {
		perror("malloc() failed");
		exit(-1);
	}

	for (i = 0; i < npages; i++)
		map[i] = 0;

	err = syscall(__NR_subpage_prot, ptr, size, map);
	if (err) {
		perror("subpage() protection failed");
		exit(-1);
	}
}

static void dont_need_request(char *ptr, unsigned long size)
{
	if (madvise(ptr, size, MADV_DONTNEED)){
		perror("madvise");
		exit(-1);
	}
}

static void thp_request(char *ptr, unsigned long size)
{
	if (madvise(ptr, size, MADV_HUGEPAGE)){
		perror("madvise");
		exit(-1);
	}
}

int main(int argc, char *argv[])
{
	int nr_hp = strtol(argv[1], NULL, 0);
	char *ptr, *htlb;

	setup_events();
	open_events();

	do {
		printf("HugeTLB allocation::::::::\n");
		htlb = mmap(NULL, nr_hp * HPAGE_SIZE, PROT_READ | PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_HUGETLB, -1, 0);
		if (!htlb) {
			perror("mmap");
			exit(-1);
		}

		prepare_events();
		memset(htlb, 0, nr_hp * HPAGE_SIZE);
		display_events();

		printf("THP allocation::::::::\n");
		ptr = mmap((void *) ADDR_INPUT, nr_hp * HPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (ptr != (void *) ADDR_INPUT) {
			perror("mmap");
			exit(-1);
		}

		thp_request(ptr, nr_hp * HPAGE_SIZE);
		prepare_events();
		memset(ptr, 0, nr_hp * HPAGE_SIZE);
		display_events();

		printf("SUBPAGE protection::::\n");
		subpage_prot_change(ptr, nr_hp * HPAGE_SIZE);

		prepare_events();
		memset(ptr, 0, nr_hp * HPAGE_SIZE);
		display_events();

		printf("PFN flush::::::::::::\n");
		dont_need_request(ptr, nr_hp * HPAGE_SIZE);

		prepare_events();
		memset(ptr, 0, nr_hp * HPAGE_SIZE);
		display_events();

		munmap(ptr, nr_hp * HPAGE_SIZE);
		munmap(htlb, nr_hp * HPAGE_SIZE);

	} while(0);
	close_events();
	return 0;
}
