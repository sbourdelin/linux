/*
 * Copyright (c) 2015, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/clocksource.h>
#include "en.h"

enum {
	MLX5E_CYCLES_SHIFT	= 23
};

void mlx5e_fill_hwstamp(struct mlx5e_tstamp *tstamp,
			struct skb_shared_hwtstamps *hwts,
			u64 timestamp)
{
	unsigned long flags;
	u64 nsec;

	memset(hwts, 0, sizeof(struct skb_shared_hwtstamps));
	read_lock_irqsave(&tstamp->lock, flags);
	nsec = timecounter_cyc2time(&tstamp->clock, timestamp);
	read_unlock_irqrestore(&tstamp->lock, flags);

	hwts->hwtstamp = ns_to_ktime(nsec);
}

static cycle_t mlx5e_read_clock(const struct cyclecounter *cc)
{
	struct mlx5e_tstamp *tstamp = container_of(cc, struct mlx5e_tstamp,
						   cycles);
	struct mlx5e_priv *priv = container_of(tstamp, struct mlx5e_priv,
					       tstamp);

	return mlx5_core_read_clock(priv->mdev) & cc->mask;
}

void mlx5e_timestamp_overflow_check(struct mlx5e_priv *priv)
{
	bool timeout = time_is_before_jiffies(priv->tstamp.last_overflow_check +
					      priv->tstamp.overflow_period);
	unsigned long flags;

	if (timeout) {
		write_lock_irqsave(&priv->tstamp.lock, flags);
		timecounter_read(&priv->tstamp.clock);
		write_unlock_irqrestore(&priv->tstamp.lock, flags);
		priv->tstamp.last_overflow_check = jiffies;
	}
}

static void mlx5e_timestamp_init_config(struct mlx5e_tstamp *tstamp)
{
	tstamp->hwtstamp_config.flags = 0;
	tstamp->hwtstamp_config.tx_type = HWTSTAMP_TX_OFF;
	tstamp->hwtstamp_config.rx_filter = HWTSTAMP_FILTER_NONE;
}

void mlx5e_timestamp_init(struct mlx5e_priv *priv)
{
	struct mlx5e_tstamp *tstamp = &priv->tstamp;
	u64 ns;
	u64 frac = 0;
	u32 dev_freq;

	mlx5e_timestamp_init_config(tstamp);
	dev_freq = MLX5_CAP_GEN(priv->mdev, device_frequency_khz);
	if (!dev_freq) {
		mlx5_core_warn(priv->mdev, "invalid device_frequency_khz. %s failed\n",
			       __func__);
		return;
	}
	rwlock_init(&tstamp->lock);
	memset(&tstamp->cycles, 0, sizeof(tstamp->cycles));
	tstamp->cycles.read = mlx5e_read_clock;
	tstamp->cycles.shift = MLX5E_CYCLES_SHIFT;
	tstamp->cycles.mult = clocksource_khz2mult(dev_freq,
						   tstamp->cycles.shift);
	tstamp->nominal_c_mult = tstamp->cycles.mult;
	tstamp->cycles.mask = CLOCKSOURCE_MASK(41);

	timecounter_init(&tstamp->clock, &tstamp->cycles,
			 ktime_to_ns(ktime_get_real()));

	/* Calculate period in seconds to call the overflow watchdog - to make
	 * sure counter is checked at least once every wrap around.
	 */
	ns = cyclecounter_cyc2ns(&tstamp->cycles, tstamp->cycles.mask, frac,
				 &frac);
	do_div(ns, NSEC_PER_SEC / 2 / HZ);
	tstamp->overflow_period = ns;
}
