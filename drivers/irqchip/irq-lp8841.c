/*
 *  linux/drivers/irqchip/irq-lp8841.c
 *
 *  Support for ICP DAS LP-8841 FPGA irq
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

#define MODULE_NAME		"irq-lp8841"

#define EOI			0x00000000
#define INSINT			0x00000002
#define ENSYSINT		0x00000004
#define PRIMINT			0x00000006
#define PRIMINT_MASK		0xe0
#define SECOINT			0x00000008
#define SECOINT_MASK		(~(u8)PRIMINT_MASK)
#define ENRISEINT		0x0000000A
#define CLRRISEINT		0x0000000C
#define ENHILVINT		0x0000000E
#define CLRHILVINT		0x00000010
#define ENFALLINT		0x00000012
#define CLRFALLINT		0x00000014
#define IRQ_MEM_SIZE		0x00000016
#define LP8841_NUM_IRQ_DEFAULT	16

/**
 * struct lp8841_irq_data - LP8841 custom irq controller state container
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

struct lp8841_irq_data {
	void			*base;
	struct irq_domain	*domain;
	unsigned char		irq_sys_enabled;
	unsigned char		irq_high_enabled;
};

static void lp8841_mask_irq(struct irq_data *d)
{
	unsigned mask;
	unsigned long hwirq = d->hwirq;
	struct lp8841_irq_data *host = irq_data_get_irq_chip_data(d);

	if (hwirq < 8) {
		host->irq_high_enabled &= ~BIT(hwirq);

		mask = readb(host->base + ENHILVINT);
		mask &= ~BIT(hwirq);
		writeb(mask, host->base + ENHILVINT);
	} else {
		hwirq -= 8;
		host->irq_sys_enabled &= ~BIT(hwirq);

		mask = readb(host->base + ENSYSINT);
		mask &= ~BIT(hwirq);
		writeb(mask, host->base + ENSYSINT);
	}
}

static void lp8841_unmask_irq(struct irq_data *d)
{
	unsigned mask;
	unsigned long hwirq = d->hwirq;
	struct lp8841_irq_data *host = irq_data_get_irq_chip_data(d);

	if (hwirq < 8) {
		host->irq_high_enabled |= BIT(hwirq);
		mask = readb(host->base + CLRHILVINT);
		mask |= BIT(hwirq);
		writeb(mask, host->base + CLRHILVINT);

		mask = readb(host->base + ENHILVINT);
		mask |= BIT(hwirq);
		writeb(mask, host->base + ENHILVINT);
	} else {
		hwirq -= 8;
		host->irq_sys_enabled |= BIT(hwirq);

		mask = readb(host->base + SECOINT);
		mask |= BIT(hwirq);
		writeb(mask, host->base + SECOINT);

		mask = readb(host->base + ENSYSINT);
		mask |= BIT(hwirq);
		writeb(mask, host->base + ENSYSINT);
	}
}

static struct irq_chip lp8841_irq_chip = {
	.name			= "FPGA",
	.irq_ack		= lp8841_mask_irq,
	.irq_mask		= lp8841_mask_irq,
	.irq_mask_ack		= lp8841_mask_irq,
	.irq_unmask		= lp8841_unmask_irq,
};

static void lp8841_irq_handler(struct irq_desc *desc)
{
	int n;
	unsigned long mask;
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct lp8841_irq_data *host = irq_desc_get_handler_data(desc);

	chained_irq_enter(chip, desc);

	for (;;) {
		mask = readb(host->base + CLRHILVINT) & 0xff;
		/* load two registers into a single byte */
		mask |= (readb(host->base + SECOINT) & SECOINT_MASK) << 8;
		mask |= (readb(host->base + PRIMINT) & PRIMINT_MASK) << 8;
		if (mask == 0)
			break;
		for_each_set_bit(n, &mask, BITS_PER_LONG)
			generic_handle_irq(irq_find_mapping(host->domain, n));
	}

	writeb(0, host->base + EOI);
	chained_irq_exit(chip, desc);
}

static int lp8841_irq_domain_map(struct irq_domain *d, unsigned int irq,
				 irq_hw_number_t hw)
{
	struct lp8841_irq_data *host = d->host_data;
	int err;

	err = irq_set_chip_data(irq, host);
	if (err < 0)
		return err;

	irq_set_chip_and_handler(irq, &lp8841_irq_chip, handle_level_irq);
	irq_set_probe(irq);
	return 0;
}

const struct irq_domain_ops lp8841_irq_domain_ops = {
	.map	= lp8841_irq_domain_map,
	.xlate	= irq_domain_xlate_onecell,
};

static const struct of_device_id lp8841_irq_dt_ids[] = {
	{ .compatible = "icpdas,lp8841-irq", },
	{}
};

/*
 * REVISIT probing will need to rewritten when PXA is converted to DT
 */

static int lp8841_irq_probe(struct platform_device *pdev)
{
	struct resource *res_mem;
	int irq;
	struct device_node *np = pdev->dev.of_node;
	struct lp8841_irq_data *host;
	int i, err;

	irq = platform_get_irq(pdev, 0);
	if (IS_ERR_VALUE(irq)) {
		dev_err(&pdev->dev, "bad irq %i\n", irq);
		return irq;
	}

	res_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res_mem || resource_size(res_mem) < IRQ_MEM_SIZE) {
		dev_err(&pdev->dev, "bad IOmem %p\n", res_mem);
		if (res_mem)
			dev_err(&pdev->dev, "bad start %p or size %u\n",
					(void *) res_mem->start,
					resource_size(res_mem));
		return -ENODEV;
	}

	host = devm_kzalloc(&pdev->dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	host->base = devm_ioremap_resource(&pdev->dev, res_mem);
	if (!host->base) {
		dev_err(&pdev->dev, "Failed to ioremap %p\n", host->base);
		return -EFAULT;
	}

	host->domain = irq_domain_add_linear(np, LP8841_NUM_IRQ_DEFAULT,
				       &lp8841_irq_domain_ops, host);
	if (!host->domain) {
		dev_err(&pdev->dev, "Failed to add IRQ domain\n");
		return -ENOMEM;
	}

	for (i = 0; i < LP8841_NUM_IRQ_DEFAULT; i++) {
		err = irq_create_mapping(host->domain, i);
		if (err < 0)
			dev_err(&pdev->dev, "Failed to map IRQ %i\n", i);
	}

	/* Initialize chip registers */
	writeb(0, host->base + CLRRISEINT);
	writeb(0, host->base + ENRISEINT);
	writeb(0, host->base + CLRFALLINT);
	writeb(0, host->base + ENFALLINT);
	writeb(0, host->base + CLRHILVINT);
	writeb(0, host->base + ENHILVINT);
	writeb(0, host->base + ENSYSINT);
	writeb(0, host->base + SECOINT);

	irq_set_handler_data(irq, host);
	irq_set_chained_handler(irq, lp8841_irq_handler);

	pr_info(MODULE_NAME ": %i IRQs\n", LP8841_NUM_IRQ_DEFAULT);
	return 0;
}

static struct platform_driver lp8841_irq_driver = {
	.probe		= lp8841_irq_probe,
	.driver		= {
		.name	= MODULE_NAME,
		.of_match_table = lp8841_irq_dt_ids,
	},
};

static int __init lp8841_irq_init(void)
{
	return platform_driver_register(&lp8841_irq_driver);
}
device_initcall(lp8841_irq_init);
