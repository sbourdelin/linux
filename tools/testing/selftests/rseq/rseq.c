#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rseq.h"

__thread volatile const int __rseq_current_cpu = -1;

#define __NR_rseq	323
#define SYS_RSEQ_SET_CRITICAL		0
#define SYS_RSEQ_SET_CPU_POINTER	1

int sys_rseq(int op, int flags, void *val1, void *val2, void *val3)
{
	return syscall(__NR_rseq, op, flags,
		(intptr_t)val1, (intptr_t)val2, (intptr_t)val3);
}

static void sys_rseq_checked(int op, int flags,
			void *val1, void *val2, void *val3)
{
	int rc = sys_rseq(op, flags, val1, val2, val3);

	if (rc) {
		fprintf(stderr, "sys_rseq(%d, %d, %p, %p, %p) failed(%d): %s\n",
			op, flags, val1, val2, val3, errno, strerror(errno));
		exit(1);
	}
}

void rseq_configure_region(void *rseq_text_start, void *rseq_text_end,
			void *rseq_text_restart)
{
	sys_rseq_checked(SYS_RSEQ_SET_CRITICAL, 0,
			rseq_text_start, rseq_text_end, rseq_text_restart);
}

void rseq_configure_cpu_pointer(void)
{
	sys_rseq_checked(SYS_RSEQ_SET_CPU_POINTER, 0,
			(void *)&__rseq_current_cpu, 0, 0);
	assert(rseq_current_cpu() != -1); /* always updated prior to return. */
}
