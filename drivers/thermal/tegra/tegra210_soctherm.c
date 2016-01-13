/*
 * Copyright (c) 2014-2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <soc/tegra/fuse.h>

#include <dt-bindings/thermal/tegra124-soctherm.h>

#include "tegra_soctherm.h"

#define TEGRA210_THERMTRIP_ANY_EN_MASK		(0x1 << 31)
#define TEGRA210_THERMTRIP_MEM_EN_MASK		(0x1 << 30)
#define TEGRA210_THERMTRIP_GPU_EN_MASK		(0x1 << 29)
#define TEGRA210_THERMTRIP_CPU_EN_MASK		(0x1 << 28)
#define TEGRA210_THERMTRIP_TSENSE_EN_MASK	(0x1 << 27)
#define TEGRA210_THERMTRIP_GPUMEM_THRESH_MASK	(0x1ff << 18)
#define TEGRA210_THERMTRIP_CPU_THRESH_MASK	(0x1ff << 9)
#define TEGRA210_THERMTRIP_TSENSE_THRESH_MASK	0x1ff

#define TEGRA210_THRESH_GRAIN			500

static const struct tegra_tsensor_configuration tegra210_tsensor_config = {
	.tall = 16300,
	.tiddq_en = 1,
	.ten_count = 1,
	.tsample = 120,
	.tsample_ate = 480,
};

static const struct tegra_tsensor_group tegra210_tsensor_group_cpu = {
	.id				= TEGRA124_SOCTHERM_SENSOR_CPU,
	.name				= "cpu",
	.sensor_temp_offset		= SENSOR_TEMP1,
	.sensor_temp_mask		= SENSOR_TEMP1_CPU_TEMP_MASK,
	.pdiv				= 8,
	.pdiv_ate			= 8,
	.pdiv_mask			= SENSOR_PDIV_CPU_MASK,
	.pllx_hotspot_diff		= 10,
	.pllx_hotspot_mask		= SENSOR_HOTSPOT_CPU_MASK,
	.thermtrip_any_en_mask		= TEGRA210_THERMTRIP_ANY_EN_MASK,
	.thermtrip_enable_mask		= TEGRA210_THERMTRIP_CPU_EN_MASK,
	.thermtrip_threshold_mask	= TEGRA210_THERMTRIP_CPU_THRESH_MASK,
	.thresh_grain			= TEGRA210_THRESH_GRAIN,
};

static const struct tegra_tsensor_group tegra210_tsensor_group_gpu = {
	.id				= TEGRA124_SOCTHERM_SENSOR_GPU,
	.name				= "gpu",
	.sensor_temp_offset		= SENSOR_TEMP1,
	.sensor_temp_mask		= SENSOR_TEMP1_GPU_TEMP_MASK,
	.pdiv				= 8,
	.pdiv_ate			= 8,
	.pdiv_mask			= SENSOR_PDIV_GPU_MASK,
	.pllx_hotspot_diff		= 5,
	.pllx_hotspot_mask		= SENSOR_HOTSPOT_GPU_MASK,
	.thermtrip_any_en_mask		= TEGRA210_THERMTRIP_ANY_EN_MASK,
	.thermtrip_enable_mask		= TEGRA210_THERMTRIP_GPU_EN_MASK,
	.thermtrip_threshold_mask	= TEGRA210_THERMTRIP_GPUMEM_THRESH_MASK,
	.thresh_grain			= TEGRA210_THRESH_GRAIN,
};

static const struct tegra_tsensor_group tegra210_tsensor_group_pll = {
	.id				= TEGRA124_SOCTHERM_SENSOR_PLLX,
	.name				= "pll",
	.sensor_temp_offset		= SENSOR_TEMP2,
	.sensor_temp_mask		= SENSOR_TEMP2_PLLX_TEMP_MASK,
	.pdiv				= 8,
	.pdiv_ate			= 8,
	.pdiv_mask			= SENSOR_PDIV_PLLX_MASK,
	.thermtrip_any_en_mask		= TEGRA210_THERMTRIP_ANY_EN_MASK,
	.thermtrip_enable_mask		= TEGRA210_THERMTRIP_TSENSE_EN_MASK,
	.thermtrip_threshold_mask	= TEGRA210_THERMTRIP_TSENSE_THRESH_MASK,
	.thresh_grain			= TEGRA210_THRESH_GRAIN,
};

static const struct tegra_tsensor_group tegra210_tsensor_group_mem = {
	.id				= TEGRA124_SOCTHERM_SENSOR_MEM,
	.name				= "mem",
	.sensor_temp_offset		= SENSOR_TEMP2,
	.sensor_temp_mask		= SENSOR_TEMP2_MEM_TEMP_MASK,
	.pdiv				= 8,
	.pdiv_ate			= 8,
	.pdiv_mask			= SENSOR_PDIV_MEM_MASK,
	.pllx_hotspot_diff		= 0,
	.pllx_hotspot_mask		= SENSOR_HOTSPOT_MEM_MASK,
	.thermtrip_any_en_mask		= TEGRA210_THERMTRIP_ANY_EN_MASK,
	.thermtrip_enable_mask		= TEGRA210_THERMTRIP_MEM_EN_MASK,
	.thermtrip_threshold_mask	= TEGRA210_THERMTRIP_GPUMEM_THRESH_MASK,
	.thresh_grain			= TEGRA210_THRESH_GRAIN,
};

static const struct tegra_tsensor_group *
tegra210_tsensor_groups[TEGRA124_SOCTHERM_SENSOR_NUM] = {
	&tegra210_tsensor_group_cpu,
	&tegra210_tsensor_group_gpu,
	&tegra210_tsensor_group_pll,
	&tegra210_tsensor_group_mem,
};

static struct tegra_tsensor tegra210_tsensors[] = {
	{
		.name = "cpu0",
		.base = 0xc0,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x098,
		.fuse_corr_alpha = 1085000,
		.fuse_corr_beta = 3244200,
		.group = &tegra210_tsensor_group_cpu,
	},
	{
		.name = "cpu1",
		.base = 0xe0,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x084,
		.fuse_corr_alpha = 1126200,
		.fuse_corr_beta = -67500,
		.group = &tegra210_tsensor_group_cpu,
	},
	{
		.name = "cpu2",
		.base = 0x100,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x088,
		.fuse_corr_alpha = 1098400,
		.fuse_corr_beta = 2251100,
		.group = &tegra210_tsensor_group_cpu,
	},
	{
		.name = "cpu3",
		.base = 0x120,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x12c,
		.fuse_corr_alpha = 1108000,
		.fuse_corr_beta = 602700,
		.group = &tegra210_tsensor_group_cpu,
	},
	{
		.name = "mem0",
		.base = 0x140,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x158,
		.fuse_corr_alpha = 1069200,
		.fuse_corr_beta = 3549900,
		.group = &tegra210_tsensor_group_mem,
	},
	{
		.name = "mem1",
		.base = 0x160,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x15c,
		.fuse_corr_alpha = 1173700,
		.fuse_corr_beta = -6263600,
		.group = &tegra210_tsensor_group_mem,
	},
	{
		.name = "gpu",
		.base = 0x180,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x154,
		.fuse_corr_alpha = 1074300,
		.fuse_corr_beta = 2734900,
		.group = &tegra210_tsensor_group_gpu,
	},
	{
		.name = "pllx",
		.base = 0x1a0,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x160,
		.fuse_corr_alpha = 1039700,
		.fuse_corr_beta = 6829100,
		.group = &tegra210_tsensor_group_pll,
	},
	{ .name = NULL },
};

/*
 * Mask/shift bits in FUSE_TSENSOR_COMMON and
 * FUSE_TSENSOR_COMMON, which are described in
 * tegra_soctherm_fuse.c
 */
static const struct tegra_soctherm_fuse tegra210_soctherm_fuse = {
	.fuse_base_cp_mask = 0x3ff << 11,
	.fuse_base_cp_shift = 11,
	.fuse_base_ft_mask = 0x7ff << 21,
	.fuse_base_ft_shift = 21,
	.fuse_shift_ft_mask = 0x1f << 6,
	.fuse_shift_ft_shift = 6,
	.fuse_spare_realignment = 0,
};

static const struct of_device_id tegra210_soctherm_of_match[] = {
	{ .compatible = "nvidia,tegra210-soctherm" },
	{ },
};
MODULE_DEVICE_TABLE(of, tegra_soctherm_of_match);

#define TEGRA210_FUSE_CP_REV 0x90

static int tegra210_soctherm_probe(struct platform_device *pdev)
{
	u32 rev;

	tegra_fuse_readl(TEGRA210_FUSE_CP_REV, &rev);
	pr_debug("tsosc: CP rev %d.%d\n", (rev & 0x1f), ((rev >> 5) & 0x3f));

	return tegra_soctherm_probe(pdev,
				    tegra210_tsensors,
				    tegra210_tsensor_groups,
				    &tegra210_soctherm_fuse);
}

static struct platform_driver tegra210_soctherm_driver = {
	.probe = tegra210_soctherm_probe,
	.remove = tegra_soctherm_remove,
	.driver = {
		.name = "tegra210_soctherm",
		.owner = THIS_MODULE,
		.of_match_table = tegra210_soctherm_of_match,
	},
};
module_platform_driver(tegra210_soctherm_driver);

MODULE_AUTHOR("NVIDIA");
MODULE_DESCRIPTION("Tegra210 SOCTHERM thermal management driver");
MODULE_LICENSE("GPL v2");
