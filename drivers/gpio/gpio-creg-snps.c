// SPDX-License-Identifier: GPL-2.0+
//
// Synopsys CREG (Control REGisers) GPIO driver
//
// Copyright (C) 2018 Synopsys
// Author: Eugeniy Paltsev <Eugeniy.Paltsev@synopsys.com>

#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/of_gpio.h>
#include <linux/io.h>
#include <linux/of_platform.h>
#include <linux/module.h>

#include "gpiolib.h"

/*
 * GPIO via CREG (Control REGisers) driver
 *
 * 31              11        8         7        5         0   < bit number
 * |                |        |         |        |         |
 * [    not used    | gpio-1 | shift-1 | gpio-0 | shift-0 ]   < 32 bit register
 *                      ^                  ^
 *                      |                  |
 *                      |           write 0x2 == set output to "1" (on)
 *                      |           write 0x3 == set output to "0" (off)
 *                      |
 *               write 0x1 == set output to "1" (on)
 *               write 0x4 == set output to "0" (off)
 */

#define MAX_GPIO	32

struct creg_gpio {
	struct of_mm_gpio_chip mmchip;
	spinlock_t lock;

	u32 shift[MAX_GPIO];
	u32 on[MAX_GPIO];
	u32 off[MAX_GPIO];
	u32 bit_per_gpio[MAX_GPIO];
};

static void _creg_gpio_set(struct creg_gpio *hcg, unsigned int gpio, u32 val)
{
	u32 reg, reg_shift;
	unsigned long flags;
	int i;

	reg_shift = hcg->shift[gpio];
	for (i = 0; i < gpio; i++)
		reg_shift += hcg->bit_per_gpio[i] + hcg->shift[i];

	spin_lock_irqsave(&hcg->lock, flags);
	reg = readl(hcg->mmchip.regs);
	reg &= ~(GENMASK(hcg->bit_per_gpio[i] - 1, 0) << reg_shift);
	reg |=  (val << reg_shift);
	writel(reg, hcg->mmchip.regs);
	spin_unlock_irqrestore(&hcg->lock, flags);
}

static void creg_gpio_set(struct gpio_chip *gc, unsigned int gpio, int val)
{
	struct creg_gpio *hcg = gpiochip_get_data(gc);
	u32 value = val ? hcg->on[gpio] : hcg->off[gpio];

	_creg_gpio_set(hcg, gpio, value);
}

static int creg_gpio_dir_out(struct gpio_chip *gc, unsigned int gpio, int val)
{
	creg_gpio_set(gc, gpio, val);

	return 0;
}

static int creg_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	return 0; /* output */
}

static int creg_gpio_xlate(struct gpio_chip *gc,
			   const struct of_phandle_args *gpiospec, u32 *flags)
{
	if (gpiospec->args_count != 1) {
		dev_err(&gc->gpiodev->dev, "invalid args_count: %d\n",
			gpiospec->args_count);
		return -EINVAL;
	}

	if (gpiospec->args[0] >= gc->ngpio) {
		dev_err(&gc->gpiodev->dev, "gpio number is too big: %d\n",
			gpiospec->args[0]);
		return -EINVAL;
	}

	return gpiospec->args[0];
}

static int creg_gpio_validate_pgv(struct device *dev, struct creg_gpio *hcg,
				  int i, u32 *defaults, bool use_defaults)
{
	if (hcg->bit_per_gpio[i] < 1 || hcg->bit_per_gpio[i] > 8) {
		dev_err(dev, "'bit-per-line[%d]' is out of bounds\n", i);
		return -EINVAL;
	}

	/* Check that on valiue suits it's placeholder */
	if (GENMASK(31, hcg->bit_per_gpio[i]) & hcg->on[i]) {
		dev_err(dev, "'on-val[%d]' can't be more than %lu\n",
			i, GENMASK(hcg->bit_per_gpio[i] - 1, 0));
		return -EINVAL;
	}

	/* Check that off valiue suits it's placeholder */
	if (GENMASK(31, hcg->bit_per_gpio[i]) & hcg->off[i]) {
		dev_err(dev, "'off-val[%d]' can't be more than %lu\n",
			i, GENMASK(hcg->bit_per_gpio[i] - 1, 0));
		return -EINVAL;
	}

	/* Check that default valiue suits it's placeholder */
	if (use_defaults && (GENMASK(31, hcg->bit_per_gpio[i]) & defaults[i])) {
		dev_err(dev, "'default-val[%d]' can't be more than %lu\n",
			i, GENMASK(hcg->bit_per_gpio[i] - 1, 0));
		return -EINVAL;
	}

	if (hcg->on[i] == hcg->off[i]) {
		dev_err(dev, "'off-val[%d]' and 'on-val[%d]' can't be equal\n",
			i, i);
		return -EINVAL;
	}

	return 0;
}

static int creg_gpio_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct creg_gpio *hcg;

	u32 defaults[MAX_GPIO];
	bool use_defaults;
	u32 ngpio, reg_len = 0;
	int ret, i;

	hcg = devm_kzalloc(dev, sizeof(struct creg_gpio), GFP_KERNEL);
	if (!hcg)
		return -ENOMEM;

	if (of_property_read_u32(np, "snps,ngpios", &ngpio)) {
		dev_err(dev, "'ngpios' isn't set\n");
		return -EINVAL;
	}

	if (ngpio < 1 || ngpio > MAX_GPIO) {
		dev_err(dev, "'ngpios' is out of bounds\n");
		return -EINVAL;
	}

	if (of_property_read_u32_array(np, "snps,shift", hcg->shift, ngpio)) {
		dev_err(dev, "'shift' is set incorrectly\n");
		return -EINVAL;
	}

	if (of_property_read_u32_array(np, "snps,bit-per-line",
				       hcg->bit_per_gpio, ngpio)) {
		dev_err(dev, "'bit-per-line' is set incorrectly\n");
		return -EINVAL;
	}

	if (of_property_read_u32_array(np, "snps,on-val", hcg->on, ngpio)) {
		dev_err(dev, "'on-val' is set incorrectly\n");
		return -EINVAL;
	}

	if (of_property_read_u32_array(np, "snps,off-val", hcg->off, ngpio)) {
		dev_err(dev, "'off-val' is set incorrectly\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_array(np, "snps,default-val", defaults, ngpio);
	if (ret && ret != -EINVAL) {
		dev_err(dev, "'default-val' is set incorrectly\n");
		return ret;
	}
	use_defaults = !ret;

	for (i = 0; i < ngpio; i++) {
		if (creg_gpio_validate_pgv(dev, hcg, i, defaults, use_defaults))
			return -EINVAL;

		reg_len += hcg->shift[i] + hcg->bit_per_gpio[i];
	}

	/* Check that we suit in 32 bit register */
	if (reg_len > 32) {
		dev_err(dev,
			"32-bit io register overflow: attempt to use %u bits\n",
			reg_len);
		return -EINVAL;
	}

	spin_lock_init(&hcg->lock);

	hcg->mmchip.gc.ngpio = ngpio;
	hcg->mmchip.gc.set = creg_gpio_set;
	hcg->mmchip.gc.get_direction = creg_gpio_get_direction;
	hcg->mmchip.gc.direction_output = creg_gpio_dir_out;
	hcg->mmchip.gc.of_xlate = creg_gpio_xlate;
	hcg->mmchip.gc.of_gpio_n_cells = 1;

	ret = of_mm_gpiochip_add_data(pdev->dev.of_node, &hcg->mmchip, hcg);
	if (ret)
		return ret;

	/* Setup default GPIO value if we have "snps,default-val" array */
	if (use_defaults)
		for (i = 0; i < ngpio; i++)
			_creg_gpio_set(hcg, i, defaults[i]);

	dev_info(dev, "GPIO controller with %d gpios probed\n", ngpio);

	return 0;
}

static const struct of_device_id creg_gpio_ids[] = {
	{ .compatible = "snps,creg-gpio" },
	{ }
};

static struct platform_driver creg_gpio_snps_driver = {
	.driver = {
		.name = "snps-creg-gpio",
		.of_match_table = creg_gpio_ids,
	},
	.probe  = creg_gpio_probe,
};
builtin_platform_driver(creg_gpio_snps_driver);
