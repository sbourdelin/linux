/*
 * Adaptive moderation support for I/O devices.
 * Copyright (c) 2018 Lightbits Labs.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */
#ifndef _IRQ_AM_H
#define _IRQ_AM_H

#include <linux/ktime.h>
#include <linux/workqueue.h>

struct irq_am;
typedef int (irq_am_fn)(struct irq_am *, unsigned short level);

/*
 * struct irq_am_sample_stats - sample stats for adpative moderation
 * @cps:        completions per-second
 * @eps:        events per-second
 * @cpe:	completions per event
 */
struct irq_am_sample_stats {
	u32 cps;
	u32 eps;
	u32 cpe;
};

/*
 * struct irq_am_sample - per-irq interrupt batch sample unit
 * @time:         current time
 * @comps:     completions count since last sample
 * @events:    events count since the last sample
 */
struct irq_am_sample {
	ktime_t	time;
	u64	comps;
	u64	events;
};

/*
 * enum irq_am_state - adaptive moderation monitor states
 * @IRQ_AM_START_MEASURING:        collect first sample (start_sample)
 * @IRQ_AM_MEASURING:              measurement in progress
 * @IRQ_AM_PROGRAM_MODERATION:     moderatio program scheduled
 *                                 so we should not react to any stats
 *                                 from the old moderation profile.
 */
enum irq_am_state {
	IRQ_AM_START_MEASURING,
	IRQ_AM_MEASURING,
	IRQ_AM_PROGRAM_MODERATION,
};

enum irq_am_tune_state {
	IRQ_AM_GOING_UP,
	IRQ_AM_GOING_DOWN,
};

enum irq_am_relative_diff {
	IRQ_AM_STATS_WORSE,
	IRQ_AM_STATS_SAME,
	IRQ_AM_STATS_BETTER,
};

struct irq_am_stats {
	u64	events;
	u64	comps;
};

/*
 * struct irq_am - irq adaptive moderation monitor
 * @state:             adaptive moderation monitor state
 * @tune_state:        tuning state of the moderation monitor
 * @am_stats:          overall completions and events counters
 * @start_sample:      first sample in moderation batch
 * @prev_stats:        previous stats for trend detection
 * @nr_events:         number of events between samples
 * @nr_levels:         number of moderation levels
 * @curr_level:        current moderation level
 * @work:              schedule moderation program
 * @program:           moderation program handler
 */
struct irq_am {
	enum irq_am_state		state;
	enum irq_am_tune_state		tune_state;

	struct irq_am_stats		am_stats;
	struct irq_am_sample		start_sample;
	struct irq_am_sample_stats	prev_stats;

	u16				nr_events;
	unsigned short			nr_levels;
	unsigned short			curr_level;

	struct work_struct		work;
	irq_am_fn			*program;
};

void irq_am_add_event(struct irq_am *am);
static inline void irq_am_add_comps(struct irq_am *am, u64 n)
{
	am->am_stats.comps += n;
}

void irq_am_cleanup(struct irq_am *am);
void irq_am_init(struct irq_am *am, unsigned int nr_events,
	unsigned short nr_levels, unsigned short start_level, irq_am_fn *fn);

#endif
