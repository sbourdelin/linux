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
#include <linux/debugfs.h>

static DEFINE_IDA(am_ida);

#ifdef CONFIG_DEBUG_FS
struct dentry *irq_am_debugfs_root;

struct irq_am_debugfs_attr {
	const char *name;
	umode_t mode;
	int (*show)(void *, struct seq_file *);
	ssize_t (*write)(void *, const char __user *, size_t, loff_t *);
};

static int irq_am_tune_state_show(void *data, struct seq_file *m)
{
	struct irq_am *am = data;

	seq_printf(m, "%d\n", am->tune_state);
	return 0;
}

static int irq_am_curr_level_show(void *data, struct seq_file *m)
{
	struct irq_am *am = data;

	seq_printf(m, "%d\n", am->curr_level);
	return 0;
}

static int irq_am_debugfs_show(struct seq_file *m, void *v)
{
	const struct irq_am_debugfs_attr *attr = m->private;
	void *data = d_inode(m->file->f_path.dentry->d_parent)->i_private;

	return attr->show(data, m);
}

static int irq_am_debugfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, irq_am_debugfs_show, inode->i_private);
}

static const struct file_operations irq_am_debugfs_fops = {
	.open		= irq_am_debugfs_open,
	.read		= seq_read,
	.release	= seq_release,
};

static const struct irq_am_debugfs_attr irq_am_attrs[] = {
	{"tune_state", 0400, irq_am_tune_state_show},
	{"curr_level", 0400, irq_am_curr_level_show},
	{},
};
#endif

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

#ifdef CONFIG_DEBUG_FS
static bool debugfs_create_files(struct dentry *parent, void *data,
				 const struct irq_am_debugfs_attr *attr)
{
	d_inode(parent)->i_private = data;

	for (; attr->name; attr++) {
		if (!debugfs_create_file(attr->name, attr->mode, parent,
					 (void *)attr, &irq_am_debugfs_fops))
			return false;
	}
	return true;
}

static int irq_am_register_debugfs(struct irq_am *am)
{
	char name[20];

	snprintf(name, sizeof(name), "am%u", am->id);
	am->debugfs_dir = debugfs_create_dir(name,
				irq_am_debugfs_root);
	if (!am->debugfs_dir)
		return -ENOMEM;

	if (!debugfs_create_files(am->debugfs_dir, am,
				irq_am_attrs))
		return -ENOMEM;
	return 0;
}

static void irq_am_deregister_debugfs(struct irq_am *am)
{
	debugfs_remove_recursive(am->debugfs_dir);
}

#else
static int irq_am_register_debugfs(struct irq_am *am) {}
static void irq_am_deregister_debugfs(struct irq_am *am) {}
#endif

void irq_am_cleanup(struct irq_am *am)
{
	flush_work(&am->work);
	irq_am_deregister_debugfs(am);
	ida_simple_remove(&am_ida, am->id);
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
	am->id = ida_simple_get(&am_ida, 0, 0, GFP_KERNEL);
	WARN_ON(am->id < 0);
	INIT_WORK(&am->work, irq_am_program_moderation_work);
	if (irq_am_register_debugfs(am))
		pr_warn("irq-am %d failed to register debugfs\n", am->id);
}
EXPORT_SYMBOL_GPL(irq_am_init);

static __init int irq_am_setup(void)
{
#ifdef CONFIG_DEBUG_FS
	irq_am_debugfs_root = debugfs_create_dir("irq_am", NULL);
#endif
	return 0;
}
subsys_initcall(irq_am_setup);
