/*
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author:
 *	Mikko Perttunen <mperttunen@nvidia.com>
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

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/thermal.h>

#include <dt-bindings/thermal/tegra124-soctherm.h>

#include "tegra_soctherm.h"

#define SENSOR_CONFIG0				0
#define SENSOR_CONFIG0_STOP			BIT(0)
#define SENSOR_CONFIG0_TALL_SHIFT		8
#define SENSOR_CONFIG0_TCALC_OVER		BIT(4)
#define SENSOR_CONFIG0_OVER			BIT(3)
#define SENSOR_CONFIG0_CPTR_OVER		BIT(2)

#define SENSOR_CONFIG1				4
#define SENSOR_CONFIG1_TSAMPLE_SHIFT		0
#define SENSOR_CONFIG1_TIDDQ_EN_SHIFT		15
#define SENSOR_CONFIG1_TEN_COUNT_SHIFT		24
#define SENSOR_CONFIG1_TEMP_ENABLE		BIT(31)

/*
 * SENSOR_CONFIG2 is defined in tegra_soctherm.h
 * because, it will be used by tegra_soctherm_fuse.c
 */

#define READBACK_VALUE_MASK			0xff00
#define READBACK_VALUE_SHIFT			8
#define READBACK_ADD_HALF			BIT(7)
#define READBACK_NEGATE				BIT(1)

/* get val from register(r) mask bits(m) */
#define REG_GET_MASK(r, m)	(((r) & (m)) >> (ffs(m) - 1))
/* set val(v) to mask bits(m) of register(r) */
#define REG_SET_MASK(r, m, v)	(((r) & ~(m)) | \
				 (((v) & (m >> (ffs(m) - 1))) << (ffs(m) - 1)))

struct tegra_thermctl_zone {
	void __iomem *reg;
	u32 mask;
};

struct tegra_soctherm {
	struct reset_control *reset;
	struct clk *clock_tsensor;
	struct clk *clock_soctherm;
	void __iomem *regs;

	struct thermal_zone_device *thermctl_tzs[TEGRA124_SOCTHERM_SENSOR_NUM];
};

static int enable_tsensor(struct tegra_soctherm *tegra,
			  struct tegra_tsensor *sensor,
			  const struct tsensor_shared_calibration *shared)
{
	void __iomem *base = tegra->regs + sensor->base;
	unsigned int val;
	int err;

	err = tegra_soctherm_calculate_tsensor_calibration(sensor, shared);
	if (err)
		return err;

	val = sensor->config->tall << SENSOR_CONFIG0_TALL_SHIFT;
	writel(val, base + SENSOR_CONFIG0);

	val  = (sensor->config->tsample - 1) << SENSOR_CONFIG1_TSAMPLE_SHIFT;
	val |= sensor->config->tiddq_en << SENSOR_CONFIG1_TIDDQ_EN_SHIFT;
	val |= sensor->config->ten_count << SENSOR_CONFIG1_TEN_COUNT_SHIFT;
	val |= SENSOR_CONFIG1_TEMP_ENABLE;
	writel(val, base + SENSOR_CONFIG1);

	writel(sensor->calib, base + SENSOR_CONFIG2);

	return 0;
}

/*
 * Translate from soctherm readback format to millicelsius.
 * The soctherm readback format in bits is as follows:
 *   TTTTTTTT H______N
 * where T's contain the temperature in Celsius,
 * H denotes an addition of 0.5 Celsius and N denotes negation
 * of the final value.
 */
static int translate_temp(u16 val)
{
	long t;

	t = ((val & READBACK_VALUE_MASK) >> READBACK_VALUE_SHIFT) * 1000;
	if (val & READBACK_ADD_HALF)
		t += 500;
	if (val & READBACK_NEGATE)
		t *= -1;

	return t;
}

static int tegra_thermctl_get_temp(void *data, int *out_temp)
{
	struct tegra_thermctl_zone *zone = data;
	u32 val;

	val = readl(zone->reg);
	val = REG_GET_MASK(val, zone->mask);
	*out_temp = translate_temp(val);

	return 0;
}

static const struct thermal_zone_of_device_ops tegra_of_thermal_ops = {
	.get_temp = tegra_thermctl_get_temp,
};

int tegra_soctherm_probe(struct platform_device *pdev,
			 struct tegra_tsensor *tsensors,
			 const struct tegra_tsensor_group **ttgs,
			 const struct tegra_soctherm_fuse *tfuse)
{
	struct tegra_soctherm *tegra;
	struct thermal_zone_device *tz;
	struct tsensor_shared_calibration shared_calib;
	struct resource *res;
	unsigned int i;
	int err;
	u32 pdiv, hotspot;

	tegra = devm_kzalloc(&pdev->dev, sizeof(*tegra), GFP_KERNEL);
	if (!tegra)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	tegra->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(tegra->regs))
		return PTR_ERR(tegra->regs);

	tegra->reset = devm_reset_control_get(&pdev->dev, "soctherm");
	if (IS_ERR(tegra->reset)) {
		dev_err(&pdev->dev, "can't get soctherm reset\n");
		return PTR_ERR(tegra->reset);
	}

	tegra->clock_tsensor = devm_clk_get(&pdev->dev, "tsensor");
	if (IS_ERR(tegra->clock_tsensor)) {
		dev_err(&pdev->dev, "can't get tsensor clock\n");
		return PTR_ERR(tegra->clock_tsensor);
	}

	tegra->clock_soctherm = devm_clk_get(&pdev->dev, "soctherm");
	if (IS_ERR(tegra->clock_soctherm)) {
		dev_err(&pdev->dev, "can't get soctherm clock\n");
		return PTR_ERR(tegra->clock_soctherm);
	}

	reset_control_assert(tegra->reset);

	err = clk_prepare_enable(tegra->clock_soctherm);
	if (err)
		return err;

	err = clk_prepare_enable(tegra->clock_tsensor);
	if (err) {
		clk_disable_unprepare(tegra->clock_soctherm);
		return err;
	}

	reset_control_deassert(tegra->reset);

	/* Initialize raw sensors */

	err = tegra_soctherm_calculate_shared_calibration(tfuse, &shared_calib);
	if (err)
		goto disable_clocks;

	for (i = 0; tsensors[i].name; ++i) {
		err = enable_tsensor(tegra, tsensors + i, &shared_calib);
		if (err)
			goto disable_clocks;
	}

	/* program pdiv and hotspot offsets per THERM */
	pdiv = readl(tegra->regs + SENSOR_PDIV);
	hotspot = readl(tegra->regs + SENSOR_HOTSPOT_OFF);
	for (i = 0; i < TEGRA124_SOCTHERM_SENSOR_NUM; ++i) {
		pdiv = REG_SET_MASK(pdiv, ttgs[i]->pdiv_mask,
				   ttgs[i]->pdiv);
		if (ttgs[i]->id != TEGRA124_SOCTHERM_SENSOR_PLLX)
			hotspot =  REG_SET_MASK(hotspot,
						ttgs[i]->pllx_hotspot_mask,
						ttgs[i]->pllx_hotspot_diff);
	}
	writel(pdiv, tegra->regs + SENSOR_PDIV);
	writel(hotspot, tegra->regs + SENSOR_HOTSPOT_OFF);

	/* Initialize thermctl sensors */

	for (i = 0; i < TEGRA124_SOCTHERM_SENSOR_NUM; ++i) {
		struct tegra_thermctl_zone *zone =
			devm_kzalloc(&pdev->dev, sizeof(*zone), GFP_KERNEL);
		if (!zone) {
			err = -ENOMEM;
			goto unregister_tzs;
		}

		zone->reg = tegra->regs + ttgs[i]->sensor_temp_offset;
		zone->mask = ttgs[i]->sensor_temp_mask;

		tz = thermal_zone_of_sensor_register(&pdev->dev,
						     ttgs[i]->id, zone,
						     &tegra_of_thermal_ops);
		if (IS_ERR(tz)) {
			err = PTR_ERR(tz);
			dev_err(&pdev->dev, "failed to register sensor: %d\n",
				err);
			goto unregister_tzs;
		}

		tegra->thermctl_tzs[ttgs[i]->id] = tz;
	}

	return 0;

unregister_tzs:
	while (i--)
		thermal_zone_of_sensor_unregister(&pdev->dev,
						  tegra->thermctl_tzs[i]);

disable_clocks:
	clk_disable_unprepare(tegra->clock_tsensor);
	clk_disable_unprepare(tegra->clock_soctherm);

	return err;
}

int tegra_soctherm_remove(struct platform_device *pdev)
{
	struct tegra_soctherm *tegra = platform_get_drvdata(pdev);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(tegra->thermctl_tzs); ++i) {
		thermal_zone_of_sensor_unregister(&pdev->dev,
						  tegra->thermctl_tzs[i]);
	}

	clk_disable_unprepare(tegra->clock_tsensor);
	clk_disable_unprepare(tegra->clock_soctherm);

	return 0;
}

MODULE_AUTHOR("Mikko Perttunen <mperttunen@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA Tegra SOCTHERM thermal management driver");
MODULE_LICENSE("GPL v2");
