// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Amlogic Meson MMC Sub Clock Controller Driver
 *
 * Copyright (c) 2017 Baylibre SAS.
 * Author: Jerome Brunet <jbrunet@baylibre.com>
 *
 * Copyright (c) 2018 Amlogic, inc.
 * Author: Yixun Lan <yixun.lan@amlogic.com>
 * Author: Jianxin Pan <jianxin.pan@amlogic.com>
 */

#include <linux/clk-provider.h>
#include "clkc.h"

static int meson_clk_phase_delay_get_phase(struct clk_hw *hw)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_phase_delay_data *ph =
		meson_clk_get_phase_delay_data(clk);
	unsigned long period_ps, p, d;
	int degrees;

	p = meson_parm_read(clk->map, &ph->phase);
	degrees = p * 360 / (1 << (ph->phase.width));

	period_ps = DIV_ROUND_UP((unsigned long)NSEC_PER_SEC * 1000,
				 clk_hw_get_rate(hw));

	d = meson_parm_read(clk->map, &ph->delay);
	degrees += d * ph->delay_step_ps * 360 / period_ps;
	degrees %= 360;

	return degrees;
}

static int meson_clk_phase_delay_set_phase(struct clk_hw *hw, int degrees)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_phase_delay_data *ph =
		meson_clk_get_phase_delay_data(clk);
	unsigned long period_ps, d = 0, r;
	u64 p;

	p = degrees % 360;
	period_ps = DIV_ROUND_UP((unsigned long)NSEC_PER_SEC * 1000,
				 clk_hw_get_rate(hw));

	/* First compute the phase index (p), the remainder (r) is the
	 * part we'll try to acheive using the delays (d).
	 */
	r = do_div(p, 360 / (1 << (ph->phase.width)));
	d = DIV_ROUND_CLOSEST(r * period_ps,
			      360 * ph->delay_step_ps);
	d = min(d, PMASK(ph->delay.width));

	meson_parm_write(clk->map, &ph->phase, p);
	meson_parm_write(clk->map, &ph->delay, d);
	return 0;
}

const struct clk_ops meson_clk_phase_delay_ops = {
	.get_phase = meson_clk_phase_delay_get_phase,
	.set_phase = meson_clk_phase_delay_set_phase,
};
EXPORT_SYMBOL_GPL(meson_clk_phase_delay_ops);
