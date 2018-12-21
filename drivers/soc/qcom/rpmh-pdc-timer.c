// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 */

#include <linux/cpu_pm.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/tick.h>
#include <asm/arch_timer.h>

#include <soc/qcom/rpmh.h>

#define ARCH_TIMER_HZ (19200000)
#define PDC_TIME_VALID_SHIFT	31
#define PDC_TIME_UPPER_MASK	0xFFFFFF

static struct regmap *rsc_regmap;
static resource_size_t cmd0_data_offset;
static resource_size_t cmd1_data_offset;
static uint64_t pdc_wakeup = ~0ULL;
static raw_spinlock_t pdc_wakeup_lock;
static int suspended;

/* convert micro sec to ticks or clock cycles
 *
 *   time in cycles = (time in sec) * Freq
 *                  = (time in sec) * 19200000
 *
 * However, while converting micro sec to sec,
 * there is a possibility of round of errors.
 * So round of errors are minimized by finding
 * nano sec.
 */
static uint64_t us_to_cycles(uint64_t time_us)
{
	uint64_t sec, nsec, time_cycles;

	sec = time_us;
	do_div(sec, USEC_PER_SEC);
	nsec = time_us - sec * USEC_PER_SEC;

	if (nsec > 0) {
		nsec = nsec * NSEC_PER_USEC;
		do_div(nsec, NSEC_PER_SEC);
	}

	sec += nsec;

	time_cycles = (u64)sec * ARCH_TIMER_HZ;

	return time_cycles;
}

/*
 * Find next wakeup of a cpu and convert into
 * cycles.
 */
static uint64_t get_next_wakeup_cycles(int cpu)
{
	ktime_t next_wakeup;
	uint64_t next_wakeup_cycles;

	next_wakeup = tick_nohz_get_next_wakeup(cpu);

	/*
	 * Find the relative wakeup from current time
	 * in kernel time scale
	 */
	next_wakeup = ktime_sub(next_wakeup, ktime_get());

	next_wakeup_cycles = us_to_cycles(ktime_to_us(next_wakeup));

	/*
	 * Add current time in arch timer scale.
	 * This is needed as PDC match value is programmed
	 * with absolute value, not the relative value
	 * from current time
	 */
	next_wakeup_cycles += arch_counter_get_cntvct();

	return next_wakeup_cycles;
}

static void setup_pdc_wakeup_timer(uint64_t wakeup_cycles)
{
	u32 data0, data1;

	data0 =  (wakeup_cycles >> 32) & PDC_TIME_UPPER_MASK;
	data0 |= 1 << PDC_TIME_VALID_SHIFT;
	data1 = (wakeup_cycles & 0xFFFFFFFF);

	regmap_write(rsc_regmap, cmd0_data_offset, data0);
	regmap_write(rsc_regmap, cmd1_data_offset, data1);

}

static int cpu_pm_notifier(struct notifier_block *b,
			       unsigned long cmd, void *v)
{
	uint64_t cpu_next_wakeup;

	if (suspended)
		return NOTIFY_DONE;

	switch (cmd) {
	case CPU_PM_ENTER:
		cpu_next_wakeup = get_next_wakeup_cycles(smp_processor_id());
		raw_spin_lock(&pdc_wakeup_lock);
		/*
		 * If PDC wakeup is expired or
		 * If current cpu next wakeup is early,
		 * program the same to pdc wakeup
		 */
		if ((pdc_wakeup < arch_counter_get_cntvct()) ||
				(cpu_next_wakeup < pdc_wakeup)) {
			pdc_wakeup = cpu_next_wakeup;
			setup_pdc_wakeup_timer(pdc_wakeup);
		}
		raw_spin_unlock(&pdc_wakeup_lock);
		break;
	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}

static struct notifier_block cpu_pm_notifier_block = {
	.notifier_call = cpu_pm_notifier,
	.priority = -1, /* Should be last in the order of notifications */
};

static int pdc_timer_suspend(struct device *dev)
{
	suspended = true;
	raw_spin_lock(&pdc_wakeup_lock);
	pdc_wakeup = ~0ULL;
	setup_pdc_wakeup_timer(pdc_wakeup);
	raw_spin_unlock(&pdc_wakeup_lock);
	return 0;
}

static int pdc_timer_resume(struct device *dev)
{
	suspended = false;
	return 0;
}
static const struct dev_pm_ops pdc_timer_dev_pm_ops = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(pdc_timer_suspend, pdc_timer_resume)
};

static int pdc_timer_probe(struct platform_device *pdev)
{
	struct device *pdc_timer_dev = &pdev->dev;
	struct resource *res = NULL;

	if (IS_ERR_OR_NULL(pdc_timer_dev)) {
		pr_err("%s fail\n", __func__);
		return PTR_ERR(pdc_timer_dev);
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		pr_err("res not found\n");
		return -ENODEV;
	}
	cmd0_data_offset = res->start;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		pr_err("res not found\n");
		return -ENODEV;
	}
	cmd1_data_offset =  res->start;

	rsc_regmap = dev_get_regmap(pdc_timer_dev->parent, NULL);
	if (!rsc_regmap) {
		pr_err("regmap for parent is not found\n");
		return -ENODEV;
	}

	raw_spin_lock_init(&pdc_wakeup_lock);
	cpu_pm_register_notifier(&cpu_pm_notifier_block);

	return 0;
}

static const struct of_device_id pdc_timer_drv_match[] = {
	{ .compatible = "qcom,pdc-timer", },
	{ }
};

static struct platform_driver pdc_timer_driver = {
	.probe = pdc_timer_probe,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = pdc_timer_drv_match,
		.pm = &pdc_timer_dev_pm_ops,
	},
};
builtin_platform_driver(pdc_timer_driver);
