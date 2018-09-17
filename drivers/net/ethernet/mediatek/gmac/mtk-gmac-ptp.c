// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2018 MediaTek Inc.
#include "mtk-gmac.h"

static int gmac_adjust_freq(struct ptp_clock_info *ptp, s32 ppb)
{
	struct gmac_pdata *pdata =
		container_of(ptp, struct gmac_pdata, ptp_clock_info);
	unsigned long adj, diff, freq_top;
	int neg_adj = 0;

	if (ppb < 0) {
		neg_adj = 1;
		ppb = -ppb;
	}

	freq_top = pdata->ptptop_rate;
	adj = freq_top;
	adj *= ppb;
	/* div_u64 will divided the "adj" by "1000000000ULL"
	 * and return the quotient.
	 */
	diff = div_u64(adj, 1000000000ULL);
	freq_top = neg_adj ? (freq_top - diff) : (freq_top + diff);

	clk_set_rate(pdata->plat->clks[GMAC_CLK_PTP_TOP], freq_top);

	return 0;
}

static int gmac_adjust_time(struct ptp_clock_info *ptp, s64 delta)
{
	struct gmac_pdata *pdata =
		container_of(ptp, struct gmac_pdata, ptp_clock_info);
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;
	unsigned long flags;
	u32 sec, nsec, quotient, reminder;
	int neg_adj = 0;

	if (delta < 0) {
		neg_adj = 1;
		delta = -delta;
	}

	quotient = div_u64_rem(delta, 1000000000ULL, &reminder);
	sec = quotient;
	nsec = reminder;

	spin_lock_irqsave(&pdata->ptp_lock, flags);

	hw_ops->adjust_systime(pdata, sec, nsec, neg_adj);

	spin_unlock_irqrestore(&pdata->ptp_lock, flags);

	return 0;
}

static int gmac_get_time(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	struct gmac_pdata *pdata =
		container_of(ptp, struct gmac_pdata, ptp_clock_info);
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;
	u64 ns;
	u32 reminder;
	unsigned long flags;

	spin_lock_irqsave(&pdata->ptp_lock, flags);

	hw_ops->get_systime(pdata, &ns);

	spin_unlock_irqrestore(&pdata->ptp_lock, flags);

	ts->tv_sec = div_u64_rem(ns, 1000000000ULL, &reminder);
	ts->tv_nsec = reminder;

	return 0;
}

static int gmac_set_time(struct ptp_clock_info *ptp,
			 const struct timespec64 *ts)
{
	struct gmac_pdata *pdata =
		container_of(ptp, struct gmac_pdata, ptp_clock_info);
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;
	unsigned long flags;

	spin_lock_irqsave(&pdata->ptp_lock, flags);

	hw_ops->init_systime(pdata, ts->tv_sec, ts->tv_nsec);

	spin_unlock_irqrestore(&pdata->ptp_lock, flags);

	return 0;
}

static int gmac_enable(struct ptp_clock_info *ptp,
		       struct ptp_clock_request *rq,
		       int on)
{
	return -EOPNOTSUPP;
}

int ptp_init(struct gmac_pdata *pdata)
{
	struct ptp_clock_info *info = &pdata->ptp_clock_info;
	struct ptp_clock *clock;
	int ret = 0;

	if (!pdata->hw_feat.ts_src) {
		pdata->ptp_clock = NULL;
		pr_err("No PTP supports in HW\n"
			"Aborting PTP clock driver registration\n");
		return -EOPNOTSUPP;
	}

	spin_lock_init(&pdata->ptp_lock);

	pdata->ptpclk_rate = clk_get_rate(pdata->plat->clks[GMAC_CLK_PTP]);
	pdata->ptptop_rate = clk_get_rate(pdata->plat->clks[GMAC_CLK_PTP_TOP]);
	pdata->ptp_divider = pdata->ptptop_rate / pdata->ptpclk_rate;

	snprintf(info->name, sizeof(info->name), "%s",
		 netdev_name(pdata->netdev));
	info->owner = THIS_MODULE;
	info->max_adj = pdata->ptpclk_rate;
	info->adjfreq = gmac_adjust_freq;
	info->adjtime = gmac_adjust_time;
	info->gettime64 = gmac_get_time;
	info->settime64 = gmac_set_time;
	info->enable = gmac_enable;

	clock = ptp_clock_register(info, pdata->dev);
	if (IS_ERR(clock)) {
		pdata->ptp_clock = NULL;
		netdev_err(pdata->netdev, "ptp_clock_register() failed\n");
	} else {
		pdata->ptp_clock = clock;
		netdev_info(pdata->netdev, "Added PTP HW clock successfully\n");
	}

	return ret;
}

void ptp_remove(struct gmac_pdata *pdata)
{
	if (pdata->ptp_clock) {
		ptp_clock_unregister(pdata->ptp_clock);
		pdata->ptp_clock = NULL;
		pr_debug("Removed PTP HW clock successfully on %s\n",
			 pdata->netdev->name);
	}
}
