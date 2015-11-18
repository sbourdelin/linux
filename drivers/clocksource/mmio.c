/*
 * Generic MMIO clocksource support
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/clocksource.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>

cycle_t clocksource_mmio_readl_up(struct clocksource *c)
{
	return (cycle_t)readl_relaxed(c->reg);
}

cycle_t clocksource_mmio_readl_down(struct clocksource *c)
{
	return ~(cycle_t)readl_relaxed(c->reg) & c->mask;
}

cycle_t clocksource_mmio_readw_up(struct clocksource *c)
{
	return (cycle_t)readw_relaxed(c->reg);
}

cycle_t clocksource_mmio_readw_down(struct clocksource *c)
{
	return ~(cycle_t)readw_relaxed(c->reg) & c->mask;
}

/**
 * clocksource_mmio_init - Initialize a simple mmio based clocksource
 * @base:	Virtual address of the clock readout register
 * @name:	Name of the clocksource
 * @hz:		Frequency of the clocksource in Hz
 * @rating:	Rating of the clocksource
 * @bits:	Number of valid bits
 * @read:	One of clocksource_mmio_read*() above
 */
int __init clocksource_mmio_init(void __iomem *base, const char *name,
	unsigned long hz, int rating, unsigned bits,
	cycle_t (*read)(struct clocksource *))
{
	struct clocksource *cs;

	if (bits > 32 || bits < 16)
		return -EINVAL;

	cs = kzalloc(sizeof *cs, GFP_KERNEL);
	if (!cs)
		return -ENOMEM;

	cs->read = read;
	cs->reg  = base;
	cs->name = name;
	cs->rating = rating;
	cs->mask = CLOCKSOURCE_MASK(bits);
	cs->flags = CLOCK_SOURCE_IS_CONTINUOUS;

	return clocksource_register_hz(cs, hz);
}
