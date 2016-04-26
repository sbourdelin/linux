/*
 * Real-time clock definitions and interfaces
 *
 * Author: Tom Rini <trini@mvista.com>
 *
 * 2002 (c) MontaVista, Software, Inc.  This file is licensed under
 * the terms of the GNU General Public License version 2.  This program
 * is licensed "as is" without any warranty of any kind, whether express
 * or implied.
 *
 * Based on:
 * include/asm-m68k/rtc.h
 *
 * Copyright Richard Zidlicky
 * implementation details for genrtc/q40rtc driver
 *
 * And the old drivers/macintosh/rtc.c which was heavily based on:
 * Linux/SPARC Real Time Clock Driver
 * Copyright (C) 1996 Thomas K. Dyas (tdyas@eden.rutgers.edu)
 *
 * With additional work by Paul Mackerras and Franz Sirl.
 */

#ifndef __ASM_POWERPC_RTC_H__
#define __ASM_POWERPC_RTC_H__

#ifdef __KERNEL__

#define get_rtc_time powerpc_get_rtc_time
#define set_rtc_time powerpc_set_rtc_time
#include <asm-generic/rtc.h>

#include <asm/machdep.h>
#include <asm/time.h>

static inline unsigned int powerpc_get_rtc_time(struct rtc_time *time)
{
	if (ppc_md.get_rtc_time)
		ppc_md.get_rtc_time(time);
	return RTC_24H;
}

/* Set the current date and time in the real time clock. */
static inline int powerpc_set_rtc_time(struct rtc_time *time)
{
	if (ppc_md.set_rtc_time)
		return ppc_md.set_rtc_time(time);
	return -EINVAL;
}

#endif /* __KERNEL__ */
#endif /* __ASM_POWERPC_RTC_H__ */
