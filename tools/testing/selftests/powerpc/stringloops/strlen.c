// SPDX-License-Identifier: GPL-2.0
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "utils.h"

#define SIZE 256
#define ITERATIONS 1000
#define ITERATIONS_BENCH 100000

int test_strlen(const void *s);

/* test all offsets and lengths */
static void test_one(char *s)
{
	unsigned long offset;

	for (offset = 0; offset < SIZE; offset++) {
		int x, y;
		unsigned long i;

		y = strlen(s + offset);
		x = test_strlen(s + offset);

		if (x != y) {
			printf("strlen() returned %d, should have returned %d (%p offset %ld)\n", x, y, s, offset);

			for (i = offset; i < SIZE; i++)
				printf("%02x ", s[i]);
			printf("\n");
		}
	}
}

static int testcase(void)
{
	char *s;
	unsigned long i;
	struct timespec ts_start, ts_end;

	s = memalign(128, SIZE);
	if (!s) {
		perror("memalign");
		exit(1);
	}

	srandom(1);

	memset(s, 0, SIZE);
	for (i = 0; i < SIZE; i++) {
		char c;

		do {
			c = random() & 0x7f;
		} while (!c);
		s[i] = c;
		test_one(s);
	}

	for (i = 0; i < ITERATIONS; i++) {
		unsigned long j;

		for (j = 0; j < SIZE; j++) {
			char c;

			do {
				c = random() & 0x7f;
			} while (!c);
			s[j] = c;
		}
		for (j = 0; j < sizeof(long); j++) {
			s[SIZE - 1 - j] = 0;
			test_one(s);
		}
	}

	for (i = 0; i < SIZE; i++) {
		char c;

		do {
			c = random() & 0x7f;
		} while (!c);
		s[i] = c;
	}

	clock_gettime(CLOCK_MONOTONIC, &ts_start);

	s[SIZE - 1] = 0;
	for (i = 0; i < ITERATIONS_BENCH; i++)
		test_strlen(s);

	clock_gettime(CLOCK_MONOTONIC, &ts_end);

	printf("len %3.3d : time = %.6f\n", SIZE, ts_end.tv_sec - ts_start.tv_sec + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9);

	clock_gettime(CLOCK_MONOTONIC, &ts_start);

	s[16] = 0;
	for (i = 0; i < ITERATIONS_BENCH; i++)
		test_strlen(s);

	clock_gettime(CLOCK_MONOTONIC, &ts_end);

	printf("len 16  : time = %.6f\n", ts_end.tv_sec - ts_start.tv_sec + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9);

	clock_gettime(CLOCK_MONOTONIC, &ts_start);

	s[4] = 0;
	for (i = 0; i < ITERATIONS_BENCH; i++)
		test_strlen(s);

	clock_gettime(CLOCK_MONOTONIC, &ts_end);

	printf("len 4   : time = %.6f\n", ts_end.tv_sec - ts_start.tv_sec + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9);

	return 0;
}

int main(void)
{
	return test_harness(testcase, "strlen");
}
