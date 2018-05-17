// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 */

/*
 * In Certain QCOM SoCs like apq8096 and msm8996 that have KRYO processors,
 * the CPU frequency subset and voltage value of each OPP varies
 * based on the silicon variant in use. Qualcomm Process Voltage Scaling Tables
 * defines the voltage and frequency value based on the msm-id in SMEM
 * and speedbin blown in the efuse combination.
 * The qcom-cpufreq-kryo driver reads the msm-id and efuse value from the SoC
 * to provide the OPP framework with required information.
 * This is used to determine the voltage and frequency value for each OPP of
 * operating-points-v2 table when it is parsed by the OPP framework.
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
	int ret = 0;
	u32 versions;
	enum _msm8996_version msm8996_version;
	u8 *speedbin;
	struct device *cpu_dev_silver, *cpu_dev_gold;
	struct device_node *np;
	struct nvmem_cell *speedbin_nvmem;
	struct platform_device *pdev;
	struct opp_table *opp_silver = NULL;
	struct opp_table *opp_gold = NULL;

	cpu_dev_silver = get_cpu_device(SILVER_LEAD);
	if (IS_ERR_OR_NULL(cpu_dev_silver))
		return PTR_ERR(cpu_dev_silver);

	cpu_dev_gold = get_cpu_device(SILVER_LEAD);
	if (IS_ERR_OR_NULL(cpu_dev_gold))
		return PTR_ERR(cpu_dev_gold);

	msm8996_version = qcom_cpufreq_kryo_get_msm_id();
	if (NUM_OF_MSM8996_VERSIONS == msm8996_version) {
		dev_err(cpu_dev_silver, "Not Snapdragon 820/821!");
		return -ENODEV;
	}

	np = dev_pm_opp_of_get_opp_desc_node(cpu_dev_silver);
	if (IS_ERR_OR_NULL(np))
		return PTR_ERR(np);

	if (!of_device_is_compatible(np, "operating-points-v2-kryo-cpu")) {
		ret = -ENOENT;
		goto free_np;
	}

	speedbin_nvmem = of_nvmem_cell_get(np, NULL);
	if (IS_ERR(speedbin_nvmem)) {
		ret = PTR_ERR(speedbin_nvmem);
		dev_err(cpu_dev_silver, "Could not get nvmem cell: %d\n", ret);
		goto free_np;
	}

	speedbin = nvmem_cell_read(speedbin_nvmem, &len);
	nvmem_cell_put(speedbin_nvmem);

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

	opp_silver = dev_pm_opp_set_supported_hw(cpu_dev_silver,&versions,1);
	if (IS_ERR_OR_NULL(opp_silver)) {
		dev_err(cpu_dev_silver, "Failed to set supported hardware\n");
		ret = PTR_ERR(opp_silver);
		goto free_np;
	}

	opp_gold = dev_pm_opp_set_supported_hw(cpu_dev_gold,&versions,1);
	if (IS_ERR_OR_NULL(opp_gold)) {
		dev_err(cpu_dev_gold, "Failed to set supported hardware\n");
		ret = PTR_ERR(opp_gold);
		goto free_opp_silver;
	}

	pdev = platform_device_register_simple("cpufreq-dt", -1, NULL, 0);
	if (!IS_ERR_OR_NULL(pdev))
		goto out;

	ret = PTR_ERR(pdev);
	dev_err(cpu_dev_silver, "Failed to register platform device\n");
	dev_pm_opp_put_supported_hw(opp_gold);

free_opp_silver:
	dev_pm_opp_put_supported_hw(opp_silver);

free_np:
	of_node_put(np);

out:
	return ret;
}
late_initcall(qcom_cpufreq_kryo_driver_init);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. Kryo CPUfreq driver");
MODULE_LICENSE("GPL v2");
