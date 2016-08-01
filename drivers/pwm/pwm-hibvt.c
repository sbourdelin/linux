/*
 * PWM Controller Driver for HiSilicon BVT SOCs
 *
 * Copyright (c) 2016 HiSilicon Technologies Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/reset.h>

#define PWM_CFG0_ADDR(x)    (((x) * 0x20) + 0x0)
#define PWM_CFG1_ADDR(x)    (((x) * 0x20) + 0x4)
#define PWM_CFG2_ADDR(x)    (((x) * 0x20) + 0x8)
#define PWM_CTRL_ADDR(x)    (((x) * 0x20) + 0xC)

#define PWM_ENABLE_SHIFT    0
#define PWM_ENABLE_MASK		BIT(0)

#define PWM_POLARITY_SHIFT	1
#define PWM_POLARITY_MASK	BIT(1)

#define PWM_KEEP_SHIFT	    2
#define PWM_KEEP_MASK	    BIT(2)

#define PWM_PERIOD_MASK	    GENMASK(31, 0)
#define PWM_DUTY_MASK	    GENMASK(31, 0)

struct hibvt_pwm_chip {
	struct pwm_chip	chip;
	struct clk	*clk;
	void __iomem	*mmio_base;
	struct reset_control *rstc;
};

static inline
struct hibvt_pwm_chip *to_hibvt_pwm_chip(struct pwm_chip *chip)
{
	return container_of(chip, struct hibvt_pwm_chip, chip);
}

static void hibvt_pwm_set_bits(void __iomem *base, unsigned int offset,
					unsigned int mask, unsigned int data)
{
	void __iomem *address = base + offset;
	unsigned int value;

	value = readl(address);
	value &= ~mask;
	value |= (data & mask);
	writel(value, address);
}

static int hibvt_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct hibvt_pwm_chip *hi_pwm_chip = to_hibvt_pwm_chip(chip);
	unsigned int offset;

	offset = PWM_CTRL_ADDR(pwm->hwpwm);
	hibvt_pwm_set_bits(hi_pwm_chip->mmio_base, offset,
			PWM_ENABLE_MASK, 0x1);

	return 0;
}

static void hibvt_pwm_disable(struct pwm_chip *chip,
					struct pwm_device *pwm)
{
	struct hibvt_pwm_chip *hi_pwm_chip = to_hibvt_pwm_chip(chip);
	unsigned int offset;

	offset = PWM_CTRL_ADDR(pwm->hwpwm);
	hibvt_pwm_set_bits(hi_pwm_chip->mmio_base, offset,
			PWM_ENABLE_MASK, 0x0);
}

static int hibvt_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
					int duty_cycle_ns, int period_ns)
{
	struct hibvt_pwm_chip *hi_pwm_chip = to_hibvt_pwm_chip(chip);
	unsigned int offset;
	unsigned int freq;
	unsigned int period_num, duty_num;

	freq = div_u64(clk_get_rate(hi_pwm_chip->clk), 1000000);

	period_num = div_u64(freq * period_ns, 1000);
	duty_num = div_u64(period_num * duty_cycle_ns, period_ns);

	offset = PWM_CFG0_ADDR(pwm->hwpwm);
	hibvt_pwm_set_bits(hi_pwm_chip->mmio_base, offset,
			PWM_PERIOD_MASK, period_num);

	offset = PWM_CFG1_ADDR(pwm->hwpwm);
	hibvt_pwm_set_bits(hi_pwm_chip->mmio_base, offset,
			PWM_DUTY_MASK, duty_num);

	return 0;
}

static int hibvt_pwm_set_polarity(struct pwm_chip *chip,
					struct pwm_device *pwm,
					enum pwm_polarity polarity)
{
	struct hibvt_pwm_chip *hi_pwm_chip = to_hibvt_pwm_chip(chip);
	unsigned int offset;

	offset = PWM_CTRL_ADDR(pwm->hwpwm);
	if (polarity == PWM_POLARITY_INVERSED)
		hibvt_pwm_set_bits(hi_pwm_chip->mmio_base, offset,
				PWM_POLARITY_MASK, (0x1 << PWM_POLARITY_SHIFT));
	else
		hibvt_pwm_set_bits(hi_pwm_chip->mmio_base, offset,
				PWM_POLARITY_MASK, (0x0 << PWM_POLARITY_SHIFT));

	return 0;
}

void hibvt_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				struct pwm_state *state)
{
	struct hibvt_pwm_chip *hi_pwm_chip = to_hibvt_pwm_chip(chip);
	void __iomem *base;
	unsigned int offset;
	unsigned int freq;
	unsigned int period_num, duty_num;
	unsigned int enable;

	freq = div_u64(clk_get_rate(hi_pwm_chip->clk), 1000000);
	base = hi_pwm_chip->mmio_base;

	offset = PWM_CFG0_ADDR(pwm->hwpwm);
	period_num = readl(base + offset);
	state->period = div_u64(period_num * 1000, freq);

	offset = PWM_CFG1_ADDR(pwm->hwpwm);
	duty_num = readl(base + offset);
	state->duty_cycle = div_u64(duty_num * 1000, freq);

	offset = PWM_CTRL_ADDR(pwm->hwpwm);
	enable = readl(base + offset);
	state->enabled = (PWM_ENABLE_MASK & enable);
}

static struct pwm_ops hibvt_pwm_ops = {
	.enable       = hibvt_pwm_enable,
	.disable      = hibvt_pwm_disable,
	.config       = hibvt_pwm_config,
	.set_polarity = hibvt_pwm_set_polarity,
	.get_state    = hibvt_pwm_get_state,

	.owner        = THIS_MODULE,
};

static int hibvt_pwm_probe(struct platform_device *pdev)
{
	struct hibvt_pwm_chip *pwm_chip;
	struct device_node *np = pdev->dev.of_node;
	struct resource *res;
	int ret = 0;
	int i;
	int offset;
	int pwm_nums;

	pwm_chip = devm_kzalloc(&pdev->dev, sizeof(*pwm_chip), GFP_KERNEL);
	if (pwm_chip == NULL)
		return -ENOMEM;

	pwm_chip->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(pwm_chip->clk)) {
		dev_err(&pdev->dev, "getting clock failed with %ld\n",
				PTR_ERR(pwm_chip->clk));
		return PTR_ERR(pwm_chip->clk);
	}

	ret = of_property_read_u32(np, "pwm-nums", &pwm_nums);
	if (ret < 0) {
		dev_err(&pdev->dev,
				"failed to get PWM number from device tree (error: %d)\n",
				ret);
		return ret;
	}

	pwm_chip->chip.ops = &hibvt_pwm_ops;
	pwm_chip->chip.dev = &pdev->dev;
	pwm_chip->chip.base = -1;
	pwm_chip->chip.npwm = pwm_nums;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pwm_chip->mmio_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pwm_chip->mmio_base))
		return PTR_ERR(pwm_chip->mmio_base);

	ret = clk_prepare_enable(pwm_chip->clk);
	if (ret < 0)
		return ret;

	pwm_chip->rstc = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(pwm_chip->rstc))
		return PTR_ERR(pwm_chip->rstc);

	reset_control_assert(pwm_chip->rstc);
	msleep(30);
	reset_control_deassert(pwm_chip->rstc);

	ret = pwmchip_add(&pwm_chip->chip);
	if (ret < 0)
		return ret;

	for (i = 0; i < pwm_nums; i++) {
		offset = PWM_CTRL_ADDR(i);
		hibvt_pwm_set_bits(pwm_chip->mmio_base, offset,
				PWM_KEEP_MASK, (0x1 << PWM_KEEP_SHIFT));
	}

	platform_set_drvdata(pdev, pwm_chip);

	return 0;
}

static int hibvt_pwm_remove(struct platform_device *pdev)
{
	struct hibvt_pwm_chip *pwm_chip;

	pwm_chip = platform_get_drvdata(pdev);
	if (pwm_chip == NULL)
		return -ENODEV;

	clk_disable_unprepare(pwm_chip->clk);

	return pwmchip_remove(&pwm_chip->chip);
}

static const struct of_device_id hibvt_pwm_of_match[] = {
	{.compatible = "hisilicon,hibvt-pwm"},
	{.compatible = "hisilicon,hi3516cv300-pwm"},
	{  }
};
MODULE_DEVICE_TABLE(of, hibvt_pwm_of_match);

static struct platform_driver hibvt_pwm_driver = {
	.driver		= {
		.name	= "hibvt-pwm",
		.of_match_table = hibvt_pwm_of_match,
	},
	.probe		= hibvt_pwm_probe,
	.remove		= hibvt_pwm_remove,
};

module_platform_driver(hibvt_pwm_driver);

MODULE_AUTHOR("yuanjian12@hisilicon.com");
MODULE_DESCRIPTION("Hisilicon BVT SOCs PWM driver");
MODULE_LICENSE("GPL v2");
