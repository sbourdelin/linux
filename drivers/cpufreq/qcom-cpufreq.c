// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018, The Linux Foundation. All rights reserved.

#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include "cpufreq-dt.h"
#include <linux/nvmem-consumer.h>

static void __init get_krait_bin_format_a(int *speed, int *pvs, int *pvs_ver,
					  struct nvmem_cell *pvs_nvmem, u8 *buf)
{
	u32 pte_efuse;

	pte_efuse = *((u32 *)buf);

	*speed = pte_efuse & 0xf;
	if (*speed == 0xf)
		*speed = (pte_efuse >> 4) & 0xf;

	if (*speed == 0xf) {
		*speed = 0;
		pr_warn("Speed bin: Defaulting to %d\n", *speed);
	} else {
		pr_info("Speed bin: %d\n", *speed);
	}

	*pvs = (pte_efuse >> 10) & 0x7;
	if (*pvs == 0x7)
		*pvs = (pte_efuse >> 13) & 0x7;

	if (*pvs == 0x7) {
		*pvs = 0;
		pr_warn("PVS bin: Defaulting to %d\n", *pvs);
	} else {
		pr_info("PVS bin: %d\n", *pvs);
	}

	kfree(buf);
}

static void __init get_krait_bin_format_b(int *speed, int *pvs, int *pvs_ver,
					  struct nvmem_cell *pvs_nvmem, u8 *buf)
{
	u32 pte_efuse, redundant_sel;

	pte_efuse = *((u32 *)buf);
	redundant_sel = (pte_efuse >> 24) & 0x7;
	*speed = pte_efuse & 0x7;

	/* 4 bits of PVS are in efuse register bits 31, 8-6. */
	*pvs = ((pte_efuse >> 28) & 0x8) | ((pte_efuse >> 6) & 0x7);
	*pvs_ver = (pte_efuse >> 4) & 0x3;

	switch (redundant_sel) {
	case 1:
		*speed = (pte_efuse >> 27) & 0xf;
		break;
	case 2:
		*pvs = (pte_efuse >> 27) & 0xf;
		break;
	}

	/* Check SPEED_BIN_BLOW_STATUS */
	if (pte_efuse & BIT(3)) {
		pr_info("Speed bin: %d\n", *speed);
	} else {
		pr_warn("Speed bin not set. Defaulting to 0!\n");
		*speed = 0;
	}

	/* Check PVS_BLOW_STATUS */
	pte_efuse = *(((u32 *)buf) + 4);
	if (pte_efuse) {
		pr_info("PVS bin: %d\n", *pvs);
	} else {
		pr_warn("PVS bin not set. Defaulting to 0!\n");
		*pvs = 0;
	}

	pr_info("PVS version: %d\n", *pvs_ver);
	kfree(buf);
}

static int __init qcom_cpufreq_populate_opps(struct nvmem_cell *pvs_nvmem)
{
	int speed = 0, pvs = 0, pvs_ver = 0, cpu;
	struct device *dev;
	u8 *buf;
	size_t len;
	char pvs_name[] = "speedXX-pvsXX-vXX";

	buf = nvmem_cell_read(pvs_nvmem, &len);
	if (len == 4)
		get_krait_bin_format_a(&speed, &pvs, &pvs_ver, pvs_nvmem, buf);
	else if (len == 8)
		get_krait_bin_format_b(&speed, &pvs, &pvs_ver, pvs_nvmem, buf);
	else
		pr_warn("Unable to read nvmem data. Defaulting to 0!\n");

	snprintf(pvs_name, sizeof(pvs_name), "speed%d-pvs%d-v%d",
		 speed, pvs, pvs_ver);

	for (cpu = 0; cpu < num_possible_cpus(); cpu++) {
		dev = get_cpu_device(cpu);
		if (!dev)
			return -ENODEV;

		if (IS_ERR(dev_pm_opp_set_prop_name(dev, pvs_name)))
			pr_warn("failed to add OPP name %s\n", pvs_name);
	}

	return 0;
}

static int __init qcom_cpufreq_driver_init(void)
{
	struct device *cpu_dev;
	struct device_node *np;
	struct nvmem_cell *pvs_nvmem;
	int ret;

	cpu_dev = get_cpu_device(0);
	if (!cpu_dev)
		return -ENODEV;

	np = dev_pm_opp_of_get_opp_desc_node(cpu_dev);
	if (!np)
		return -ENOENT;

	if (!of_device_is_compatible(np, "operating-points-v2-krait-cpu")) {
		of_node_put(np);
		return -ENODEV;
	}

	pvs_nvmem = of_nvmem_cell_get(np, NULL);
	if (IS_ERR(pvs_nvmem)) {
		dev_err(cpu_dev, "Could not get nvmem cell\n");
		return PTR_ERR(pvs_nvmem);
	}

	of_node_put(np);

	ret = qcom_cpufreq_populate_opps(pvs_nvmem);
	if (ret)
		return ret;

	return PTR_ERR(platform_device_register_simple("cpufreq-dt",
						       -1, NULL, 0));
}
module_init(qcom_cpufreq_driver_init);

MODULE_DESCRIPTION("Qualcomm CPUfreq driver");
MODULE_LICENSE("GPL v2");
