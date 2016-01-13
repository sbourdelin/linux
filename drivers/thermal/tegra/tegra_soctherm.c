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

#include <linux/debugfs.h>
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
#define SENSOR_CONFIG0_CPTR_OVER		BIT(2)
#define SENSOR_CONFIG0_OVER			BIT(3)
#define SENSOR_CONFIG0_TCALC_OVER		BIT(4)
#define SENSOR_CONFIG0_TALL_MASK		(0xfffff << 8)
#define SENSOR_CONFIG0_TALL_SHIFT		8

#define SENSOR_CONFIG1				4
#define SENSOR_CONFIG1_TSAMPLE_MASK		0x3ff
#define SENSOR_CONFIG1_TSAMPLE_SHIFT		0
#define SENSOR_CONFIG1_TIDDQ_EN_MASK		(0x3f << 15)
#define SENSOR_CONFIG1_TIDDQ_EN_SHIFT		15
#define SENSOR_CONFIG1_TEN_COUNT_MASK		(0x3f << 24)
#define SENSOR_CONFIG1_TEN_COUNT_SHIFT		24
#define SENSOR_CONFIG1_TEMP_ENABLE		BIT(31)

/*
 * SENSOR_CONFIG2 is defined in tegra_soctherm.h
 * because, it will be used by tegra_soctherm_fuse.c
 */

#define SENSOR_STATUS0				0xc
#define SENSOR_STATUS0_VALID_MASK		BIT(31)
#define SENSOR_STATUS0_CAPTURE_MASK		0xffff

#define SENSOR_STATUS1				0x10
#define SENSOR_STATUS1_TEMP_VALID_MASK		BIT(31)
#define SENSOR_STATUS1_TEMP_MASK		0xffff

#define READBACK_VALUE_MASK			0xff00
#define READBACK_VALUE_SHIFT			8
#define READBACK_ADD_HALF			BIT(7)
#define READBACK_NEGATE				BIT(1)

/* get val from register(r) mask bits(m) */
#define REG_GET_MASK(r, m)	(((r) & (m)) >> (ffs(m) - 1))
/* set val(v) to mask bits(m) of register(r) */
#define REG_SET_MASK(r, m, v)	(((r) & ~(m)) | \
				 (((v) & (m >> (ffs(m) - 1))) << (ffs(m) - 1)))

static const int min_low_temp = -127000;
static const int max_high_temp = 127000;

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
	struct tegra_tsensor *tsensors;
	const struct tegra_tsensor_group **sensor_groups;
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

/**
 * enforce_temp_range() - check and enforce temperature range [min, max]
 * @trip_temp: the trip temperature to check
 *
 * Checks and enforces the permitted temperature range that SOC_THERM
 * HW can support This is
 * done while taking care of precision.
 *
 * Return: The precision adjusted capped temperature in millicelsius.
 */
static int enforce_temp_range(struct device *dev, int trip_temp)
{
	int temp;

	temp = clamp_val(trip_temp, min_low_temp, max_high_temp);
	if (temp != trip_temp)
		dev_info(dev, "soctherm: trip temp %d forced to %d\n",
			 trip_temp, temp);
	return temp;
}

/**
 * thermtrip_program() - Configures the hardware to shut down the
 * system if a given sensor group reaches a given temperature
 * @dev: ptr to the struct device for the SOC_THERM IP block
 * @sg: pointer to the sensor group to set the thermtrip temperature for
 * @trip_temp: the temperature in millicelsius to trigger the thermal trip at
 *
 * Sets the thermal trip threshold of the given sensor group to be the
 * @trip_temp.  If this threshold is crossed, the hardware will shut
 * down.
 *
 * Note that, although @trip_temp is specified in millicelsius, the
 * hardware is programmed in degrees Celsius.
 *
 * Return: 0 upon success, or %-EINVAL upon failure.
 */
static int thermtrip_program(struct device *dev,
			     const struct tegra_tsensor_group *sg,
			     int trip_temp)
{
	struct tegra_soctherm *ts = dev_get_drvdata(dev);
	int temp;
	u32 r;

	if (!dev || !sg)
		return -EINVAL;

	if (!sg->thermtrip_threshold_mask)
		return -EINVAL;

	temp = enforce_temp_range(dev, trip_temp) / sg->thresh_grain;

	r = readl(ts->regs + THERMCTL_THERMTRIP_CTL);
	r = REG_SET_MASK(r, sg->thermtrip_threshold_mask, temp);
	r = REG_SET_MASK(r, sg->thermtrip_enable_mask, 1);
	r = REG_SET_MASK(r, sg->thermtrip_any_en_mask, 0);
	writel(r, ts->regs + THERMCTL_THERMTRIP_CTL);

	return 0;
}

/**
 * tegra_soctherm_thermtrip() - configure thermal shutdown from DT data
 * @dev: struct device * of the SOC_THERM instance
 *
 * Configure the SOC_THERM "THERMTRIP" feature, using data from DT.
 * After it's been configured, THERMTRIP will take action when the
 * configured SoC thermal sensor group reaches a certain temperature.
 *
 * SOC_THERM registers are in the VDD_SOC voltage domain.  This means
 * that SOC_THERM THERMTRIP programming does not survive an LP0/SC7
 * transition, unless this driver has been modified to save those
 * registers before entering SC7 and restore them upon exiting SC7.
 *
 * Return: 0 upon success, or a negative error code on failure.
 * "Success" does not mean that thermtrip was enabled; it could also
 * mean that no "thermtrip" node was found in DT.  THERMTRIP has been
 * enabled successfully when a message similar to this one appears on
 * the serial console: "thermtrip: will shut down when sensor group
 * XXX reaches YYYYYY millidegrees C"
 */
static int tegra_soctherm_thermtrip(struct device *dev)
{
	struct tegra_soctherm *ts = dev_get_drvdata(dev);
	const struct tegra_tsensor_group **ttgs =  ts->sensor_groups;
	struct device_node *dn;
	int i;

	dn = of_find_node_by_name(dev->of_node, "hw-trips");
	if (!dn) {
		dev_info(dev, "thermtrip: no DT node - not enabling\n");
		return -ENODEV;
	}

	for (i = 0; i < TEGRA124_SOCTHERM_SENSOR_NUM; ++i) {
		const struct tegra_tsensor_group *sg = ttgs[i];
		struct device_node *sgdn;
		u32 temperature;
		int r;

		sgdn = of_find_node_by_name(dn, sg->name);
		if (!sgdn) {
			dev_info(dev,
				 "thermtrip: %s: skip due to no configuration\n",
				 sg->name);
			continue;
		}

		r = of_property_read_u32(sgdn, "therm-temp", &temperature);
		if (r) {
			dev_err(dev,
				"thermtrip: %s: missing temperature property\n",
				sg->name);
			continue;
		}

		r = thermtrip_program(dev, sg, temperature);
		if (r) {
			dev_err(dev, "thermtrip: %s: error during enable\n",
				sg->name);
			continue;
		}

		dev_info(dev,
			 "thermtrip: will shut down when %s reaches %d mC\n",
			 sg->name, temperature);
	}

	return 0;
}

#ifdef CONFIG_DEBUG_FS
static const struct tegra_tsensor_group *find_sensor_group_by_id(
						struct tegra_soctherm *ts,
						int id)
{
	int i;

	if ((id < 0) || (id > TEGRA124_SOCTHERM_SENSOR_NUM))
		return ERR_PTR(-EINVAL);

	for (i = 0; i < TEGRA124_SOCTHERM_SENSOR_NUM; i++)
		if (ts->sensor_groups[i]->id == id)
			return ts->sensor_groups[i];

	return NULL;
}

static int thermtrip_read(struct platform_device *pdev,
			  int id, u32 *temp)
{
	struct tegra_soctherm *ts = platform_get_drvdata(pdev);
	const struct tegra_tsensor_group *sg;
	u32 state;
	int r;

	sg = find_sensor_group_by_id(ts, id);
	if (IS_ERR(sg)) {
		dev_err(&pdev->dev, "Read thermtrip failed\n");
		return -EINVAL;
	}

	r = readl(ts->regs + THERMCTL_THERMTRIP_CTL);
	state = REG_GET_MASK(r, sg->thermtrip_threshold_mask);
	state *= sg->thresh_grain;
	*temp = state;

	return 0;
}

static int thermtrip_write(struct platform_device *pdev,
			   int id, int temp)
{
	struct tegra_soctherm *ts = platform_get_drvdata(pdev);
	const struct tegra_tsensor_group *sg;
	u32 state;
	int r;

	sg = find_sensor_group_by_id(ts, id);
	if (IS_ERR(sg)) {
		dev_err(&pdev->dev, "Read thermtrip failed\n");
		return -EINVAL;
	}

	r = readl(ts->regs + THERMCTL_THERMTRIP_CTL);
	state = REG_GET_MASK(r, sg->thermtrip_enable_mask);
	if (!state) {
		dev_err(&pdev->dev, "%s thermtrip not enabled.\n", sg->name);
		return -EINVAL;
	}

	r = thermtrip_program(&pdev->dev, sg, temp);
	if (r) {
		dev_err(&pdev->dev, "Set %s thermtrip failed.\n", sg->name);
		return r;
	}

	return 0;
}

#define DEFINE_THERMTRIP_SIMPLE_ATTR(__name, __id)			\
static int __name##_show(void *data, u64 *val)				\
{									\
	struct platform_device *pdev = data;				\
	u32 temp;							\
	int r;								\
									\
	r = thermtrip_read(pdev, __id, &temp);				\
	if (r < 0)							\
		return 0;						\
	*val = temp;							\
									\
	return 0;							\
}									\
									\
static int __name##_set(void *data, u64 val)				\
{									\
	struct platform_device *pdev = data;				\
	int r;								\
									\
	r = thermtrip_write(pdev, __id, val);				\
	if (r)								\
		return r;						\
	else								\
		return 0;						\
}									\
DEFINE_SIMPLE_ATTRIBUTE(__name##_fops, __name##_show, __name##_set, "%lld\n")

DEFINE_THERMTRIP_SIMPLE_ATTR(cpu_thermtrip, TEGRA124_SOCTHERM_SENSOR_CPU);
DEFINE_THERMTRIP_SIMPLE_ATTR(gpu_thermtrip, TEGRA124_SOCTHERM_SENSOR_GPU);
DEFINE_THERMTRIP_SIMPLE_ATTR(pll_thermtrip, TEGRA124_SOCTHERM_SENSOR_PLLX);

static int regs_show(struct seq_file *s, void *data)
{
	struct platform_device *pdev = s->private;
	struct tegra_soctherm *ts = platform_get_drvdata(pdev);
	struct tegra_tsensor *tsensors = ts->tsensors;
	const struct tegra_tsensor_group **ttgs = ts->sensor_groups;
	u32 r, state;
	int i;

	seq_puts(s, "-----TSENSE (convert HW)-----\n");

	for (i = 0; tsensors[i].name; i++) {
		r = readl(ts->regs + tsensors[i].base + SENSOR_CONFIG1);
		state = REG_GET_MASK(r, SENSOR_CONFIG1_TEMP_ENABLE);
		if (!state)
			continue;

		seq_printf(s, "%s: ", tsensors[i].name);

		seq_printf(s, "En(%d) ", state);
		state = REG_GET_MASK(r, SENSOR_CONFIG1_TIDDQ_EN_MASK);
		seq_printf(s, "tiddq(%d) ", state);
		state = REG_GET_MASK(r, SENSOR_CONFIG1_TEN_COUNT_MASK);
		seq_printf(s, "ten_count(%d) ", state);
		state = REG_GET_MASK(r, SENSOR_CONFIG1_TSAMPLE_MASK);
		seq_printf(s, "tsample(%d) ", state + 1);

		r = readl(ts->regs + tsensors[i].base + SENSOR_STATUS1);
		state = REG_GET_MASK(r, SENSOR_STATUS1_TEMP_VALID_MASK);
		seq_printf(s, "Temp(%d/", state);
		state = REG_GET_MASK(r, SENSOR_STATUS1_TEMP_MASK);
		seq_printf(s, "%d) ", translate_temp(state));

		r = readl(ts->regs + tsensors[i].base + SENSOR_STATUS0);
		state = REG_GET_MASK(r, SENSOR_STATUS0_VALID_MASK);
		seq_printf(s, "Capture(%d/", state);
		state = REG_GET_MASK(r, SENSOR_STATUS0_CAPTURE_MASK);
		seq_printf(s, "%d) ", state);

		r = readl(ts->regs + tsensors[i].base + SENSOR_CONFIG0);
		state = REG_GET_MASK(r, SENSOR_CONFIG0_STOP);
		seq_printf(s, "Stop(%d) ", state);
		state = REG_GET_MASK(r, SENSOR_CONFIG0_TALL_MASK);
		seq_printf(s, "Tall(%d) ", state);
		state = REG_GET_MASK(r, SENSOR_CONFIG0_TCALC_OVER);
		seq_printf(s, "Over(%d/", state);
		state = REG_GET_MASK(r, SENSOR_CONFIG0_OVER);
		seq_printf(s, "%d/", state);
		state = REG_GET_MASK(r, SENSOR_CONFIG0_CPTR_OVER);
		seq_printf(s, "%d) ", state);

		r = readl(ts->regs + tsensors[i].base + SENSOR_CONFIG2);
		state = REG_GET_MASK(r, SENSOR_CONFIG2_THERMA_MASK);
		seq_printf(s, "Therm_A/B(%d/", state);
		state = REG_GET_MASK(r, SENSOR_CONFIG2_THERMB_MASK);
		seq_printf(s, "%d)\n", (s16)state);
	}

	r = readl(ts->regs + SENSOR_PDIV);
	seq_printf(s, "PDIV: 0x%x\n", r);

	r = readl(ts->regs + SENSOR_HOTSPOT_OFF);
	seq_printf(s, "HOTSPOT: 0x%x\n", r);

	seq_puts(s, "\n");
	seq_puts(s, "-----SOC_THERM-----\n");

	r = readl(ts->regs + SENSOR_TEMP1);
	state = REG_GET_MASK(r, SENSOR_TEMP1_CPU_TEMP_MASK);
	seq_printf(s, "Temperatures: CPU(%d) ", translate_temp(state));
	state = REG_GET_MASK(r, SENSOR_TEMP1_GPU_TEMP_MASK);
	seq_printf(s, " GPU(%d) ", translate_temp(state));
	r = readl(ts->regs + SENSOR_TEMP2);
	state = REG_GET_MASK(r, SENSOR_TEMP2_PLLX_TEMP_MASK);
	seq_printf(s, " PLLX(%d) ", translate_temp(state));
	state = REG_GET_MASK(r, SENSOR_TEMP2_MEM_TEMP_MASK);
	seq_printf(s, " MEM(%d)\n", translate_temp(state));

	r = readl(ts->regs + THERMCTL_THERMTRIP_CTL);
	state = REG_GET_MASK(r, ttgs[0]->thermtrip_any_en_mask);
	seq_printf(s, "ThermTRIP ANY En(%d)\n", state);
	for (i = 0; i < TEGRA124_SOCTHERM_SENSOR_NUM; i++) {
		state = REG_GET_MASK(r, ttgs[i]->thermtrip_enable_mask);
		seq_printf(s, "     %s En(%d) ", ttgs[i]->name, state);
		state = REG_GET_MASK(r, ttgs[i]->thermtrip_threshold_mask);
		state *= ttgs[i]->thresh_grain;
		seq_printf(s, "Thresh(%d)\n", state);
	}

	return 0;
}

static int regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, regs_show, inode->i_private);
}

static const struct file_operations regs_fops = {
	.open		= regs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int soctherm_debug_init(struct platform_device *pdev)
{
	struct dentry *tegra_soctherm_root;

	tegra_soctherm_root = debugfs_create_dir("tegra_soctherm", NULL);
	debugfs_create_file("regs", 0644, tegra_soctherm_root,
			    pdev, &regs_fops);
	debugfs_create_file("cpu_thermtrip", S_IRUGO | S_IWUSR,
			    tegra_soctherm_root, pdev, &cpu_thermtrip_fops);
	debugfs_create_file("gpu_thermtrip", S_IRUGO | S_IWUSR,
			    tegra_soctherm_root, pdev, &gpu_thermtrip_fops);
	debugfs_create_file("pll_thermtrip", S_IRUGO | S_IWUSR,
			    tegra_soctherm_root, pdev, &pll_thermtrip_fops);

	return 0;
}
#else
static inline int soctherm_debug_init(struct platform_device *pdev)
{ return 0; }
#endif

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

	dev_set_drvdata(&pdev->dev, tegra);

	tegra->tsensors = tsensors;
	tegra->sensor_groups = ttgs;

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
	tegra_soctherm_thermtrip(&pdev->dev);

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

	soctherm_debug_init(pdev);

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
