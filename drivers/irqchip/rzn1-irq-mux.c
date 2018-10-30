// SPDX-License-Identifier: GPL-2.0
/*
 * RZ/N1 GPIO Interrupt Multiplexer
 *
 * Copyright (C) 2018 Renesas Electronics Europe Limited
 *
 * On RZ/N1 devices, there are 3 Synopsys DesignWare GPIO blocks each configured
 * to have 32 interrupt outputs, so we have a total of 96 GPIO interrupts.
 * All of these are passed to the GPIO IRQ Muxer, which selects 8 of the GPIO
 * interrupts to pass onto the GIC.
 */

#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>

#define GPIO_IRQ_SPEC_SIZE	3
#define MAX_NR_GPIO_CONTROLLERS	3
#define MAX_NR_GPIO_IRQ		32
#define MAX_NR_INPUT_IRQS	(MAX_NR_GPIO_CONTROLLERS * MAX_NR_GPIO_IRQ)
#define MAX_NR_OUTPUT_IRQS	8

struct irqmux_priv;
struct irqmux_one {
	unsigned int mapped_irq;
	unsigned int input_irq_nr;
	struct irqmux_priv *priv;
};

struct irqmux_priv {
	struct device *dev;
	struct irq_chip irq_chip;
	struct irq_domain *irq_domain;
	unsigned int nr_irqs;
	struct irqmux_one irq[MAX_NR_OUTPUT_IRQS];
};

static void irqmux_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct irqmux_one *girq = irq_desc_get_handler_data(desc);
	struct irqmux_priv *priv = girq->priv;
	unsigned int irq;

	chained_irq_enter(chip, desc);

	irq = irq_find_mapping(priv->irq_domain, girq->input_irq_nr);
	generic_handle_irq(irq);

	chained_irq_exit(chip, desc);
}

static int irqmux_domain_map(struct irq_domain *h, unsigned int irq,
			     irq_hw_number_t hwirq)
{
	struct irqmux_priv *priv = h->host_data;

	irq_set_chip_data(irq, h->host_data);
	irq_set_chip_and_handler(irq, &priv->irq_chip, handle_simple_irq);

	return 0;
}

static const struct irq_domain_ops irqmux_domain_ops = {
	.map	= irqmux_domain_map,
};

static int irqmux_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource *res;
	u32 __iomem *regs;
	struct irqmux_priv *priv;
	u32 int_specs[MAX_NR_OUTPUT_IRQS][GPIO_IRQ_SPEC_SIZE];
	DECLARE_BITMAP(irqs_in_used, MAX_NR_INPUT_IRQS);
	unsigned int irqs_out_used = 0;
	unsigned int i;
	int nr_irqs;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	platform_set_drvdata(pdev, priv);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	nr_irqs = of_irq_count(np);
	if (nr_irqs < 0)
		return nr_irqs;

	if (nr_irqs > MAX_NR_OUTPUT_IRQS) {
		dev_err(dev, "too many output interrupts\n");
		return -ENOENT;
	}

	priv->nr_irqs = nr_irqs;

	/* Get the interrupt specifers */
	if (of_property_read_u32_array(dev->of_node, "interrupts",
				       (u32 *)int_specs,
				       priv->nr_irqs * GPIO_IRQ_SPEC_SIZE)) {
		dev_err(dev, "cannot get interrupt specifiers\n");
		return -ENOENT;
	}

	bitmap_zero(irqs_in_used, MAX_NR_INPUT_IRQS);

	/* Check the interrupt specifiers */
	for (i = 0; i < priv->nr_irqs; i++) {
		u32 *int_spec = int_specs[i];
		u32 input_irq = int_spec[1] * MAX_NR_GPIO_IRQ + int_spec[2];

		dev_info(dev, "irq %u=gpio%ua:%u\n", int_spec[0], int_spec[1],
			 int_spec[2]);

		if (int_spec[0] >= MAX_NR_OUTPUT_IRQS ||
		    int_spec[1] >= MAX_NR_GPIO_CONTROLLERS ||
		    int_spec[2] >= MAX_NR_GPIO_IRQ) {
			dev_err(dev, "invalid interrupt args\n");
			return -ENOENT;
		}

		if (irqs_out_used & BIT(int_spec[0]) ||
		    test_bit(input_irq, irqs_in_used)) {
			dev_err(dev, "irq %d already used\n", i);
			return -ENOENT;
		}

		irqs_out_used |= BIT(int_spec[0]);
		set_bit(input_irq, irqs_in_used);
	}

	/* Create IRQ domain for the interrupts coming from the GPIO blocks */
	priv->irq_chip.name = dev_name(dev);
	priv->irq_domain = irq_domain_add_linear(np, MAX_NR_INPUT_IRQS,
						 &irqmux_domain_ops, priv);
	if (!priv->irq_domain)
		return -ENOMEM;

	/* Setup the interrupts */
	for (i = 0; i < priv->nr_irqs; i++) {
		struct of_phandle_args ofirq;
		u32 *int_spec = int_specs[i];
		u32 input_irq = int_spec[1] * MAX_NR_GPIO_IRQ + int_spec[2];
		struct irqmux_one *irq = &priv->irq[i];

		if (of_irq_parse_one(dev->of_node, i, &ofirq)) {
			ret = -ENOENT;
			goto err;
		}

		priv->irq[i].mapped_irq = irq_create_of_mapping(&ofirq);
		if (!priv->irq[i].mapped_irq) {
			dev_err(dev, "cannot get interrupt\n");
			ret = -ENOENT;
			goto err;
		}

		irq->priv = priv;
		irq->input_irq_nr = input_irq;

		irq_set_chained_handler_and_data(irq->mapped_irq,
						 irqmux_handler, irq);

		/* Set up the hardware to pass the interrupt through */
		writel(irq->input_irq_nr, &regs[int_spec[0]]);
	}

	dev_info(dev, "probed, %d gpio interrupts\n", priv->nr_irqs);

	return 0;

err:
	while (i--) {
		struct irqmux_one *irq = &priv->irq[i];

		irq_set_chained_handler_and_data(irq->mapped_irq, NULL, NULL);
		irq_dispose_mapping(irq->mapped_irq);
	}
	irq_domain_remove(priv->irq_domain);

	return 0;
}

static int irqmux_remove(struct platform_device *pdev)
{
	struct irqmux_priv *priv = platform_get_drvdata(pdev);
	unsigned int i;

	for (i = 0; i < priv->nr_irqs; i++) {
		struct irqmux_one *irq = &priv->irq[i];

		irq_set_chained_handler_and_data(irq->mapped_irq, NULL, NULL);
		irq_dispose_mapping(irq->mapped_irq);
	}
	irq_domain_remove(priv->irq_domain);

	return 0;
}

static const struct of_device_id irqmux_match[] = {
	{ .compatible = "renesas,rzn1-gpioirqmux", },
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, irqmux_match);

static struct platform_driver irqmux_driver = {
	.driver = {
		.name = "gpio_irq_mux",
		.owner = THIS_MODULE,
		.of_match_table = irqmux_match,
	},
	.probe = irqmux_probe,
	.remove = irqmux_remove,
};

module_platform_driver(irqmux_driver);

MODULE_DESCRIPTION("Renesas RZ/N1 GPIO IRQ Multiplexer Driver");
MODULE_AUTHOR("Phil Edworthy <phil.edworthy@renesas.com>");
MODULE_LICENSE("GPL v2");
