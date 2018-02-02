// SPDX-License-Identifier: GPL-2.0
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../util/header.h"

/* This code based on gcc cpuid.h __get_cpuid_max() */
unsigned int have_cpuid(void)
{
	unsigned int __eax, __ebx;

#ifndef __x86_64__
/* See if we can use cpuid.  On AMD64 we always can.  */
#if __GNUC__ >= 3
	__asm__ ("pushf{l|d}\n\t"
		"pushf{l|d}\n\t"
		"pop{l}\t%0\n\t"
		"mov{l}\t{%0, %1|%1, %0}\n\t"
		"xor{l}\t{%2, %0|%0, %2}\n\t"
		"push{l}\t%0\n\t"
		"popf{l|d}\n\t"
		"pushf{l|d}\n\t"
		"pop{l}\t%0\n\t"
		"popf{l|d}\n\t"
		: "=&r" (__eax), "=&r" (__ebx)
		: "i" (0x00200000));
#else
/* Host GCCs older than 3.0 weren't supporting Intel asm syntax
 * nor alternatives in i386 code.
 */
	__asm__ ("pushfl\n\t"
		"pushfl\n\t"
		"popl\t%0\n\t"
		"movl\t%0, %1\n\t"
		"xorl\t%2, %0\n\t"
		"pushl\t%0\n\t"
		"popfl\n\t"
		"pushfl\n\t"
		"popl\t%0\n\t"
		"popfl\n\t"
		: "=&r" (__eax), "=&r" (__ebx)
		: "i" (0x00200000));
#endif

	if (!((__eax ^ __ebx) & 0x00200000))
		return 0;
	else
		return 1;
#endif

/* All x86_64 support cpuid */
	return 1;
}

static inline void
cpuid(unsigned int op, unsigned int *a, unsigned int *b, unsigned int *c,
      unsigned int *d)
{
	__asm__ __volatile__ (".byte 0x53\n\tcpuid\n\t"
			      "movl %%ebx, %%esi\n\t.byte 0x5b"
			: "=a" (*a),
			"=S" (*b),
			"=c" (*c),
			"=d" (*d)
			: "a" (op));
}

static int
__get_cpuid(char *buffer, size_t sz, const char *fmt)
{
	unsigned int a, b, c, d, lvl;
	int family = -1, model = -1, step = -1;
	int nb;
	char vendor[16];

	if (!have_cpuid())
		return -1;

	cpuid(0, &lvl, &b, &c, &d);
	strncpy(&vendor[0], (char *)(&b), 4);
	strncpy(&vendor[4], (char *)(&d), 4);
	strncpy(&vendor[8], (char *)(&c), 4);
	vendor[12] = '\0';

	if (lvl >= 1) {
		cpuid(1, &a, &b, &c, &d);

		family = (a >> 8) & 0xf;  /* bits 11 - 8 */
		model  = (a >> 4) & 0xf;  /* Bits  7 - 4 */
		step   = a & 0xf;

		/* extended family */
		if (family == 0xf)
			family += (a >> 20) & 0xff;

		/* extended model */
		if (family >= 0x6)
			model += ((a >> 16) & 0xf) << 4;
	}
	nb = scnprintf(buffer, sz, fmt, vendor, family, model, step);

	/* look for end marker to ensure the entire data fit */
	if (strchr(buffer, '$')) {
		buffer[nb-1] = '\0';
		return 0;
	}
	return -1;
}

int
get_cpuid(char *buffer, size_t sz)
{
	return __get_cpuid(buffer, sz, "%s,%u,%u,%u$");
}

char *
get_cpuid_str(struct perf_pmu *pmu __maybe_unused)
{
	char *buf = malloc(128);

	if (buf && __get_cpuid(buf, 128, "%s-%u-%X$") < 0) {
		free(buf);
		return NULL;
	}
	return buf;
}
