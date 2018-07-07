/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_COMPAT_TIME_H
#define _LINUX_COMPAT_TIME_H

#include <linux/types.h>
#include <linux/time64.h>

typedef s32		compat_time_t;

/* TODO: Move to linux/compat.h when this file is deleted. */
#include <asm/compat.h>

struct compat_timespec {
	compat_time_t	tv_sec;
	s32		tv_nsec;
};

struct compat_timeval {
	compat_time_t	tv_sec;
	s32		tv_usec;
};

struct compat_itimerspec {
	struct compat_timespec it_interval;
	struct compat_timespec it_value;
};

struct compat_timex {
	compat_uint_t modes;
	compat_long_t offset;
	compat_long_t freq;
	compat_long_t maxerror;
	compat_long_t esterror;
	compat_int_t status;
	compat_long_t constant;
	compat_long_t precision;
	compat_long_t tolerance;
	struct compat_timeval time;
	compat_long_t tick;
	compat_long_t ppsfreq;
	compat_long_t jitter;
	compat_int_t shift;
	compat_long_t stabil;
	compat_long_t jitcnt;
	compat_long_t calcnt;
	compat_long_t errcnt;
	compat_long_t stbcnt;
	compat_int_t tai;

	compat_int_t:32; compat_int_t:32; compat_int_t:32; compat_int_t:32;
	compat_int_t:32; compat_int_t:32; compat_int_t:32; compat_int_t:32;
	compat_int_t:32; compat_int_t:32; compat_int_t:32;
};


extern int compat_get_timespec64(struct timespec64 *, const void __user *);
extern int compat_put_timespec64(const struct timespec64 *, void __user *);
extern int get_compat_itimerspec64(struct itimerspec64 *its,
			const struct compat_itimerspec __user *uits);
extern int put_compat_itimerspec64(const struct itimerspec64 *its,
			struct compat_itimerspec __user *uits);
struct timex;
int compat_get_timex(struct timex *, const struct compat_timex __user *);
int compat_put_timex(struct compat_timex __user *, const struct timex *);

#endif /* _LINUX_COMPAT_TIME_H */
