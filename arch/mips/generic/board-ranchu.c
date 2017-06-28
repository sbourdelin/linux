/*
 * Copyright (C) 2017 Imagination Technologies Ltd.
 * Author: Miodrag Dinic <miodrag.dinic@imgtec.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <asm/machine.h>
#include <asm/time.h>

#define GOLDFISH_TIMER_LOW		0x00
#define GOLDFISH_TIMER_HIGH		0x04
#define GOLDFISH_TIMER_BASE		0x1f005000

static __init uint64_t read_rtc_time(void __iomem *base)
{
	uint64_t time_low;
	uint64_t time_high;
	uint64_t time_high_prev;

	time_high = readl(base + GOLDFISH_TIMER_HIGH);
	do {
		time_high_prev = time_high;
		time_low = readl(base + GOLDFISH_TIMER_LOW);
		time_high = readl(base + GOLDFISH_TIMER_HIGH);
	} while (time_high != time_high_prev);

	return ((int64_t)time_high << 32) | time_low;
}

static __init unsigned int ranchu_measure_hpt_freq(void)
{
	uint64_t rtc_start, rtc_current, rtc_delta;
	unsigned int start, count;
	unsigned int prid = read_c0_prid() & 0xffff00;
	void __iomem *rtc_base = ioremap(GOLDFISH_TIMER_BASE, 0x1000);

	if (!rtc_base)
		panic("%s(): Failed to ioremap Goldfish timer base %p!",
			__func__, (void *)GOLDFISH_TIMER_BASE);

	/*
	 * poll the nanosecond resolution RTC for 1 second
	 * to calibrate the CPU frequency
	 */

	rtc_start = read_rtc_time(rtc_base);
	start = read_c0_count();

	do {
		rtc_current = read_rtc_time(rtc_base);
		rtc_delta = rtc_current - rtc_start;
	} while (rtc_delta < NSEC_PER_SEC);

	count = read_c0_count() - start;

	mips_hpt_frequency = count;
	if ((prid != (PRID_COMP_MIPS | PRID_IMP_20KC)) &&
		(prid != (PRID_COMP_MIPS | PRID_IMP_25KF)))
		count *= 2;

	count += 5000;	/* round */
	count -= count%10000;

	return count;
}

static const struct of_device_id ranchu_of_match[];

MIPS_MACHINE(ranchu) = {
	.matches = ranchu_of_match,
	.measure_hpt_freq = ranchu_measure_hpt_freq,
};

static const struct of_device_id ranchu_of_match[] = {
	{
		.compatible = "mti,ranchu",
		.data = &__mips_mach_ranchu,
	},
};
