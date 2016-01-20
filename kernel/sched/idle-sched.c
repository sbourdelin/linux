/*
 *  Copyright (C) 2016 Linaro Ltd, Daniel Lezcano <daniel.lezcano@linaro.org>
 *                                 Nicolas Pitre <nicolas.pitre@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/cpuidle.h>
#include <linux/interrupt.h>
#include <linux/irqdesc.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/tick.h>
#include <linux/time64.h>

/*
 * Define the number of samples over which the average and variance
 * are computed. A power of 2 is preferred so to let the compiler
 * optimize divisions by that number with simple arithmetic shifts.
 */
#define STATS_NR_VALUES 4

/**
 * struct stats - internal structure to encapsulate stats informations
 *
 * @sum: sum of the values
 * @values: array of values to do stats on
 * @w_ptr: current buffer pointer
 */
struct stats {
	u64           sum;                     /* sum of values */
	u32           values[STATS_NR_VALUES]; /* array of values */
	unsigned char w_ptr;                   /* current window pointer */
};

/**
 * struct wakeup - internal structure describing a source of wakeup
 *
 * @stats: the stats structure on the different event intervals
 * @timestamp: latest update timestamp
 */
struct wakeup {
	struct stats stats;
	ktime_t timestamp;
};

/*
 * Per cpu and irq statistics. Each cpu receives interrupts and those
 * ones can be distributed following an irq chip specific
 * algorithm. Random irq distribution is the worst case to predict
 * interruption behavior but usually that does not happen or could be
 * fixed from userspace by setting the irq affinity.
 */
static DEFINE_PER_CPU(struct wakeup, *wakeups[NR_IRQS]);

static DECLARE_BITMAP(enabled_irq, NR_IRQS);

/**
 * stats_add - add a new value in the statistic structure
 *
 * @s: the statistic structure
 * @value: the new value to be added
 *
 * Adds the value to the array, if the array is full, the oldest value
 * is replaced.
 */
static void stats_add(struct stats *s, u32 value)
{
	/*
	 * This is a circular buffer, so the oldest value is the next
	 * one in the buffer. Let's compute the next pointer to
	 * retrieve the oldest value and re-use it to update the w_ptr
	 * after adding the new value.
	 */
	s->w_ptr = (s->w_ptr + 1) % STATS_NR_VALUES;

	/*
	 * Remove the oldest value from the summing. If this is the
	 * first time we go through this array slot, the previous
	 * value will be zero and we won't substract anything from the
	 * current sum. Hence this code relies on a zero-ed stat
	 * structure at init time via memset or kzalloc.
	 */
	s->sum -= s->values[s->w_ptr];
	s->values[s->w_ptr] = value;

	/*
	 * In order to reduce the overhead and to prevent value
	 * derivation due to the integer computation, we just sum the
	 * value and do the division when the average and the variance
	 * are requested.
	 */
	s->sum += value;
}

/**
 * stats_reset - reset the stats
 *
 * @s: the statistic structure
 *
 * Reset the statistics and reset the values
 */
static inline void stats_reset(struct stats *s)
{
	memset(s, 0, sizeof(*s));
}

/**
 * stats_mean - compute the average
 *
 * @s: the statistics structure
 *
 * Returns an u32 corresponding to the mean value, or zero if there is
 * no data
 */
static inline u32 stats_mean(struct stats *s)
{
	/*
	 * gcc is smart enough to convert to a bits shift when the
	 * divisor is constant and multiple of 2^x.
	 *
	 * The number of values could have not reached STATS_NR_VALUES
	 * yet, but we can consider it acceptable as the situation is
	 * only at the beginning of the burst of irqs.
	 */
	return s->sum / STATS_NR_VALUES;
}

/**
 * stats_variance - compute the variance
 *
 * @s: the statistic structure
 *
 * Returns an u64 corresponding to the variance, or zero if there is
 * no data
 */
static u64 stats_variance(struct stats *s, u32 mean)
{
	int i;
	u64 variance = 0;

	/*
	 * The variance is the sum of the squared difference to the
	 * average divided by the number of elements.
	 */
	for (i = 0; i < STATS_NR_VALUES; i++) {
		s64 diff = s->values[i] - mean;
		variance += (u64)diff * diff;
	}

	return variance / STATS_NR_VALUES;
}

/**
 * sched_idle_irq - irq timestamp callback
 *
 * @irq: the irq number
 * @timestamp: when the interrupt occured
 * @dev_id: device id for shared interrupt (not yet used)
 *
 * Interrupt callback called when an interrupt happens. This function
 * is critical as it is called under an interrupt section: minimum
 * operations as possible are done here:
 */
static void sched_irq_timing_handler(unsigned int irq, ktime_t timestamp, void *dev_id)
{
	u32 diff;
	unsigned int cpu = raw_smp_processor_id();
	struct wakeup *w = per_cpu(wakeups[irq], cpu);

	/*
	 * It is the first time the interrupt occurs of the series, we
	 * can't do any stats as we don't have an interval, just store
	 * the timestamp and exit.
	 */
	if (ktime_equal(w->timestamp, ktime_set(0, 0))) {
		w->timestamp = timestamp;
		return;
	}

	/*
	 * Microsec resolution is enough for our purpose.
	 */
	diff = ktime_us_delta(timestamp, w->timestamp);
	w->timestamp = timestamp;

	/*
	 * There is no point attempting predictions on interrupts more
	 * than ~1 second apart. This has no benefit for sleep state
	 * selection and increases the risk of overflowing our variance
	 * computation. Reset all stats in that case.
	 */
	if (diff > (1 << 20)) {
		stats_reset(&w->stats);
		return;
	}

	stats_add(&w->stats, diff);
}

static ktime_t next_irq_event(void)
{
	unsigned int irq, cpu = raw_smp_processor_id();
	ktime_t diff, next, min = ktime_set(KTIME_SEC_MAX, 0);
	ktime_t now = ktime_get();
	struct wakeup *w;
	u32 interval, mean;
	u64 variance;

	/*
	 * Lookup the interrupt array for this cpu and search for the
	 * earlier expected interruption.
	 */
	for (irq = 0; irq < NR_IRQS; irq = find_next_bit(enabled_irq, NR_IRQS, irq)) {

		w = per_cpu(wakeups[irq], cpu);

		/*
		 * The interrupt was not setup as a source of a wakeup
		 * or the wakeup source is not considered at this
		 * moment stable enough to do a prediction.
		 */
		if (!w)
			continue;

		/*
		 * No statistics available yet.
		 */
		if (ktime_equal(w->timestamp, ktime_set(0, 0)))
			continue;

		diff = ktime_sub(now, w->timestamp);

		/*
		 * There is no point attempting predictions on interrupts more
		 * than 1 second apart. This has no benefit for sleep state
		 * selection and increases the risk of overflowing our variance
		 * computation. Reset all stats in that case.
		 */
		if (unlikely(ktime_after(diff, ktime_set(1, 0)))) {
			stats_reset(&w->stats);
			continue;
		}

		/*
		 * If the mean value is null, just ignore this wakeup
		 * source.
		 */
		mean = stats_mean(&w->stats);
		if (!mean)
			continue;

		variance = stats_variance(&w->stats, mean);
		/*
		 * We want to check the last interval is:
		 *
		 *  mean - stddev < interval < mean + stddev
		 *
		 * That simplifies to:
		 *
		 * -stddev < interval - mean < stddev
		 *
		 * abs(interval - mean) < stddev
		 *
		 * The standard deviation is the sqrt of the variance:
		 *
		 * abs(interval - mean) < sqrt(variance)
		 *
		 * and we want to prevent to do an sqrt, so we square
		 * the equation:
		 *
		 * (interval - mean)^2 < variance
		 *
		 * So if the latest value of the stats complies with
		 * this condition, then the wakeup source is
		 * considered predictable and can be used to predict
		 * the next event.
		 */
		interval = w->stats.values[w->stats.w_ptr];
		if ((u64)((interval - mean) * (interval - mean)) > variance)
			continue;

		/*
		 * Let's compute the next event: the wakeup source is
		 * considered predictable, we add the average interval
		 * time added to the latest interruption event time.
		 */
		next = ktime_add_us(w->timestamp, stats_mean(&w->stats));

		/*
		 * If the interrupt is supposed to happen before the
		 * minimum time, then it becomes the minimum.
		 */
		if (ktime_before(next, min))
			min = next;
	}

	/*
	 * At this point, we have our prediction but the caller is
	 * expecting the remaining time before the next event, so
	 * compute the expected sleep length.
	 */
	diff = ktime_sub(min, now);

	/*
	 * The result could be negative for different reasons:
	 *  - the prediction is incorrect
	 *  - the prediction was too near now and expired while we were
	 *    in this function
	 *
	 * In both cases, we return KTIME_MAX as a failure to do a
	 * prediction
	 */
	if (ktime_compare(diff, ktime_set(0, 0)) <= 0)
		return ktime_set(KTIME_SEC_MAX, 0);

	return diff;
}

/**
 * sched_idle_next_wakeup - Predict the next wakeup on the current cpu
 *
 * The next event on the cpu is based on a statistic approach of the
 * interrupt events and the timer deterministic value. From the timer
 * or the irqs, we return the one expected to occur first.
 *
 * Returns the expected remaining idle time before being woken up by
 * an interruption.
 */
s64 sched_idle_next_wakeup(void)
{
	s64 next_timer = ktime_to_us(tick_nohz_get_sleep_length());
	s64 next_irq = ktime_to_us(next_irq_event());

	return min(next_irq, next_timer);
}

/**
 * sched_idle - go to idle for a specified amount of time
 *
 * @duration: the idle duration time
 * @latency: the latency constraint
 *
 * Returns 0 on success, < 0 otherwise.
 */
int sched_idle(s64 duration, unsigned int latency)
{
	struct cpuidle_device *dev = __this_cpu_read(cpuidle_devices);
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);
	struct cpuidle_state_usage *su;
	struct cpuidle_state *s;
	int i, ret = 0, index = -1;

	rcu_idle_enter();

	/*
	 * No cpuidle driver is available, let's use the default arch
	 * idle function.
	 */
	if (cpuidle_not_available(drv, dev))
		goto default_idle;

	/*
	 * Find the idle state with the lowest power while satisfying
	 * our constraints. We will save energy if the duration of the
	 * idle time is bigger than the target residency which is the
	 * break even point. The choice will be modulated by the
	 * latency.
	 */
	for (i = 0; i < drv->state_count; i++) {

		s = &drv->states[i];

		su = &dev->states_usage[i];

		if (s->disabled || su->disable)
			continue;
		if (s->target_residency > duration)
			continue;
		if (s->exit_latency > latency)
			continue;

		index = i;
	}

	/*
	 * The idle task must be scheduled, it is pointless to go to
	 * idle, just re-enable the interrupt and return.
	 */
	if (current_clr_polling_and_test()) {
		local_irq_enable();
		goto out;
	}

	if (index < 0) {
		/*
		 * No idle callbacks fulfilled the constraints, jump
		 * to the default function like there wasn't any
		 * cpuidle driver.
		 */
		goto default_idle;
	} else {
		/*
		 * Enter the idle state previously returned by the
		 * governor decision.  This function will block until
		 * an interrupt occurs and will take care of
		 * re-enabling the local interrupts
		 */
		return cpuidle_enter(drv, dev, index);
	}

default_idle:
	default_idle_call();
out:
	rcu_idle_exit();
	return ret;
}

/**
 * sched_irq_timing_remove - disable the tracking of the specified irq
 *
 * Clear the irq table slot to stop tracking the interrupt.
 *
 * @irq: the irq number to stop tracking
 * @dev_id: the device id for shared irq
 *
 * This function will remove from the wakeup source prediction table.
 */
static void sched_irq_timing_remove(unsigned int irq, void *dev_id)
{
	clear_bit(irq, enabled_irq);
}

/**
 * sched_irq_timing_setup - enable the tracking of the specified irq
 *
 * Function is called with the corresponding irqdesc lock taken. It is
 * not allowed to do any memory allocation or blocking call. Flag the
 * irq table slot to be tracked in order to predict the next event.
 *
 * @irq: the interrupt numbe to be tracked
 * @act: the new irq action to be set to this interrupt
 *
 * Returns zero on success, < 0 otherwise.
 */
static int sched_irq_timing_setup(unsigned int irq, struct irqaction *act)
{
	/*
	 * No interrupt set for this descriptor or related to a timer.
	 * Timers are deterministic, so no need to try to do any
	 * prediction on them. No error for both cases, we are just not
	 * interested.
	 */
	if (!(act->flags & __IRQF_TIMER))
		return 0;

	set_bit(irq, enabled_irq);

	return 0;
}

/**
 * sched_irq_timing_free - free memory previously allocated
 *
 * @irq: the interrupt number
 */
static void sched_irq_timing_free(unsigned int irq)
{
	struct wakeup *w;
	int cpu;

	for_each_possible_cpu(cpu) {

		w = per_cpu(wakeups[irq], cpu);
		if (!w)
			continue;

		per_cpu(wakeups[irq], cpu) = NULL;
		kfree(w);
	}
}

/**
 * sched_irq_timing_alloc - allocates memory for irq tracking
 *
 * Allocates the memory to track the specified irq.
 *
 * @irq: the interrupt number
 *
 * Returns 0 on success, -ENOMEM on error.
 */
static int sched_irq_timing_alloc(unsigned int irq)
{
	struct wakeup *w;
	int cpu, ret = -ENOMEM;

	/*
	 * Allocates the wakeup structure and the stats structure. As
	 * the interrupt can occur on any cpu, allocate the wakeup
	 * structure per cpu basis.
	 */
	for_each_possible_cpu(cpu) {

		w = kzalloc(sizeof(*w), GFP_KERNEL);
		if (!w)
			goto out;

		per_cpu(wakeups[irq], cpu) = w;
	}

	ret = 0;
out:
	if (ret)
		sched_irq_timing_free(irq);

	return ret;
}

static struct irqtimings_ops irqt_ops = {
	.alloc   = sched_irq_timing_alloc,
	.free    = sched_irq_timing_free,
	.setup   = sched_irq_timing_setup,
	.remove  = sched_irq_timing_remove,
	.handler = sched_irq_timing_handler,
};

DECLARE_IRQ_TIMINGS(&irqt_ops);
