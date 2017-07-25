/*
 * Userland implementation of gettimeofday() for 64 bits processes in a
 * ppc64 kernel for use in the vDSO
 *
 * Copyright (C) 2017 Santosh Sivaraj (santosh@fossix.org), IBM.
 *
 * Originally implemented in assembly by:
 *   Benjamin Herrenschmuidt (benh@kernel.crashing.org),
 *                    IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/compiler.h>
#include <linux/types.h>
#include <asm/ppc_asm.h>
#include <asm/vdso.h>
#include <asm/vdso_datapage.h>
#include <asm/time.h>

static notrace void kernel_get_tspec(struct timespec *tp,
				     struct vdso_data *vdata, u32 *wtom_sec,
				     u32 *wtom_nsec)
{
	u64 tb;
	u32 update_count;

	do {
		/* check for update count & load values */
		update_count = vdata->tb_update_count;

		/* Get TB, offset it and scale result */
		tb = mulhdu((get_tb() - vdata->tb_orig_stamp) << 12,
			    vdata->tb_to_xs) + vdata->stamp_sec_fraction;
		tp->tv_sec = vdata->stamp_xtime.tv_sec;
		if (wtom_sec)
			*wtom_sec = vdata->wtom_clock_sec;
		if (wtom_nsec)
			*wtom_nsec = vdata->wtom_clock_nsec;
	} while (update_count != vdata->tb_update_count);

	tp->tv_nsec = ((u64)mulhwu(tb, NSEC_PER_SEC) << 32) >> 32;
	tp->tv_sec += (tb >> 32);
}

static notrace int clock_get_realtime(struct timespec *tp,
				      struct vdso_data *vdata)
{
	kernel_get_tspec(tp, vdata, NULL, NULL);

	return 0;
}

static notrace int clock_get_monotonic(struct timespec *tp,
				       struct vdso_data *vdata)
{
	__s32 wtom_sec, wtom_nsec;
	u64 nsec;

	kernel_get_tspec(tp, vdata, &wtom_sec, &wtom_nsec);

	tp->tv_sec += wtom_sec;

	nsec = tp->tv_nsec;
	tp->tv_nsec = 0;
	timespec_add_ns(tp, nsec + wtom_nsec);

	return 0;
}

static notrace int clock_realtime_coarse(struct timespec *tp,
					 struct vdso_data *vdata)
{
	u32 update_count;

	do {
		/* check for update count & load values */
		update_count = vdata->tb_update_count;

		tp->tv_sec = vdata->stamp_xtime.tv_sec;
		tp->tv_nsec = vdata->stamp_xtime.tv_nsec;
	} while (update_count != vdata->tb_update_count);

	return 0;
}

static notrace int clock_monotonic_coarse(struct timespec *tp,
					  struct vdso_data *vdata)
{
	__s32 wtom_sec, wtom_nsec;
	u64 nsec;
	u32 update_count;

	do {
		/* check for update count & load values */
		update_count = vdata->tb_update_count;

		tp->tv_sec = vdata->stamp_xtime.tv_sec;
		tp->tv_nsec = vdata->stamp_xtime.tv_nsec;
		wtom_sec = vdata->wtom_clock_sec;
		wtom_nsec = vdata->wtom_clock_nsec;
	} while (update_count != vdata->tb_update_count);

	tp->tv_sec += wtom_sec;
	nsec = tp->tv_nsec;
	tp->tv_nsec = 0;
	timespec_add_ns(tp, nsec + wtom_nsec);

	return 0;
}

notrace int kernel_clock_gettime(clockid_t clk_id, struct timespec *tp)
{
	int ret;
	struct vdso_data *vdata = __get_datapage();

	if (!tp || !vdata)
		return -EBADR;

	switch (clk_id) {
	case CLOCK_REALTIME:
		ret = clock_get_realtime(tp, vdata);
		break;
	case CLOCK_MONOTONIC:
		ret = clock_get_monotonic(tp, vdata);
		break;
	case CLOCK_REALTIME_COARSE:
		ret = clock_realtime_coarse(tp, vdata);
		break;
	case CLOCK_MONOTONIC_COARSE:
		ret = clock_monotonic_coarse(tp, vdata);
		break;
	default:
		/* fallback to syscall */
		ret = -1;
		break;
	}

	return ret;
}
