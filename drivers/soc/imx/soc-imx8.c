// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 NXP.
 */

#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/sys_soc.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#define OCOTP_CFG3			0x440
#define OCOTP_CFG3_MKT_SEGMENT_SHIFT	6
#define OCOTP_CFG3_CONSUMER		0
#define OCOTP_CFG3_EXT_CONSUMER		1
#define OCOTP_CFG3_INDUSTRIAL		2
#define OCOTP_CFG3_AUTO			3

static void __init imx8mq_opp_check_speed_grading(struct device *cpu_dev)
{
	struct device_node *np;
	void __iomem *base;
	u32 val;

	np = of_find_compatible_node(NULL, NULL, "fsl,imx8mq-ocotp");
	if (!np) {
		pr_warn("failed to find ocotp node\n");
		return;
	}

	base = of_iomap(np, 0);
	if (!base) {
		pr_warn("failed to map ocotp\n");
		goto put_node;
	}
	val = readl_relaxed(base + OCOTP_CFG3);
	val >>= OCOTP_CFG3_MKT_SEGMENT_SHIFT;
	val &= 0x3;

	switch (val) {
	case OCOTP_CFG3_CONSUMER:
		if (dev_pm_opp_disable(cpu_dev, 800000000))
			pr_warn("failed to disable 800MHz OPP!\n");
		if (dev_pm_opp_disable(cpu_dev, 1300000000))
			pr_warn("failed to disable 1.3GHz OPP!\n");
		break;
	case OCOTP_CFG3_INDUSTRIAL:
		if (dev_pm_opp_disable(cpu_dev, 1000000000))
			pr_warn("failed to disable 1GHz OPP!\n");
		if (dev_pm_opp_disable(cpu_dev, 1500000000))
			pr_warn("failed to disable 1.5GHz OPP!\n");
		break;
	default:
		/* consumer part for default */
		if (dev_pm_opp_disable(cpu_dev, 800000000))
			pr_warn("failed to disable 800MHz OPP!\n");
		if (dev_pm_opp_disable(cpu_dev, 1300000000))
			pr_warn("failed to disable 1.3GHz OPP!\n");
		break;
	}

	iounmap(base);

put_node:
	of_node_put(np);
}

static void __init imx8mq_opp_init(void)
{
	struct device_node *np;
	struct device *cpu_dev = get_cpu_device(0);

	if (!cpu_dev) {
		pr_warn("failed to get cpu0 device\n");
		return;
	}
	np = of_node_get(cpu_dev->of_node);
	if (!np) {
		pr_warn("failed to find cpu0 node\n");
		return;
	}

	if (dev_pm_opp_of_add_table(cpu_dev)) {
		pr_warn("failed to init OPP table\n");
		goto put_node;
	}

	imx8mq_opp_check_speed_grading(cpu_dev);

put_node:
	of_node_put(np);
}

static int __init imx8_register_cpufreq(void)
{
	if (of_machine_is_compatible("fsl,imx8mq")) {
		imx8mq_opp_init();
		platform_device_register_simple("imx8mq-cpufreq", -1, NULL, 0);
	}

	return 0;
}
late_initcall(imx8_register_cpufreq);
