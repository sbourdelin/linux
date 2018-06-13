// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Linaro, Inc.
 *
 * Author: Baolin Wang <baolin.wang@linaro.org>
 */

#include <linux/alarmtimer.h>
#include <linux/clocksource.h>
#include <linux/persistent_clock.h>

/**
 * persistent_clock_read_data - data required to read persistent clock
 * @read: Returns a cycle value from persistent clock.
 * @last_cycles: Clock cycle value at last update.
 * @last_ns: Time value (nanoseconds) at last update.
 * @mask: Bitmask for two's complement subtraction of non 64bit clocks.
 * @mult: Cycle to nanosecond multiplier.
 * @shift: Cycle to nanosecond divisor.
 */
struct persistent_clock_read_data {
	u64 (*read)(void);
	u64 last_cycles;
	u64 last_ns;
	u64 mask;
	u32 mult;
	u32 shift;
};

/**
 * persistent_clock - represent the persistent clock
 * @read_data: Data required to read from persistent clock.
 * @seq: Sequence counter for protecting updates.
 * @freq: The frequency of the persistent clock.
 * @wrap: Duration for persistent clock can run before wrapping.
 * @alarm: Update timeout for persistent clock wrap.
 * @alarm_inited: Indicate if the alarm has been initialized.
 */
struct persistent_clock {
	struct persistent_clock_read_data read_data;
	seqcount_t seq;
	u32 freq;
	ktime_t wrap;
	struct alarm alarm;
	bool alarm_inited;
};

static struct persistent_clock p;

void read_persistent_clock64(struct timespec64 *ts)
{
	struct persistent_clock_read_data *read_data = &p.read_data;
	unsigned long seq;
	u64 delta, nsecs;

	if (!read_data->read) {
		ts->tv_sec = 0;
		ts->tv_nsec = 0;
		return;
	}

	do {
		seq = read_seqcount_begin(&p.seq);
		delta = (read_data->read() - read_data->last_cycles) &
			read_data->mask;

		nsecs = read_data->last_ns +
			clocksource_cyc2ns(delta, read_data->mult,
					   read_data->shift);
		*ts = ns_to_timespec64(nsecs);
	} while (read_seqcount_retry(&p.seq, seq));
}

static void persistent_clock_update(void)
{
	struct persistent_clock_read_data *read_data = &p.read_data;
	u64 cycles, delta;

	write_seqcount_begin(&p.seq);

	cycles = read_data->read();
	delta = (cycles - read_data->last_cycles) & read_data->mask;
	read_data->last_ns += clocksource_cyc2ns(delta, read_data->mult,
						 read_data->shift);
	read_data->last_cycles = cycles;

	write_seqcount_end(&p.seq);
}

static enum alarmtimer_restart persistent_clock_alarm_fired(struct alarm *alarm,
							    ktime_t now)
{
	persistent_clock_update();

	alarm_forward(&p.alarm, now, p.wrap);
	return ALARMTIMER_RESTART;
}

int persistent_clock_init_and_register(u64 (*read)(void), u64 mask,
				       u32 freq, u64 maxsec)
{
	struct persistent_clock_read_data *read_data = &p.read_data;
	u64 wrap, res, secs = maxsec;

	if (!read || !mask || !freq)
		return -EINVAL;

	if (!secs) {
		/*
		 * If the timer driver did not specify the maximum conversion
		 * seconds of the persistent clock, then we calculate the
		 * conversion range with the persistent clock's bits and
		 * frequency.
		 */
		secs = mask;
		do_div(secs, freq);

		/*
		 * Some persistent counter can be larger than 32bit, so we
		 * need limit the max suspend time to have a good conversion
		 * precision. So 24 hours may be enough usually.
		 */
		if (secs > 86400)
			secs = 86400;
	}

	/* Calculate the mult/shift to convert cycles to ns. */
	clocks_calc_mult_shift(&read_data->mult, &read_data->shift, freq,
			       NSEC_PER_SEC, (u32)secs);

	/* Calculate how many nanoseconds until we risk wrapping. */
	wrap = clocks_calc_max_nsecs(read_data->mult, read_data->shift, 0,
				     mask, NULL);
	p.wrap = ns_to_ktime(wrap);

	p.freq = freq;
	read_data->mask = mask;
	read_data->read = read;

	persistent_clock_update();

	/* Calculate the ns resolution of this persistent clock. */
	res = clocksource_cyc2ns(1ULL, read_data->mult, read_data->shift);

	pr_info("persistent clock: mask %llu at %uHz, resolution %lluns, wraps every %lluns\n",
		mask, freq, res, wrap);
	return 0;
}

void persistent_clock_cleanup(void)
{
	p.read_data.read = NULL;

	if (p.alarm_inited) {
		alarm_cancel(&p.alarm);
		p.alarm_inited = false;
	}
}

void persistent_clock_start_alarmtimer(void)
{
	struct persistent_clock_read_data *read_data = &p.read_data;
	ktime_t now;

	/*
	 * If no persistent clock function has been provided or the alarmtimer
	 * has been initialized at that point, just return.
	 */
	if (!read_data->read || p.alarm_inited)
		return;

	persistent_clock_update();

	/*
	 * Since the persistent clock will not be stopped when system enters the
	 * suspend state, thus we need start one alarmtimer to wakeup the system
	 * to update the persistent clock before wrapping. We should start the
	 * update alarmtimer after the alarmtimer subsystem was initialized.
	 */
	alarm_init(&p.alarm, ALARM_BOOTTIME, persistent_clock_alarm_fired);
	now = ktime_get_boottime();
	alarm_start(&p.alarm, ktime_add(now, p.wrap));
	p.alarm_inited = true;
}
