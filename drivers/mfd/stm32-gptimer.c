/*
 * stm32-gptimer.c
 *
 * Copyright (C) STMicroelectronics 2016
 * Author: Benjamin Gaignard <benjamin.gaignard@st.com> for STMicroelectronics.
 * License terms:  GNU General Public License (GPL), version 2
 */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/reset.h>

#include <linux/mfd/stm32-gptimer.h>

static const struct regmap_config stm32_gptimer_regmap_cfg = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = sizeof(u32),
	.max_register = 0x400,
	.fast_io = true,
};

static int stm32_gptimer_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct stm32_gptimer_dev *mfd;
	struct resource *res;
	void __iomem *mmio;

	mfd = devm_kzalloc(dev, sizeof(*mfd), GFP_KERNEL);
	if (!mfd)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENOMEM;

	mmio = devm_ioremap_resource(dev, res);
	if (IS_ERR(mmio))
		return PTR_ERR(mmio);

	mfd->regmap = devm_regmap_init_mmio_clk(dev, "clk_int", mmio,
						&stm32_gptimer_regmap_cfg);
	if (IS_ERR(mfd->regmap))
		return PTR_ERR(mfd->regmap);

	mfd->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(mfd->clk))
		return PTR_ERR(mfd->clk);

	platform_set_drvdata(pdev, mfd);

	return of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);
}

static const struct of_device_id stm32_gptimer_of_match[] = {
	{
		.compatible = "st,stm32-gptimer",
	},
};
MODULE_DEVICE_TABLE(of, stm32_gptimer_of_match);

static struct platform_driver stm32_gptimer_driver = {
	.probe		= stm32_gptimer_probe,
	.driver	= {
		.name	= "stm32-gptimer",
		.of_match_table = stm32_gptimer_of_match,
	},
};
module_platform_driver(stm32_gptimer_driver);

MODULE_DESCRIPTION("STMicroelectronics STM32 general purpose timer");
MODULE_LICENSE("GPL");
