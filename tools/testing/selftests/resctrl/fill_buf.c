// SPDX-License-Identifier: GPL-2.0
/*
 * fill_buf benchmark
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Authors:
 *    Arshiya Hayatkhan Pathan <arshiya.hayatkhan.pathan@intel.com>
 *    Sai Praneeth Prakhya <sai.praneeth.prakhya@intel.com>,
 *    Fenghua Yu <fenghua.yu@intel.com>
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <inttypes.h>
#include <malloc.h>
#include <string.h>

#include "resctrl.h"

#define CL_SIZE			(64)
#define PAGE_SIZE		(4 * 1024)
#define MB			(1024 * 1024)

static unsigned char *startptr;

static void sb(void)
{
	asm volatile("sfence\n\t"
		     : : : "memory");
}

static void ctrl_handler(int signo)
{
	free(startptr);
	printf("\nEnding\n");
	sb();
	exit(EXIT_SUCCESS);
}

static void cl_flush(void *p)
{
	asm volatile("clflush (%0)\n\t"
		     : : "r"(p) : "memory");
}

static void mem_flush(void *p, size_t s)
{
	char *cp = (char *)p;
	size_t i = 0;

	s = s / CL_SIZE; /* mem size in cache llines */

	for (i = 0; i < s; i++)
		cl_flush(&cp[i * CL_SIZE]);

	sb();
}

static void *malloc_and_init_memory(size_t s)
{
	uint64_t *p64;
	size_t s64;

	void *p = memalign(PAGE_SIZE, s);

	p64 = (uint64_t *)p;
	s64 = s / sizeof(uint64_t);

	while (s64 > 0) {
		*p64 = (uint64_t)rand();
		p64 += (CL_SIZE / sizeof(uint64_t));
		s64 -= (CL_SIZE / sizeof(uint64_t));
	}

	return p;
}

static void fill_cache_read(unsigned char *start_ptr, unsigned char *end_ptr)
{
	while (1) {
		unsigned char sum, *p;

		p = start_ptr;
		/* Read two chars in each cache line to stress cache */
		while (p < (end_ptr - 1024)) {
			sum += p[0] + p[32] + p[64] + p[96] + p[128] +
			       p[160] + p[192] + p[224] + p[256] + p[288] +
			       p[320] + p[352] + p[384] + p[416] + p[448] +
			       p[480] + p[512] + p[544] + p[576] + p[608] +
			       p[640] + p[672] + p[704] + p[736] + p[768] +
			       p[800] + p[832] + p[864] + p[896] + p[928] +
			       p[960] + p[992];
			p += 1024;
		}
	}
}

static void fill_cache_write(unsigned char *start_ptr, unsigned char *end_ptr)
{
	while (1) {
		while (start_ptr < end_ptr) {
			*start_ptr = '1';
			start_ptr += (CL_SIZE / 2);
		}
		start_ptr = startptr;
	}
}

static void
fill_cache(unsigned long long buf_size, int malloc_and_init,
	   int memflush, int op)
{
	unsigned char *start_ptr, *end_ptr;
	unsigned long long i;

	if (malloc_and_init) {
		start_ptr = malloc_and_init_memory(buf_size);
		printf("Started benchmark with memalign\n");
	} else {
		start_ptr = malloc(buf_size);
		printf("Started benchmark with malloc\n");
	}

	if (!start_ptr)
		return;

	startptr = start_ptr;
	end_ptr = start_ptr + buf_size;

	/*
	 * It's better to touch the memory once to avoid any compiler
	 * optimizations
	 */
	if (!malloc_and_init) {
		for (i = 0; i < buf_size; i++)
			*start_ptr++ = (unsigned char)rand();
	}

	start_ptr = startptr;

	/* Flush the memory before using to avoid "cache hot pages" effect */
	if (memflush) {
		mem_flush(start_ptr, buf_size);
		printf("Started benchmark with memflush\n");
	} else {
		printf("Started benchmark *without* memflush\n");
	}

	if (op == 0)
		fill_cache_read(start_ptr, end_ptr);
	else
		fill_cache_write(start_ptr, end_ptr);

	free(startptr);
}

int run_fill_buf(int span, int malloc_and_init_memory, int memflush, int op)
{
	unsigned long long cache_size = span * MB;

	/* set up ctrl-c handler */
	if (signal(SIGINT, ctrl_handler) == SIG_ERR)
		printf("Failed to catch SIGINT!\n");
	if (signal(SIGHUP, ctrl_handler) == SIG_ERR)
		printf("Failed to catch SIGHUP!\n");

	printf("Cache size in Bytes = %llu\n", cache_size);

	fill_cache(cache_size, malloc_and_init_memory, memflush, op);

	return -1;
}
