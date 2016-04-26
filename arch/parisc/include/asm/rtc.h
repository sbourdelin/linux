/* 
 * include/asm-parisc/rtc.h
 *
 * Copyright 2002 Randolph CHung <tausq@debian.org>
 *
 * Based on: include/asm-ppc/rtc.h and the genrtc driver in the
 * 2.4 parisc linux tree
 */

#ifndef __ASM_RTC_H__
#define __ASM_RTC_H__

#ifdef __KERNEL__

#define get_rtc_time parisc_get_rtc_time
#define set_rtc_time parisc_set_rtc_time
#include <asm-generic/rtc.h>

#include <asm/pdc.h>

#define SECS_PER_HOUR   (60 * 60)
#define SECS_PER_DAY    (SECS_PER_HOUR * 24)

# define __isleap(year) \
  ((year) % 4 == 0 && ((year) % 100 != 0 || (year) % 400 == 0))

/* How many days come before each month (0-12).  */
static const unsigned short int __mon_yday[2][13] =
{
	/* Normal years.  */
	{ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
	/* Leap years.  */
	{ 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 }
};

static inline unsigned int parisc_get_rtc_time(struct rtc_time *wtime)
{
	struct pdc_tod tod_data;
	long int days, rem, y;
	const unsigned short int *ip;

	memset(wtime, 0, sizeof(*wtime));
	if (pdc_tod_read(&tod_data) < 0)
		return RTC_24H | RTC_BATT_BAD;

	// most of the remainder of this function is:
//	Copyright (C) 1991, 1993, 1997, 1998 Free Software Foundation, Inc.
//	This was originally a part of the GNU C Library.
//      It is distributed under the GPL, and was swiped from offtime.c


	days = tod_data.tod_sec / SECS_PER_DAY;
	rem = tod_data.tod_sec % SECS_PER_DAY;

	wtime->tm_hour = rem / SECS_PER_HOUR;
	rem %= SECS_PER_HOUR;
	wtime->tm_min = rem / 60;
	wtime->tm_sec = rem % 60;

	y = 1970;

#define DIV(a, b) ((a) / (b) - ((a) % (b) < 0))
#define LEAPS_THRU_END_OF(y) (DIV (y, 4) - DIV (y, 100) + DIV (y, 400))

	while (days < 0 || days >= (__isleap (y) ? 366 : 365))
	{
		/* Guess a corrected year, assuming 365 days per year.  */
		long int yg = y + days / 365 - (days % 365 < 0);

		/* Adjust DAYS and Y to match the guessed year.  */
		days -= ((yg - y) * 365
			 + LEAPS_THRU_END_OF (yg - 1)
			 - LEAPS_THRU_END_OF (y - 1));
		y = yg;
	}
	wtime->tm_year = y - 1900;

	ip = __mon_yday[__isleap(y)];
	for (y = 11; days < (long int) ip[y]; --y)
		continue;
	days -= ip[y];
	wtime->tm_mon = y;
	wtime->tm_mday = days + 1;

	return RTC_24H;
}

static int parisc_set_rtc_time(struct rtc_time *wtime)
{
	u_int32_t secs;

	secs = mktime(wtime->tm_year + 1900, wtime->tm_mon + 1, wtime->tm_mday, 
		      wtime->tm_hour, wtime->tm_min, wtime->tm_sec);

	if(pdc_tod_set(secs, 0) < 0)
		return -1;
	else
		return 0;

}

#endif /* __KERNEL__ */
#endif /* __ASM_RTC_H__ */
