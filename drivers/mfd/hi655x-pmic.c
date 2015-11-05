/*
 * Device driver for PMIC DRIVER in HI655X IC
 *
 * Copyright (c) 2015 Hisilicon Co. Ltd
 *
 * Fei Wang  <w.f@huawei.com>
 * Chen Feng <puck.chen@hisilicon.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/io.h>
#include <linux/irq.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/hardirq.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/irqdomain.h>
#include <linux/mfd/hi655x-pmic.h>
#include <linux/regmap.h>

static const struct of_device_id of_hi655x_pmic_child_match_tbl[] = {
	{ .compatible = "hisilicon,hi655x-regulator-pmic", },
	{},
};

static const struct of_device_id of_hi655x_pmic_match_tbl[] = {
	{ .compatible = "hisilicon,hi655x-pmic-driver", },
	{},
};

static unsigned int hi655x_pmic_get_version(struct hi655x_pmic *pmic)
{
	u32 val;

	regmap_read(pmic->regmap,
		    HI655X_REG_TO_BUS_ADDR(HI655X_VER_REG), &val);

	return val;
}

static irqreturn_t hi655x_pmic_irq_handler(int irq, void *data)
{
	struct hi655x_pmic *pmic = (struct hi655x_pmic *)data;
	u32 pending;
	u32 ret = IRQ_NONE;
	unsigned long offset;
	int i;

	for (i = 0; i < HI655X_IRQ_ARRAY; i++) {
		regmap_read(pmic->regmap,
			    HI655X_REG_TO_BUS_ADDR(i + HI655X_IRQ_STAT_BASE),
			    &pending);
		if (pending)
			pr_debug("pending[%d]=0x%x\n\r", i, pending);

		/* clear pmic-sub-interrupt */
		regmap_write(pmic->regmap,
			     HI655X_REG_TO_BUS_ADDR(i + HI655X_IRQ_STAT_BASE),
			     pending);

		if (pending) {
			for_each_set_bit(offset, (unsigned long *)&pending,
					 HI655X_BITS)
				generic_handle_irq(pmic->irqs[offset +
						   i * HI655X_BITS]);
			ret = IRQ_HANDLED;
		}
	}
	return ret;
}

static void hi655x_pmic_irq_mask(struct irq_data *d)
{
	u32 data, offset;
	unsigned long pmic_spin_flag = 0;
	struct hi655x_pmic *pmic = irq_data_get_irq_chip_data(d);

	offset = ((irqd_to_hwirq(d) >> 3) + HI655X_IRQ_MASK_BASE);
	spin_lock_irqsave(&pmic->ssi_hw_lock, pmic_spin_flag);
	regmap_read(pmic->regmap, HI655X_REG_TO_BUS_ADDR(offset), &data);
	data |= (1 << (irqd_to_hwirq(d) & 0x07));
	regmap_write(pmic->regmap, HI655X_REG_TO_BUS_ADDR(offset), data);
	spin_unlock_irqrestore(&pmic->ssi_hw_lock, pmic_spin_flag);
}

static void hi655x_pmic_irq_unmask(struct irq_data *d)
{
	u32 data, offset;
	unsigned long pmic_spin_flag = 0;
	struct hi655x_pmic *pmic = irq_data_get_irq_chip_data(d);

	offset = ((irqd_to_hwirq(d) >> 3) + HI655X_IRQ_MASK_BASE);
	spin_lock_irqsave(&pmic->ssi_hw_lock, pmic_spin_flag);
	regmap_read(pmic->regmap, HI655X_REG_TO_BUS_ADDR(offset), &data);
	data &= ~(1 << (irqd_to_hwirq(d) & 0x07));
	regmap_write(pmic->regmap, HI655X_REG_TO_BUS_ADDR(offset), data);
	spin_unlock_irqrestore(&pmic->ssi_hw_lock, pmic_spin_flag);
}

static struct irq_chip hi655x_pmic_irqchip = {
	.name		= "hisi-hi655x-pmic-irqchip",
	.irq_mask	= hi655x_pmic_irq_mask,
	.irq_unmask	= hi655x_pmic_irq_unmask,
};

static int hi655x_pmic_irq_map(struct irq_domain *d, unsigned int virq,
			       irq_hw_number_t hw)
{
	struct hi655x_pmic *pmic = d->host_data;

	irq_set_chip_and_handler_name(virq, &hi655x_pmic_irqchip,
				      handle_simple_irq,
				      "hisi-hi655x-pmic-irqchip");
	irq_set_chip_data(virq, pmic);
	irq_set_irq_type(virq, IRQ_TYPE_NONE);

	return 0;
}

static struct irq_domain_ops hi655x_domain_ops = {
	.map	= hi655x_pmic_irq_map,
	.xlate	= irq_domain_xlate_twocell,
};

static inline void hi655x_pmic_clear_int(struct hi655x_pmic *pmic)
{
	int addr;

	for (addr = HI655X_IRQ_STAT_BASE;
	     addr < (HI655X_IRQ_STAT_BASE + HI655X_IRQ_ARRAY);
	     addr++) {
		regmap_write(pmic->regmap,
			     HI655X_REG_TO_BUS_ADDR(addr), HI655X_IRQ_CLR);
	}
}

static inline void hi655x_pmic_mask_int(struct hi655x_pmic *pmic)
{
	int addr;

	for (addr = HI655X_IRQ_MASK_BASE;
	     addr < (HI655X_IRQ_MASK_BASE + HI655X_IRQ_ARRAY);
	     addr++) {
		regmap_write(pmic->regmap,
			     HI655X_REG_TO_BUS_ADDR(addr), HI655X_IRQ_MASK);
	}
}

static struct regmap_config hi655x_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 8,
	.max_register = HI655X_REG_TO_BUS_ADDR(HI655X_REG_MAX),
};

static int hi655x_pmic_irq_init(struct platform_device *pdev,
				struct hi655x_pmic *pmic)
{
	enum of_gpio_flags gpio_flags;
	struct device_node *np = (&pdev->dev)->of_node;
	struct device_node *gpio_np = NULL;
	unsigned int virq = 0;
	int i, ret = 0;

	pmic->ver = hi655x_pmic_get_version(pmic);
	if ((pmic->ver < PMU_VER_START) || (pmic->ver > PMU_VER_END)) {
		dev_warn(&pdev->dev, "it is wrong pmu version\n");
		return -EINVAL;
	}

	regmap_write(pmic->regmap, HI655X_REG_TO_BUS_ADDR(ANA_IRQM_REG0), 0xff);

	gpio_np = of_parse_phandle(np, "pmic-gpios", 0);
	if (!gpio_np) {
		dev_err(&pdev->dev, "can't parse property\n");
		return -ENOENT;
	}
	pmic->gpio = of_get_gpio_flags(gpio_np, 0, &gpio_flags);
	if (pmic->gpio < 0) {
		dev_err(&pdev->dev,
			"failed to of_get_gpio_flags %d\n", pmic->gpio);
		return  pmic->gpio;
	}
	if (!gpio_is_valid(pmic->gpio)) {
		dev_err(&pdev->dev, "it is invalid gpio %d\n", pmic->gpio);
		return -EINVAL;
	}
	ret = gpio_request_one(pmic->gpio, GPIOF_IN, "hi655x_pmic_irq");
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to request gpio %d  ret = %d\n",
			pmic->gpio, ret);
		return ret;
	}
	pmic->irq = gpio_to_irq(pmic->gpio);

	hi655x_pmic_clear_int(pmic);
	hi655x_pmic_mask_int(pmic);
	pmic->domain = irq_domain_add_simple(np,
			HI655X_NR_IRQ, 0, &hi655x_domain_ops, pmic);
	if (!pmic->domain) {
		dev_err(&pdev->dev, "failed irq domain add simple!\n");
		ret = -ENODEV;
		goto irq_domain_add_simple;
	}

	for (i = 0; i < HI655X_NR_IRQ; i++) {
		virq = irq_create_mapping(pmic->domain, i);
		if (!virq) {
			dev_err(&pdev->dev, "Failed mapping hwirq\n");
			ret = -ENOSPC;
			goto irq_create_mapping;
		}
		pmic->irqs[i] = virq;
	}

	ret = request_threaded_irq(pmic->irq, hi655x_pmic_irq_handler,
				   NULL, IRQF_TRIGGER_LOW |
				   IRQF_SHARED | IRQF_NO_SUSPEND,
				   "hi655x-pmic-irq", pmic);
	if (ret < 0) {
		dev_err(&pdev->dev, "could not claim pmic %d\n", ret);
		ret = -ENODEV;
		goto request_threaded_irq;
	}
	return 0;

irq_domain_add_simple:
irq_create_mapping:
request_threaded_irq:
	free_irq(pmic->irq, pmic);
	gpio_free(pmic->gpio);
	return ret;
}

static int hi655x_pmic_probe(struct platform_device *pdev)
{
	struct device_node *np = (&pdev->dev)->of_node;
	struct hi655x_pmic *pmic = NULL;
	void __iomem *base;
	int ret;

	pmic = devm_kzalloc(&pdev->dev, sizeof(*pmic), GFP_KERNEL);
	if (!pmic)
		return -ENOMEM;

	spin_lock_init(&pmic->ssi_hw_lock);
	pmic->res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, pmic->res);
	if (!base)
		return -ENOMEM;

	pmic->regmap = devm_regmap_init_mmio_clk(&pdev->dev, NULL, base,
						 &hi655x_regmap_config);
	ret = hi655x_pmic_irq_init(pdev, pmic);
	if (ret) {
		dev_err(&pdev->dev, "pmic irq init failed: %d\n", ret);
		return ret;
	}

	pmic->dev = &pdev->dev;
	platform_set_drvdata(pdev, pmic);
	of_platform_populate(np, of_hi655x_pmic_child_match_tbl,
			     NULL, &pdev->dev);

	return 0;
}

static int hi655x_pmic_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hi655x_pmic *pmic = platform_get_drvdata(pdev);

	free_irq(pmic->irq, pmic);
	gpio_free(pmic->gpio);
	devm_release_mem_region(dev, pmic->res->start,
				resource_size(pmic->res));
	devm_kfree(dev, pmic);
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static struct platform_driver hi655x_pmic_driver = {
	.driver	= {
		.name =	"hisi,hi655x-pmic",
		.owner = THIS_MODULE,
		.of_match_table = of_hi655x_pmic_match_tbl,
	},
	.probe  = hi655x_pmic_probe,
	.remove	= hi655x_pmic_remove,
};
module_platform_driver(hi655x_pmic_driver);

MODULE_AUTHOR("Fei Wang <w.f@huawei.com>");
MODULE_DESCRIPTION("Hisi hi655x pmic driver");
MODULE_LICENSE("GPL v2");
