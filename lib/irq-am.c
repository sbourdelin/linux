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
#include <linux/irq-am.h>

static void irq_am_try_step(struct irq_am *am)
{
	if (am->tune_state == IRQ_AM_GOING_UP &&
	    am->curr_level != am->nr_levels - 1) {
		am->curr_level++;
	} else if (am->tune_state == IRQ_AM_GOING_DOWN &&
		   am->curr_level != 0) {
		am->curr_level--;
	}
}

static inline bool irq_am_on_edge(struct irq_am *am)
{
	return am->curr_level == 0 || am->curr_level == am->nr_levels - 1;
}

static void irq_am_turn(struct irq_am *am)
{
	am->tune_state = am->tune_state == IRQ_AM_GOING_UP ?
		IRQ_AM_GOING_DOWN : IRQ_AM_GOING_UP;
	irq_am_try_step(am);
}

#define IRQ_AM_SIGNIFICANT_DIFF(val, ref) \
	(((100 * abs((val) - (ref))) / (ref)) > 20) /* more than 20% difference */

static int irq_am_stats_compare(struct irq_am *am, struct irq_am_sample_stats *curr)
{
	struct irq_am_sample_stats *prev = &am->prev_stats;

	/* first stat */
	if (!prev->cps)
		return IRQ_AM_STATS_SAME;

	/* more completions per second is better */
	if (IRQ_AM_SIGNIFICANT_DIFF(curr->cps, prev->cps))
		return (curr->cps > prev->cps) ? IRQ_AM_STATS_BETTER :
						 IRQ_AM_STATS_WORSE;

	/* less events per second is better */
	if (IRQ_AM_SIGNIFICANT_DIFF(curr->eps, prev->eps))
		return (curr->eps < prev->eps) ? IRQ_AM_STATS_BETTER :
						 IRQ_AM_STATS_WORSE;

	/*
	 * we get 1 completion per event, no point in trying to aggregate
	 * any further, start declining moderation
	 */
	if (curr->cpe == 1 && am->curr_level)
		return am->tune_state == IRQ_AM_GOING_UP ?
			IRQ_AM_STATS_WORSE : IRQ_AM_STATS_BETTER;

	return IRQ_AM_STATS_SAME;
}

static bool irq_am_decision(struct irq_am *am,
		struct irq_am_sample_stats *curr_stats)
{
	unsigned short prev_level = am->curr_level;
	enum irq_am_relative_diff diff;
	bool changed;

	diff = irq_am_stats_compare(am, curr_stats);
	switch (diff) {
	default:
	case IRQ_AM_STATS_SAME:
		/* fall through */
		break;
	case IRQ_AM_STATS_WORSE:
		irq_am_turn(am);
		break;
	case IRQ_AM_STATS_BETTER:
		irq_am_try_step(am);
		break;
	}

	changed = am->curr_level != prev_level || irq_am_on_edge(am);
	if (changed || !am->prev_stats.cps)
		am->prev_stats = *curr_stats;

	return changed;
}

static void irq_am_sample(struct irq_am *am, struct irq_am_sample *s)
{
	s->time = ktime_get();
	s->events = am->am_stats.events;
	s->comps = am->am_stats.comps;
}

static void irq_am_calc_stats(struct irq_am *am, struct irq_am_sample *start,
		struct irq_am_sample *end,
		struct irq_am_sample_stats *curr_stats)
{
	/* u32 holds up to 71 minutes, should be enough */
	u32 delta_us = ktime_us_delta(end->time, start->time);
	u32 ncomps = end->comps - start->comps;

	if (!delta_us)
		return;

	curr_stats->cps = DIV_ROUND_UP(ncomps * USEC_PER_SEC, delta_us);
	curr_stats->eps = DIV_ROUND_UP(am->nr_events * USEC_PER_SEC, delta_us);
	curr_stats->cpe = DIV_ROUND_UP(ncomps, am->nr_events);
}

void irq_am_add_event(struct irq_am *am)
{
	struct irq_am_sample end_sample;
	struct irq_am_sample_stats curr_stats;
	u16 nr_events;

	am->am_stats.events++;

	switch (am->state) {
	case IRQ_AM_MEASURING:
		nr_events = am->am_stats.events - am->start_sample.events;
		if (nr_events < am->nr_events)
			break;

		irq_am_sample(am, &end_sample);
		irq_am_calc_stats(am, &am->start_sample, &end_sample,
				    &curr_stats);
		if (irq_am_decision(am, &curr_stats)) {
			am->state = IRQ_AM_PROGRAM_MODERATION;
			schedule_work(&am->work);
			break;
		}
		/* fall through */
	case IRQ_AM_START_MEASURING:
		irq_am_sample(am, &am->start_sample);
		am->state = IRQ_AM_MEASURING;
		break;
	case IRQ_AM_PROGRAM_MODERATION:
		break;
	}
}
EXPORT_SYMBOL_GPL(irq_am_add_event);

static void irq_am_program_moderation_work(struct work_struct *w)
{
	struct irq_am *am = container_of(w, struct irq_am, work);

	WARN_ON_ONCE(am->program(am, am->curr_level));
	am->state = IRQ_AM_START_MEASURING;
}


void irq_am_cleanup(struct irq_am *am)
{
	flush_work(&am->work);
}
EXPORT_SYMBOL_GPL(irq_am_cleanup);

void irq_am_init(struct irq_am *am, unsigned int nr_events,
	unsigned short nr_levels, unsigned short start_level, irq_am_fn *fn)
{
	memset(am, 0, sizeof(*am));
	am->state = IRQ_AM_START_MEASURING;
	am->tune_state = IRQ_AM_GOING_UP;
	am->nr_levels = nr_levels;
	am->nr_events = nr_events;
	am->curr_level = start_level;
	am->program = fn;
	INIT_WORK(&am->work, irq_am_program_moderation_work);
}
EXPORT_SYMBOL_GPL(irq_am_init);
