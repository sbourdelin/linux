#ifndef _UAPI_LINUX_TIME_H
#define _UAPI_LINUX_TIME_H

#include <linux/libc-compat.h>
#include <linux/types.h>


#if __UAPI_DEF_TIMESPEC
#ifndef __timespec_defined
#define __timespec_defined	1
#endif
#ifndef _STRUCT_TIMESPEC
#define _STRUCT_TIMESPEC	1
#endif
struct timespec {
	__kernel_time_t	tv_sec;			/* seconds */
	long		tv_nsec;		/* nanoseconds */
};
#endif /* __UAPI_DEF_TIMESPEC */

#if __UAPI_DEF_TIMEVAL
#ifndef __timeval_defined
#define __timeval_defined	1
#endif
#ifndef _STRUCT_TIMEVAL
#define _STRUCT_TIMEVAL		1
#endif
struct timeval {
	__kernel_time_t		tv_sec;		/* seconds */
	__kernel_suseconds_t	tv_usec;	/* microseconds */
};
#endif /* __UAPI_DEF_TIMEVAL */

#if __UAPI_DEF_TIMEZONE
struct timezone {
	int	tz_minuteswest;	/* minutes west of Greenwich */
	int	tz_dsttime;	/* type of dst correction */
};
#endif /* __UAPI_DEF_TIMEZONE */


/*
 * Names of the interval timers, and structure
 * defining a timer setting:
 */
#if __UAPI_DEF_ITIMER_REAL_VIRTUAL_PROF
#define	ITIMER_REAL		0
#define	ITIMER_VIRTUAL		1
#define	ITIMER_PROF		2
#endif /* __UAPI_DEF_ITIMER_REAL_VIRTUAL_PROF */

#if __UAPI_DEF_ITIMERSPEC
#ifndef __itimerspec_defined
#define __itimerspec_defined	1
#endif
struct itimerspec {
	struct timespec it_interval;	/* timer period */
	struct timespec it_value;	/* timer expiration */
};
#endif /* __UAPI_DEF_ITIMERSPEC */

#if __UAPI_DEF_ITIMERVAL
struct itimerval {
	struct timeval it_interval;	/* timer interval */
	struct timeval it_value;	/* current value */
};
#endif /* __UAPI_DEF_ITIMERVAL */

/*
 * The IDs of the various system clocks (for POSIX.1b interval timers):
 */
#define CLOCK_REALTIME			0
#define CLOCK_MONOTONIC			1
#define CLOCK_PROCESS_CPUTIME_ID	2
#define CLOCK_THREAD_CPUTIME_ID		3
#define CLOCK_MONOTONIC_RAW		4
#define CLOCK_REALTIME_COARSE		5
#define CLOCK_MONOTONIC_COARSE		6
#define CLOCK_BOOTTIME			7
#define CLOCK_REALTIME_ALARM		8
#define CLOCK_BOOTTIME_ALARM		9
#define CLOCK_SGI_CYCLE			10	/* Hardware specific */
#define CLOCK_TAI			11

#define MAX_CLOCKS			16
#define CLOCKS_MASK			(CLOCK_REALTIME | CLOCK_MONOTONIC)
#define CLOCKS_MONO			CLOCK_MONOTONIC

/*
 * The various flags for setting POSIX.1b interval timers:
 */
#if __UAPI_DEF_TIMER_ABSTIME
#define TIMER_ABSTIME			0x01
#endif

#endif /* _UAPI_LINUX_TIME_H */
