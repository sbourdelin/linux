// SPDX-License-Identifier: GPL-2.0
/* 
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>

#define MSM_ID_SMEM	137
#define SILVER_LEAD	0
#define GOLD_LEAD	2

enum _msm_id {
	MSM8996V3 = 0xF6ul,
	APQ8096V3 = 0x123ul,
	MSM8996SG = 0x131ul,
	APQ8096SG = 0x138ul,
};

enum _msm8996_version {
	MSM8996_V3,
	MSM8996_SG,
	NUM_OF_MSM8996_VERSIONS,
};

static enum _msm8996_version __init qcom_cpufreq_kryo_get_msm_id(void)
{
	size_t len;
	u32 *msm_id;
	enum _msm8996_version version;

	msm_id = qcom_smem_get(QCOM_SMEM_HOST_ANY, MSM_ID_SMEM, &len);
	/* The first 4 bytes are format, next to them is the actual msm-id */
	msm_id++;

	switch ((enum _msm_id)*msm_id) {
	case MSM8996V3:
	case APQ8096V3:
		version = MSM8996_V3;
		break;
	case MSM8996SG:
	case APQ8096SG:
		version = MSM8996_SG;
		break;
	default:
		version = NUM_OF_MSM8996_VERSIONS;
	}

	return version;
}

static int __init qcom_cpufreq_kryo_driver_init(void)
{
	size_t len;
	int ret;
	u32 versions;
	enum _msm8996_version msm8996_version;
	u8 *speedbin;
	struct device *cpu_dev;
	struct device_node *np;
	struct nvmem_cell *speedbin_nvmem;
	struct opp_table *opp_temp = NULL;

	cpu_dev = get_cpu_device(SILVER_LEAD);
	if (IS_ERR_OR_NULL(cpu_dev))
		return PTR_ERR(cpu_dev);

	msm8996_version = qcom_cpufreq_kryo_get_msm_id();
	if (NUM_OF_MSM8996_VERSIONS == msm8996_version) {
		dev_err(cpu_dev, "Not Snapdragon 820/821!");
		return -ENODEV;
        }

	np = dev_pm_opp_of_get_opp_desc_node(cpu_dev);
	if (IS_ERR_OR_NULL(np))
		return PTR_ERR(np);

	if (!of_device_is_compatible(np, "operating-points-v2-kryo-cpu")) {
		ret = -ENOENT;
		goto free_np;
	}

	speedbin_nvmem = of_nvmem_cell_get(np, NULL);
	if (IS_ERR(speedbin_nvmem)) {
		ret = PTR_ERR(speedbin_nvmem);
		dev_err(cpu_dev, "Could not get nvmem cell: %d\n", ret);
		goto free_np;
	}

	speedbin = nvmem_cell_read(speedbin_nvmem, &len);

	switch (msm8996_version) {
	case MSM8996_V3:
		versions = 1 << (unsigned int)(*speedbin);
		break;
	case MSM8996_SG:
		versions = 1 << ((unsigned int)(*speedbin) + 4);
		break;
	default:
		BUG();
		break;
	}

	ret = PTR_ERR_OR_ZERO(opp_temp =
			      dev_pm_opp_set_supported_hw(cpu_dev,&versions,1));
	if (0 > ret)
		goto free_opp;

	cpu_dev = get_cpu_device(GOLD_LEAD);
	ret = PTR_ERR_OR_ZERO(opp_temp =
			      dev_pm_opp_set_supported_hw(cpu_dev,&versions,1));
	if (0 > ret)
		goto free_opp;

	ret = PTR_ERR_OR_ZERO(platform_device_register_simple("cpufreq-dt",
							      -1, NULL, 0));

	if (0 == ret)
		return 0;

free_opp:
	dev_pm_opp_put_supported_hw(opp_temp);

free_np:
	of_node_put(np);

	return ret;
}
late_initcall(qcom_cpufreq_kryo_driver_init);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. Kryo CPUfreq driver");
MODULE_LICENSE("GPL v2");
