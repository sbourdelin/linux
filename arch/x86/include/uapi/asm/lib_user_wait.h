/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This library provides a set of APIs for user or kernel to use
 * some new user wait instructions:
 * - tpause, umonitor, and umwait
 *
 * Detailed information on the instructions can be found in
 * Intel Architecture Instruction Set Extensions and Future Features
 * Programming Reference.
 */

#ifndef _ASM_X86_LIB_USER_WAIT_H
#define _ASM_X86_LIB_USER_WAIT_H

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>

/* CPUID.07H.0H:ECX[5] */
#define WAITPKG_BIT		5

static bool _waitpkg_supported, _waitpkg_enumerated;
static unsigned long tsc_khz;

/**
 * waitpkg_supported() - Is CPU flag waitpkg supported?
 *
 * Return:
 * true: supported
 *
 * false: not supported
 */
static inline int waitpkg_supported(void)
{
	int eax, ebx, ecx, edx, ret;

	/*
	 * If waitpkg has been enumerated before, return cached waitpkg
	 * support info.
	 */
	if (_waitpkg_enumerated)
		return _waitpkg_supported;

	/* Otherwise, enumerate the feature from CPUID. */
	asm volatile("mov $7, %%eax\t\n"
		     "mov $0, %%ecx\t\n"
		     "cpuid\t\n"
		     : "=a"(eax), "=b" (ebx), "=c" (ecx), "=d" (edx));

	if (ecx & (1 << WAITPKG_BIT))
		ret = true;
	else
		ret = false;

	/* Cache waitpkg support for future use. */
	_waitpkg_enumerated = true;
	_waitpkg_supported = true;

	return ret;
}

static inline int get_tsc_khz(unsigned long *tsc_khz_val)
{
	int fd, ret = 0;
	char buf[32];

	if (tsc_khz != 0) {
		*tsc_khz_val = tsc_khz;
		return 0;
	}

	fd = open("/sys/devices/system/cpu/user_wait/tsc_khz", O_RDONLY);
	if (!fd)
		return -1;
	ret = read(fd, buf, 32);
	if (ret < 0)
		goto out;

	tsc_khz = atol(buf);
	*tsc_khz_val = tsc_khz;
printf("tsc_khz=%ld\n", tsc_khz);

out:
	close(fd);
	return ret;
}

#define	USEC_PER_SEC	1000000

static inline int nsec_to_tsc(unsigned long nsec, unsigned long *tsc)
{
	int ret;

	/* Get tsc frequency in HZ */
	ret = get_tsc_khz(&tsc_khz);
	if (ret < 0)
		return ret;

	*tsc = (unsigned long)round((double)tsc_khz * nsec / USEC_PER_SEC);

	return 0;
}

/**
 * umonitor() - Set up monitoring address
 * @addr: Monitored address
 *
 * This API sets up address monitoring hardware using address @addr.
 * It can be executed at any privilege level.
 */
static inline void umonitor(void *addr)
{
	asm volatile("mov %0, %%rdi\t\n"
		     ".byte 0xf3, 0x0f, 0xae, 0xf7\t\n"
		     : : "r" (addr));
}

static inline int _umwait(int state, unsigned long eax, unsigned long edx)
{
	unsigned long cflags;

	asm volatile("mov %3, %%edi\t\n"
		     ".byte 0xf2, 0x0f, 0xae, 0xf7\t\n"
		     "pushf\t\n"
		     "pop %0\t\n"
		     : "=r" (cflags)
		     : "d" (edx), "a" (eax), "r"(state));

	/*
	 * If the processor wakes due to expiration of OS time-limit, the CF
	 * flag is set. Otherwise, the flag is cleared.
	 */
	return cflags & 1;
}

static unsigned long rdtsc(void)
{
	unsigned int low, high;

	asm volatile ("rdtsc\t\n"
		      : "=a" (low), "=d" (high));

	return (unsigned long)high << 32 | low;
}

/**
 * umwait() - Monitor wait
 * @state: State
 * @nsec: Time out in nano seconds
 *
 * A hint that allows the processor to stop instruction execution and
 * enter an implementation-dependent optimized state. The processor
 * wakes up because of events such as store to the monitored address,
 * timeout, NMI, SMI, machine check, debug exception, etc.
 *
 * State 0 is light-weight power optimized state. It allows the processor
 * to enter C0.2 state which has larger power saving but slower wakeup time.
 *
 * State 1 is performance optimized state. It allows the processor
 * to enter C0.1 state which has smaller power saving but faster wakeup time.
 *
 * This function can be executed at any privilege level.
 *
 * Return:
 * 1: the processor wakes due to expiration of OS time-limit
 *
 * 0: the processor wakes due to other reasons
 *
 * less than 0: error
 */
static inline int umwait(int state, unsigned long nsec)
{
	unsigned long tsc;
	int ret;

	if (state != 0 && state != 1)
		return -1;

	ret = nsec_to_tsc(nsec, &tsc);
	if (ret)
		return ret;

	/* Get umwait deadline */
	tsc += rdtsc();
	ret = _umwait(state, tsc & 0xffffffff, tsc >> 32);

	return ret;
}

static inline int _tpause(int state, unsigned long eax, unsigned long edx)
{
	unsigned long cflags;

	asm volatile("mov %3, %%edi\t\n"
		     ".byte 0x66, 0x0f, 0xae, 0xf7\t\n"
		     "pushf\t\n"
		     "pop %0\t\n"
		     : "=r" (cflags)
		     : "d" (edx), "a" (eax), "r"(state));

	/*
	 * If the processor wakes due to expiration of OS time-limit, the CF
	 * flag is set. Otherwise, the flag is cleared.
	 */
	return cflags & 1;
}

/**
 * tpause() - Timed pause
 * @state: State
 * @nsec: Timeout in nano seconds
 *
 * tpause() allows the processor to stop instruction execution and
 * enter an implementation-dependent optimized state. The processor
 * wakes up because of events such as store to the monitored
 * address, timeout, NMI, SMI, machine check, debug exception, etc.
 *
 * State 0 is light-weight power optimized state. It allows the processor
 * to enter C0.2 state which has larger power saving but slower wakeup time.
 *
 * State 1 is performance optimized state. It allows the processor
 * to enter C0.1 state which has smaller power saving but faster wakeup time.
 *
 * This function can be executed at any privilege level.
 *
 * Return:
 * 1: the processor wakes due to expiration of OS time-limit
 *
 * 0: the processor wakes due to other reasons
 *
 * less than 0: error
 */
static inline int tpause(int state, unsigned long nsec)
{
	unsigned long tsc;
	int ret;

	if (state != 0 && state != 1)
		return -1;

	ret = nsec_to_tsc(nsec, &tsc);
	if (ret)
		return ret;

	/* Get tpause deadline */
	tsc += rdtsc();
	ret = _tpause(state, tsc & 0xffffffff, tsc >> 32);

	return ret;
}

#endif /* _ASM_X86_LIB_USER_WAIT_H */
