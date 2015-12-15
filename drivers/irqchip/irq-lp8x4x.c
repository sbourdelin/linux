/*
 *  linux/drivers/irqchip/irq-lp8x4x.c
 *
 *  Support for ICP DAS LP-8x4x FPGA irq
 *  Copyright (C) 2013 Sergei Ianovich <ynvich@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation or any later version.
 */
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define EOI			0x00000000
#define INSINT			0x00000002
#define ENSYSINT		0x00000004
#define PRIMINT			0x00000006
#define PRIMINT_MASK		0xe0
#define SECOINT			0x00000008
#define SECOINT_MASK		0x1f
#define ENRISEINT		0x0000000A
#define CLRRISEINT		0x0000000C
#define ENHILVINT		0x0000000E
#define CLRHILVINT		0x00000010
#define ENFALLINT		0x00000012
#define CLRFALLINT		0x00000014
#define IRQ_MEM_SIZE		0x00000016
#define LP8X4X_NUM_IRQ_DEFAULT	16

/**
 * struct lp8x4x_irq_data - LP8X4X custom irq controller state container
 * @base:               base IO memory address
 * @irq_domain:         Interrupt translation domain; responsible for mapping
 *                      between hwirq number and linux irq number
 * @irq_sys_enabled:    mask keeping track of interrupts enabled in the
 *                      register which vendor calls 'system'
 * @irq_high_enabled:   mask keeping track of interrupts enabled in the
 *                      register which vendor calls 'high'
 *
 * The structure implements State Container from
 * Documentation/driver-model/design-patterns.txt
 */

struct lp8x4x_irq_data {
	void			*base;
	struct irq_domain	*domain;
	unsigned char		irq_sys_enabled;
	unsigned char		irq_high_enabled;
};

static void lp8x4x_mask_irq(struct irq_data *d)
{
	unsigned mask;
	unsigned long hwirq = d->hwirq;
	struct lp8x4x_irq_data *host = irq_data_get_irq_chip_data(d);

	if (hwirq < 8) {
		host->irq_high_enabled &= ~BIT(hwirq);

		mask = ioread8(host->base + ENHILVINT);
		mask &= ~BIT(hwirq);
		iowrite8(mask, host->base + ENHILVINT);
	} else {
		hwirq -= 8;
		host->irq_sys_enabled &= ~BIT(hwirq);

		mask = ioread8(host->base + ENSYSINT);
		mask &= ~BIT(hwirq);
		iowrite8(mask, host->base + ENSYSINT);
	}
}

static void lp8x4x_unmask_irq(struct irq_data *d)
{
	unsigned mask;
	unsigned long hwirq = d->hwirq;
	struct lp8x4x_irq_data *host = irq_data_get_irq_chip_data(d);

	if (hwirq < 8) {
		host->irq_high_enabled |= BIT(hwirq);
		mask = ioread8(host->base + CLRHILVINT);
		mask |= BIT(hwirq);
		iowrite8(mask, host->base + CLRHILVINT);

		mask = ioread8(host->base + ENHILVINT);
		mask |= BIT(hwirq);
		iowrite8(mask, host->base + ENHILVINT);
	} else {
		hwirq -= 8;
		host->irq_sys_enabled |= BIT(hwirq);

		mask = ioread8(host->base + SECOINT);
		mask |= BIT(hwirq);
		iowrite8(mask, host->base + SECOINT);

		mask = ioread8(host->base + ENSYSINT);
		mask |= BIT(hwirq);
		iowrite8(mask, host->base + ENSYSINT);
	}
}

static struct irq_chip lp8x4x_irq_chip = {
	.name			= "FPGA",
	.irq_ack		= lp8x4x_mask_irq,
	.irq_mask		= lp8x4x_mask_irq,
	.irq_mask_ack		= lp8x4x_mask_irq,
	.irq_unmask		= lp8x4x_unmask_irq,
};

static void lp8x4x_irq_handler(struct irq_desc *desc)
{
	int n;
	unsigned long mask;
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct lp8x4x_irq_data *host = irq_desc_get_handler_data(desc);

	chained_irq_enter(chip, desc);

	for (;;) {
		mask = ioread8(host->base + CLRHILVINT) & 0xff;
		mask |= (ioread8(host->base + SECOINT) & SECOINT_MASK) << 8;
		mask |= (ioread8(host->base + PRIMINT) & PRIMINT_MASK) << 8;
		mask &= host->irq_high_enabled | (host->irq_sys_enabled << 8);
		if (mask == 0)
			break;
		for_each_set_bit(n, &mask, BITS_PER_LONG)
			generic_handle_irq(irq_find_mapping(host->domain, n));
	}

	iowrite8(0, host->base + EOI);
	chained_irq_exit(chip, desc);
}

static int lp8x4x_irq_domain_map(struct irq_domain *d, unsigned int irq,
				 irq_hw_number_t hw)
{
	struct lp8x4x_irq_data *host = d->host_data;
	int err;

	err = irq_set_chip_data(irq, host);
	if (err < 0)
		return err;

	irq_set_chip_and_handler(irq, &lp8x4x_irq_chip, handle_level_irq);
	irq_set_probe(irq);
	return 0;
}

const struct irq_domain_ops lp8x4x_irq_domain_ops = {
	.map	= lp8x4x_irq_domain_map,
	.xlate	= irq_domain_xlate_onecell,
};

static const struct of_device_id lp8x4x_irq_dt_ids[] = {
	{ .compatible = "icpdas,irq-lp8x4x", },
	{}
};

static int lp8x4x_irq_probe(struct platform_device *pdev)
{
	struct resource *res_mem, *res_irq;
	struct device_node *np = pdev->dev.of_node;
	struct lp8x4x_irq_data *host;
	int i, err;

	res_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	res_irq = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res_mem || !res_irq || resource_size(res_mem) < IRQ_MEM_SIZE)
		return -ENODEV;

	host = devm_kzalloc(&pdev->dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENODEV;

	host->base = devm_ioremap_resource(&pdev->dev, res_mem);
	if (!host->base) {
		dev_err(&pdev->dev, "Failed to ioremap %p\n", host->base);
		return -EFAULT;
	}

	host->domain = irq_domain_add_linear(np, LP8X4X_NUM_IRQ_DEFAULT,
				       &lp8x4x_irq_domain_ops, host);
	if (!host->domain) {
		dev_err(&pdev->dev, "Failed to add IRQ domain\n");
		return -ENOMEM;
	}

	for (i = 0; i < LP8X4X_NUM_IRQ_DEFAULT; i++) {
		err = irq_create_mapping(host->domain, i);
		if (err < 0)
			dev_err(&pdev->dev, "Failed to map IRQ %i\n", i);
	}

	/* Initialize chip registers */
	iowrite8(0, host->base + CLRRISEINT);
	iowrite8(0, host->base + ENRISEINT);
	iowrite8(0, host->base + CLRFALLINT);
	iowrite8(0, host->base + ENFALLINT);
	iowrite8(0, host->base + CLRHILVINT);
	iowrite8(0, host->base + ENHILVINT);
	iowrite8(0, host->base + ENSYSINT);
	iowrite8(0, host->base + SECOINT);

	irq_set_handler_data(res_irq->start, host);
	irq_set_chained_handler(res_irq->start, lp8x4x_irq_handler);

	return 0;
}

static struct platform_driver lp8x4x_irq_driver = {
	.probe		= lp8x4x_irq_probe,
	.driver		= {
		.name	= "irq-lp8x4x",
		.of_match_table = lp8x4x_irq_dt_ids,
	},
};

static int __init lp8x4x_irq_init(void)
{
	return platform_driver_register(&lp8x4x_irq_driver);
}
postcore_initcall(lp8x4x_irq_init);
