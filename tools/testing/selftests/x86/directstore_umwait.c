// SPDX-License-Identifier: GPL-2.0
/*
 * directstore_umwait.c - Test APIs defined in lib_direct_store.h and
 * lib_user_wait.h
 *
 * Copyright (c) 2018 Intel Corporation
 * Fenghua Yu <fenghua.yu@intel.com>
 */
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <asm/lib_direct_store.h>
#include <asm/lib_user_wait.h>

void test_movdiri_32_bit(void)
{
	int __attribute((aligned(64))) dst[10];
	int __attribute((aligned(64))) data;

	if (!movdiri_supported()) {
		printf("movdiri is not supported\n");

		return;
	}
	dst[0] = 0;
	data = 0x12345670;

	movdiri32(dst, data);

	if (dst[0] == data)
		printf("movdiri 32-bit test passed\n");
	else
		printf("movdiri 32-bit test failed\n");
}

void test_movdiri_64_bit(void)
{
	long __attribute((aligned(64))) dst[10];
	long __attribute((aligned(64))) data;

	if (!movdiri_supported()) {
		printf("movdiri is not supported\n");

		return;
	}
	dst[0] = 0;
	data = 0x123456789abcdef0;

	movdiri64(dst, data);

	if (dst[0] == data)
		printf("movdiri 64-bit test passed\n");
	else
		printf("movdiri 64-bit test failed\n");
}

void test_movdiri(void)
{
	test_movdiri_32_bit();
	test_movdiri_64_bit();
}

void test_movdir64b(void)
{
	char __attribute((aligned(64))) src[1024], dst[1024];

	if (!movdir64b_supported()) {
		printf("movdir64b is not supported\n");

		return;
	}
	memset(src, 0, 1024);
	memset(dst, 0, 1024);
	for (int i = 0; i < 1024; i++)
		dst[i] = i;

	movdir64b(src, dst);
	if (memcmp(src, dst, 64))
		printf("movdir64b test failed\n");
	else
		printf("movdir64b test passed\n");
}

void test_timeout(char *test_name, int state, unsigned long timeout_ns,
		  unsigned long overhead_ns)
{
	unsigned long tsc1, tsc2, real_tsc, real_ns, tsc_per_nsec;
	int ret;

	ret = nsec_to_tsc(1, &tsc_per_nsec);
	if (ret) {
		printf("umwait test failed: nsec cannot be coverted to tsc.\n");
		return;
	}

	if (waitpkg_supported()) {
		if (!strcmp(test_name, "umwait")) {
			tsc1 = rdtsc();
			umwait(state, timeout_ns);
			tsc2 = rdtsc();
		} else {
			tsc1 = rdtsc();
			tpause(state, timeout_ns);
			tsc2 = rdtsc();
		}
		real_tsc = tsc2 - tsc1;
		real_ns = real_tsc / tsc_per_nsec;
		/* Give enough time for overhead on slow running machine. */
		if (abs(real_ns - timeout_ns) < overhead_ns) {
			printf("%s test passed\n", test_name);
		} else {
			printf("%s test failed:\n", test_name);
			printf("real=%luns, expected=%luns. ",
			       real_ns, timeout_ns);
			printf("Likely due to slow machine. ");
			printf("Please adjust overhead_ns or re-run test for a few more times.\n");
		}
	} else {
		printf("%s is not supported\n", test_name);
	}
}

void test_tpause_timeout(int state)
{
	/*
	 * Timeout 100usec. Assume overhead of executing umwait is 10usec.
	 * You can adjust the overhead number based on your machine.
	 */
	test_timeout("tpause", state, 100000, 10000);
}

void test_tpause(void)
{
	/* Test timeout in state 0 (C0.2). */
	test_tpause_timeout(0);
	/* Test timeout in state 1 (C0.1). */
	test_tpause_timeout(1);
	/* More tests ... */
}

char umonitor_range[1024];

void test_umonitor_only(void)
{
	if (waitpkg_supported()) {
		umonitor(umonitor_range);
		printf("umonitor test passed\n");
	} else {
		printf("waitpkg not supported\n");
	}
}

void show_basic_info(void)
{
	unsigned long tsc;
	int ret;

	ret = nsec_to_tsc(1, &tsc);
	if (ret < 0)
		printf("not tsc freq CPUID available\n");
	else
		printf("1 nsec = %lu tsc\n", tsc);
}

void test_umonitor(void)
{
	test_umonitor_only();
}

void test_umwait_timeout(int state)
{
	/*
	 * Timeout 100usec. Overhead of executing umwait assumes 90usec.
	 * You can adjust the overhead number based on your machine.
	 */
	test_timeout("umwait", state, 100000, 90000);
}

void test_umwait(void)
{
	/* Test timeout in state 0 (C0.2). */
	test_umwait_timeout(0);
	/* Test timeout in state 1 (C0.1). */
	test_umwait_timeout(1);
	/* More tests ... */
}

int main(void)
{
	show_basic_info();
	test_movdiri();
	test_movdir64b();
	test_tpause();
	test_umonitor();
	test_umwait();

	return 0;
}
