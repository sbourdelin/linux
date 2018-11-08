// SPDX-License-Identifier: GPL-2.0
/*
 * Timer events oriented CPU idle governor
 *
 * Copyright (C) 2018 Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 *
 * The idea of this governor is based on the observation that on many systems
 * timer events are two or more orders of magnitude more frequent than any
 * other interrupts, so they are likely to be the most significant source of CPU
 * wakeups from idle states.  Moreover, information about what happened in the
 * (relatively recent) past can be used to estimate whether or not the deepest
 * idle state with target residency within the time to the closest timer is
 * likely to be suitable for the upcoming idle time of the CPU and, if not, then
 * which of the shallower idle states to choose.
 *
 * Of course, non-timer wakeup sources are more important in some use cases and
 * they can be covered by detecting patterns among recent idle time intervals
 * of the CPU.  However, even in that case it is not necessary to take idle
 * duration values greater than the time till the closest timer into account, as
 * the patterns that they may belong to produce average values close enough to
 * the time till the closest timer (sleep length) anyway.
 *
 * Thus this governor estimates whether or not the upcoming idle time of the CPU
 * is likely to be significantly shorter than the sleep length and selects an
 * idle state for it in accordance with that, as follows:
 *
 * - If there is a pattern of 5 or more recent non-timer wakeups earlier than
 *   the closest timer event, expect one more of them to occur and use the
 *   average of the idle duration values corresponding to them to select an
 *   idle state for the CPU.
 *
 * - Otherwise, find the state on the basis of the sleep length and state
 *   statistics collected over time:
 *
 *   o Find the deepest idle state whose target residency is less than or euqal
 *     to the sleep length.
 *
 *   o Select it if it matched both the sleep length and the idle duration
 *     measured after wakeup in the past more often than it matched the sleep
 *     length, but not the idle duration (i.e. the measured idle duration was
 *     significantly shorter than the sleep length matched by that state).
 *
 *   o Otherwise, select the shallower state with the greatest matched "early"
 *     wakeups metric.
 */

#include <linux/cpuidle.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/sched/clock.h>
#include <linux/tick.h>

/*
 * The SPIKE value is added to metrics when they grow and the DECAY_SHIFT value
 * is used for decreasing metrics on a regular basis.
 */
#define SPIKE		1024
#define DECAY_SHIFT	3

/*
 * Number of the most recent idle duration values to take into consideration for
 * the detection of wakeup patterns.
 */
#define INTERVALS	8

/*
 * Ratio of the sample spread limit and the length of the interesting intervals
 * range used for pattern detection, reptesented as a shift.
 */
#define MAX_SPREAD_SHIFT	3

/**
 * struct teo_idle_state - Idle state data used by the TEO cpuidle governor.
 * @early_hits: "Early" CPU wakeups matched by this state.
 * @hits: "On time" CPU wakeups matched by this state.
 * @misses: CPU wakeups "missed" by this state.
 *
 * A CPU wakeup is "matched" by a given idle state if the idle duration measured
 * after the wakeup is between the target residency of that state and the target
 * residnecy of the next one (or if this is the deepest available idle state, it
 * "matches" a CPU wakeup when the measured idle duration is at least equal to
 * its target residency).
 *
 * Also, from the TEO governor prespective, a CPU wakeup from idle is "early" if
 * it occurs significantly earlier than the closest expected timer event (that
 * is, early enough to match an idle state shallower than the one matching the
 * time till the closest timer event).  Otherwise, the wakeup is "on time", or
 * it is a "hit".
 *
 * A "miss" occurs when the given state doesn't match the wakeup, but it matches
 * the time till the closest timer event used for idle state selection.
 */
struct teo_idle_state {
	unsigned int early_hits;
	unsigned int hits;
	unsigned int misses;
};

/**
 * struct teo_cpu - CPU data used by the TEO cpuidle governor.
 * @time_span_ns: Time between idle state selection and post-wakeup update.
 * @sleep_length_ns: Time till the closest timer event (at the selection time).
 * @states: Idle states data corresponding to this CPU.
 * @last_state: Idle state entered by the CPU last time.
 * @interval_idx: Index of the most recent saved idle interval.
 * @intervals: Saved idle duration values.
 */
struct teo_cpu {
	u64 time_span_ns;
	u64 sleep_length_ns;
	struct teo_idle_state states[CPUIDLE_STATE_MAX];
	int last_state;
	int interval_idx;
	unsigned int intervals[INTERVALS];
};

static DEFINE_PER_CPU(struct teo_cpu, teo_cpus);

/**
 * teo_update - Update CPU data after wakeup.
 * @drv: cpuidle driver containing state data.
 * @dev: Target CPU.
 */
static void teo_update(struct cpuidle_driver *drv, struct cpuidle_device *dev)
{
	struct teo_cpu *cpu_data = per_cpu_ptr(&teo_cpus, dev->cpu);
	unsigned int sleep_length_us = ktime_to_us(cpu_data->sleep_length_ns);
	int i, idx_hit = -1, idx_timer = -1;
	unsigned int measured_us;

	if (cpu_data->time_span_ns == cpu_data->sleep_length_ns) {
		/* One of the safety nets has triggered (most likely). */
		measured_us = sleep_length_us;
	} else {
		measured_us = dev->last_residency;
		i = cpu_data->last_state;
		if (measured_us >= 2 * drv->states[i].exit_latency)
			measured_us -= drv->states[i].exit_latency;
		else
			measured_us /= 2;
	}

	/*
	 * Decay the "early hits" metric for all of the states and find the
	 * states matching the sleep length and the measured idle duration.
	 */
	for (i = 0; i < drv->state_count; i++) {
		unsigned int early_hits = cpu_data->states[i].early_hits;

		cpu_data->states[i].early_hits -= early_hits >> DECAY_SHIFT;

		if (drv->states[i].target_residency <= measured_us)
			idx_hit = i;

		if (drv->states[i].target_residency <= sleep_length_us)
			idx_timer = i;
	}

	/*
	 * Update the "hits" and "misses" data for the state matching the sleep
	 * length.  If it matches the measured idle duration too, this is a hit,
	 * so increase the "hits" metric for it then.  Otherwise, this is a
	 * miss, so increase the "misses" metric for it.  In the latter case
	 * also increase the "early hits" metric for the state that actually
	 * matches the measured idle duration.
	 */
	if (idx_timer >= 0) {
		unsigned int hits = cpu_data->states[idx_timer].hits;
		unsigned int misses = cpu_data->states[idx_timer].misses;

		hits -= hits >> DECAY_SHIFT;
		misses -= misses >> DECAY_SHIFT;

		if (idx_timer > idx_hit) {
			misses += SPIKE;
			if (idx_hit >= 0)
				cpu_data->states[idx_hit].early_hits += SPIKE;
		} else {
			hits += SPIKE;
		}

		cpu_data->states[idx_timer].misses = misses;
		cpu_data->states[idx_timer].hits = hits;
	}

	/*
	 * Save idle duration values corresponding to non-timer wakeups for
	 * pattern detection.
	 *
	 * If the total time span between idle state selection and the "reflect"
	 * callback is greater than or equal to the sleep length determined at
	 * the idle state selection time, the wakeup is likely to be due to a
	 * timer event.
	 */
	if (cpu_data->time_span_ns >= cpu_data->sleep_length_ns)
		measured_us = UINT_MAX;

	cpu_data->intervals[cpu_data->interval_idx++] = measured_us;
	if (cpu_data->interval_idx > INTERVALS)
		cpu_data->interval_idx = 0;
}

/**
 * teo_idle_duration - Estimate the duration of the upcoming CPU idle time.
 * @drv: cpuidle driver containing state data.
 * @cpu_data: Governor data for the target CPU.
 * @sleep_length_us: Time till the closest timer event in microseconds.
 */
unsigned int teo_idle_duration(struct cpuidle_driver *drv,
			       struct teo_cpu *cpu_data,
			       unsigned int sleep_length_us)
{
	u64 range, max_spread, sum, max, min;
	unsigned int i, count;

	/*
	 * If the sleep length is below the target residency of idle state 1,
	 * the only viable choice is to select the first available (enabled)
	 * idle state, so return immediately in that case.
	 */
	if (sleep_length_us < drv->states[1].target_residency)
		return sleep_length_us;

	/*
	 * The purpose of this function is to check if there is a pattern of
	 * wakeups indicating that it would be better to select a state
	 * shallower than the deepest one matching the sleep length or the
	 * deepest one at all if the sleep lenght is long.  Larger idle duration
	 * values are beyond the interesting range.
	 */
	range = drv->states[drv->state_count-1].target_residency;
	range = min_t(u64, sleep_length_us, range + (range >> 2));

	/*
	 * This is the value to compare with the distance between the average
	 * and the greatest sample to decide whether or not it is small enough.
	 * Take 10 us as the total cap of it.
	 */
	max_spread = max_t(u64, range >> MAX_SPREAD_SHIFT, 10);

	/*
	 * First pass: compute the sum of interesting samples, the minimum and
	 * maximum of them and count them.
	 */
	count = 0;
	sum = 0;
	max = 0;
	min = UINT_MAX;

	for (i = 0; i < INTERVALS; i++) {
		u64 val = cpu_data->intervals[i];

		if (val >= range)
			continue;

		count++;
		sum += val;
		if (max < val)
			max = val;

		if (min > val)
			min = val;
	}

	/* Give up if the number of interesting samples is too small. */
	if (count <= INTERVALS / 2)
		return sleep_length_us;

	/*
	 * If the distance between the max or min and the average is too large,
	 * try to refine by discarding the max, as long as the count is above 3.
	 */
	while (count > 3 && max > max_spread &&
	       ((max - max_spread) * count > sum ||
	        (min + max_spread) * count < sum)) {

		range = max;

		/*
		 * Compute the sum of samples in the interesting range.  Count
		 * them and find the maximum of them.
		 */
		count = 0;
		sum = 0;
		max = 0;

		for (i = 0; i < INTERVALS; i++) {
			u64 val = cpu_data->intervals[i];

			if (val >= range)
				continue;

			count++;
			sum += val;
			if (max < val)
				max = val;
		}
	}

	return div64_u64(sum, count);
}

/**
 * teo_select - Selects the next idle state to enter.
 * @drv: cpuidle driver containing state data.
 * @dev: Target CPU.
 * @stop_tick: Indication on whether or not to stop the scheduler tick.
 */
static int teo_select(struct cpuidle_driver *drv, struct cpuidle_device *dev,
		      bool *stop_tick)
{
	struct teo_cpu *cpu_data = per_cpu_ptr(&teo_cpus, dev->cpu);
	int latency_req = cpuidle_governor_latency_req(dev->cpu);
	unsigned int sleep_length_us, duration_us;
	unsigned int max_early_count;
	int max_early_idx, idx, i;
	ktime_t delta_tick;

	if (cpu_data->last_state >= 0) {
		teo_update(drv, dev);
		cpu_data->last_state = -1;
	}

	cpu_data->time_span_ns = local_clock();

	cpu_data->sleep_length_ns = tick_nohz_get_sleep_length(&delta_tick);
	sleep_length_us = ktime_to_us(cpu_data->sleep_length_ns);

	duration_us = teo_idle_duration(drv, cpu_data, sleep_length_us);
	if (tick_nohz_tick_stopped()) {
		/*
		 * If the tick is already stopped, the cost of possible short
		 * idle duration misprediction is much higher, because the CPU
		 * may be stuck in a shallow idle state for a long time as a
		 * result of it.  In that case say we might mispredict and use
		 * the known time till the closest timer event for the idle
		 * state selection.
		 */
		if (duration_us < TICK_USEC)
			duration_us = sleep_length_us;
	} else {
		/*
		 * If the time needed to enter and exit the idle state matching
		 * the expected idle duration is comparable with the expected
		 * idle duration itself, the time to spend in that state is
		 * likely to be small, so it probably is better to select a
		 * shallower state.  Tweak the latency limit to enforce that.
		 */
		if (duration_us < latency_req)
			latency_req = duration_us;
	}

	max_early_count = 0;
	max_early_idx = -1;
	idx = -1;

	for (i = 0; i < drv->state_count; i++) {
		struct cpuidle_state *s = &drv->states[i];
		struct cpuidle_state_usage *su = &dev->states_usage[i];

		if (s->disabled || su->disable) {
			/*
			 * If the "early hits" metric of a disabled state is
			 * greater than the current maximum, it should be taken
			 * into account, because it would be a mistake to select
			 * a deeper state with lower "early hits" metric.  The
			 * index cannot be changed to point to it, however, so
			 * just increase the max count alone and let the index
			 * still point to a shallower idle state.
			 */
			if (max_early_idx >= 0 &&
			    max_early_count < cpu_data->states[i].early_hits)
				max_early_count = cpu_data->states[i].early_hits;

			continue;
		}

		if (idx < 0)
			idx = i; /* first enabled state */

		if (s->target_residency > duration_us) {
			/*
			 * If the next wakeup is expected to be "early", the
			 * time frame of it is known already.
			 */
			if (duration_us < sleep_length_us)
				break;

			/*
			 * If the "hits" metric of the state matching the sleep
			 * length is greater than its "misses" metric, that is
			 * the one to use.
			 */
			if (cpu_data->states[idx].hits >= cpu_data->states[idx].misses)
				break;

			/*
			 * It is more likely that one of the shallower states
			 * will match the idle duration measured after wakeup,
			 * so take the one with the maximum "early hits" metric,
			 * but if that cannot be determined, just use the state
			 * selected so far.
			 */
			if (max_early_idx >= 0) {
				idx = max_early_idx;
				duration_us = drv->states[idx].target_residency;
			}
			break;
		}
		if (s->exit_latency > latency_req) {
			/*
			 * If we break out of the loop for latency reasons, use
			 * the target residency of the selected state as the
			 * expected idle duration to avoid stopping the tick
			 * as long as that target residency is low enough.
			 */
			duration_us = drv->states[idx].target_residency;
			break;
		}

		idx = i;

		if (max_early_count < cpu_data->states[i].early_hits &&
		    !(tick_nohz_tick_stopped() &&
		      drv->states[i].target_residency < TICK_USEC)) {
			max_early_count = cpu_data->states[i].early_hits;
			max_early_idx = i;
		}
	}

	if (idx < 0)
		idx = 0; /* No states enabled. Must use 0. */

	/*
	 * Don't stop the tick if the selected state is a polling one or if the
	 * expected idle duration is shorter than the tick period length.
	 */
	if (((drv->states[idx].flags & CPUIDLE_FLAG_POLLING) ||
	    duration_us < TICK_USEC) && !tick_nohz_tick_stopped()) {
		unsigned int delta_tick_us = ktime_to_us(delta_tick);

		*stop_tick = false;

		if (idx > 0 && drv->states[idx].target_residency > delta_tick_us) {
			/*
			 * The tick is not going to be stopped and the target
			 * residency of the state to be returned is not within
			 * the time until the closest timer event including the
			 * tick, so try to correct that.
			 */
			for (i = idx - 1; i > 0; i--) {
				if (drv->states[i].disabled ||
				    dev->states_usage[i].disable)
					continue;

				if (drv->states[i].target_residency <= delta_tick_us)
					break;
			}
			idx = i;
		}
	}

	return idx;
}

/**
 * teo_reflect - Note that governor data for the CPU need to be updated.
 * @dev: Target CPU.
 * @state: Entered state.
 */
static void teo_reflect(struct cpuidle_device *dev, int state)
{
	struct teo_cpu *cpu_data = per_cpu_ptr(&teo_cpus, dev->cpu);

	cpu_data->last_state = state;
	/*
	 * If the wakeup was not "natural", but triggered by one of the safety
	 * nets, assume that the CPU might have been idle for the entire sleep
	 * length time.
	 */
	if (dev->poll_time_limit ||
	    (tick_nohz_idle_got_tick() && cpu_data->sleep_length_ns > TICK_NSEC))
		cpu_data->time_span_ns = cpu_data->sleep_length_ns;
	else
		cpu_data->time_span_ns = local_clock() - cpu_data->time_span_ns;
}

/**
 * teo_enable_device - Initialize the governor's data for the target CPU.
 * @drv: cpuidle driver (not used).
 * @dev: Target CPU.
 */
static int teo_enable_device(struct cpuidle_driver *drv,
			     struct cpuidle_device *dev)
{
	struct teo_cpu *cpu_data = per_cpu_ptr(&teo_cpus, dev->cpu);
	int i;

	memset(cpu_data, 0, sizeof(*cpu_data));

	for (i = 0; i < INTERVALS; i++)
		cpu_data->intervals[i] = UINT_MAX;

	return 0;
}

static struct cpuidle_governor teo_governor = {
	.name =		"teo",
	.rating =	22,
	.enable =	teo_enable_device,
	.select =	teo_select,
	.reflect =	teo_reflect,
};

static int __init teo_governor_init(void)
{
	return cpuidle_register_governor(&teo_governor);
}

postcore_initcall(teo_governor_init);
