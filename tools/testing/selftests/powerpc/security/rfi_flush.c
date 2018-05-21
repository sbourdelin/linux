#include <sys/types.h>
#include <stdint.h>
#include <malloc.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "utils.h"

#define CACHELINE_SIZE	128

struct perf_event_read {
	uint64_t nr;
	uint64_t l1d_misses;
};

static inline uint64_t load(void *addr)
{
	uint64_t tmp;

	asm volatile("ld %0,0(%1)" : "=r"(tmp) : "b" (addr));

	return tmp;
}

static void syscall_loop(char *p, unsigned long iterations, unsigned long zero_size)
{
	for (unsigned long i = 0; i < iterations; i++) {
		for (unsigned long j = 0; j < zero_size; j += CACHELINE_SIZE)
			load(p + j);
		getppid();
	}
}

int rfi_flush_test(void)
{
	char *p;
	int repetitions = 10;
	int fd, passes = 0, iter, rc = 0;
	struct perf_event_read v;
	uint64_t l1d_misses_total = 0;
	unsigned long iterations = 100000, zero_size = 24*1024;
	int rfi_flush_org, rfi_flush;

	SKIP_IF(geteuid() != 0);

	if (read_debugfs_file("powerpc/rfi_flush", &rfi_flush_org)) {
		perror("error reading powerpc/rfi_flush debugfs file");
	        printf("unable to determine current rfi_flush setting");
		return 1;
	}

	rfi_flush = rfi_flush_org;

	FAIL_IF((fd = perf_event_open_counter(PERF_TYPE_RAW,
					  0x400f0, /* L1d miss */
					  -1)) < 0);

	p = (char *)memalign(zero_size, CACHELINE_SIZE);

	FAIL_IF(perf_event_enable(fd));

	set_dscr(1);

	iter = repetitions;

again:
	FAIL_IF(perf_event_reset(fd));

	syscall_loop(p, iterations, zero_size);

	FAIL_IF(read(fd, &v, sizeof(v)) != sizeof(v));

	/* Expect at least zero_size/CACHELINE_SIZE misses per iteration */
	if (v.l1d_misses >= (iterations * zero_size / CACHELINE_SIZE) &&
			rfi_flush)
		passes++;
	else if (v.l1d_misses < iterations && !rfi_flush)
		passes++;

	l1d_misses_total += v.l1d_misses;

	while (--iter)
		goto again;

	if (passes < repetitions) {
		printf("FAIL (L1D misses with rfi_flush=%d: %lu %c %lu) [%d/%d failures]\n",
			rfi_flush,
			l1d_misses_total,
			rfi_flush ? '<' : '>',
			rfi_flush ? (repetitions * iterations * zero_size/CACHELINE_SIZE) :
				iterations,
			repetitions - passes, repetitions);
		rc = 1;
	} else
		printf("PASS (L1D misses with rfi_flush=%d: %lu %c %lu) [%d/%d pass]\n",
			rfi_flush,
			l1d_misses_total,
			rfi_flush ? '>' : '<',
			rfi_flush ? (repetitions * iterations * zero_size/CACHELINE_SIZE) :
				iterations,
			passes, repetitions);

	if (rfi_flush == rfi_flush_org) {
		rfi_flush = !rfi_flush_org;
		if (write_debugfs_file("powerpc/rfi_flush", rfi_flush) < 0) {
			perror("error writing to powerpc/rfi_flush debugfs file");
			return 1;
		}
		iter = repetitions;
		l1d_misses_total = 0;
		passes = 0;
		goto again;
	}

	perf_event_disable(fd);
	close(fd);

	set_dscr(0);

	if (write_debugfs_file("powerpc/rfi_flush", rfi_flush_org) < 0) {
		perror("unable to restore original value of powerpc/rfi_flush debugfs file");
		return 1;
	}

	return rc;
}

int main(int argc, char *argv[])
{
	return test_harness(rfi_flush_test, "rfi_flush_test");
}
