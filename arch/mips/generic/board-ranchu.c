/*
 * Copyright (C) 2017 Imagination Technologies Ltd.
 * Author: Miodrag Dinic <miodrag.dinic@imgtec.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/of_address.h>

#include <asm/machine.h>
#include <asm/time.h>

#define GOLDFISH_TIMER_LOW		0x00
#define GOLDFISH_TIMER_HIGH		0x04

static __init uint64_t read_rtc_time(void __iomem *base)
{
	uint64_t time_low;
	uint64_t time_high;

	time_low = readl(base + GOLDFISH_TIMER_LOW);
	time_high = readl(base + GOLDFISH_TIMER_HIGH);

	return (time_high << 32) | time_low;
}

static __init unsigned int ranchu_measure_hpt_freq(void)
{
	uint64_t rtc_start, rtc_current, rtc_delta;
	unsigned int start, count;
	struct device_node *np;
	void __iomem *rtc_base;

	if (!(np = of_find_compatible_node(NULL, NULL, "google,goldfish-rtc")))
		panic("%s(): Failed to find 'google,goldfish-rtc' dt node!", __func__);

	rtc_base = of_iomap(np, 0);
	if (!rtc_base)
		panic("%s(): Failed to ioremap Goldfish RTC base!", __func__);

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

	count += 5000;	/* round */
	count -= count % 10000;

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
