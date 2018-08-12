// SPDX-License-Identifier: GPL-2.0+
/*
 *
 * Actions Semi Owl SoCs SIRQ interrupt controller driver
 *
 * Copyright (C) 2014 Actions Semi Inc.
 * David Liu <liuwei@actions-semi.com>
 *
 * Author: Parthiban Nallathambi <pn@denx.de>
 * Author: Saravanan Sekar <sravanhome@gmail.com>
 */

#include <linux/interrupt.h>
#include <linux/irqchip.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>

#include <dt-bindings/interrupt-controller/arm-gic.h>

#define INTC_GIC_INTERRUPT_PIN		13
#define INTC_EXTCTL_PENDING		BIT(0)
#define INTC_EXTCTL_CLK_SEL		BIT(4)
#define INTC_EXTCTL_EN			BIT(5)
#define	INTC_EXTCTL_TYPE_MASK		GENMASK(6, 7)
#define	INTC_EXTCTL_TYPE_HIGH		0
#define	INTC_EXTCTL_TYPE_LOW		BIT(6)
#define	INTC_EXTCTL_TYPE_RISING		BIT(7)
#define	INTC_EXTCTL_TYPE_FALLING	(BIT(6) | BIT(7))

#define get_sirq_offset(x)	chip_data->sirq[x].offset

/* Per SIRQ data */
struct owl_sirq {
	u16 offset;
	/* software is responsible to clear interrupt pending bit when
	 * type is edge triggered. This value is for per SIRQ line.
	 */
	bool type_edge;
};

struct owl_sirq_chip_data {
	void __iomem *base;
	raw_spinlock_t lock;
	/* some SoC's share the register for all SIRQ lines, so maintain
	 * register is shared or not here. This value is from DT.
	 */
	bool shared_reg;
	struct owl_sirq *sirq;
};

static struct owl_sirq_chip_data *sirq_data;

static unsigned int sirq_read_extctl(struct irq_data *data)
{
	struct owl_sirq_chip_data *chip_data = data->chip_data;
	unsigned int val;

	val = readl_relaxed(chip_data->base + get_sirq_offset(data->hwirq));
	if (chip_data->shared_reg)
		val = (val >> (2 - data->hwirq) * 8) & 0xff;

	return val;
}

static void sirq_write_extctl(struct irq_data *data, unsigned int extctl)
{
	struct owl_sirq_chip_data *chip_data = data->chip_data;
	unsigned int val;

	if (chip_data->shared_reg) {
		val = readl_relaxed(chip_data->base +
				get_sirq_offset(data->hwirq));
		val &= ~(0xff << (2 - data->hwirq) * 8);
		extctl &= 0xff;
		extctl = (extctl << (2 - data->hwirq) * 8) | val;
	}

	writel_relaxed(extctl, chip_data->base +
			get_sirq_offset(data->hwirq));
}

static void owl_sirq_ack(struct irq_data *data)
{
	struct owl_sirq_chip_data *chip_data = data->chip_data;
	unsigned int extctl;
	unsigned long flags;

	/* software must clear external interrupt pending, when interrupt type
	 * is edge triggered, so we need per SIRQ based clearing.
	 */
	if (chip_data->sirq[data->hwirq].type_edge) {
		raw_spin_lock_irqsave(&chip_data->lock, flags);

		extctl = sirq_read_extctl(data);
		extctl |= INTC_EXTCTL_PENDING;
		sirq_write_extctl(data, extctl);

		raw_spin_unlock_irqrestore(&chip_data->lock, flags);
	}
	irq_chip_ack_parent(data);
}

static void owl_sirq_mask(struct irq_data *data)
{
	struct owl_sirq_chip_data *chip_data = data->chip_data;
	unsigned int extctl;
	unsigned long flags;

	raw_spin_lock_irqsave(&chip_data->lock, flags);

	extctl = sirq_read_extctl(data);
	extctl &= ~(INTC_EXTCTL_EN);
	sirq_write_extctl(data, extctl);

	raw_spin_unlock_irqrestore(&chip_data->lock, flags);
	irq_chip_mask_parent(data);
}

static void owl_sirq_unmask(struct irq_data *data)
{
	struct owl_sirq_chip_data *chip_data = data->chip_data;
	unsigned int extctl;
	unsigned long flags;

	raw_spin_lock_irqsave(&chip_data->lock, flags);

	extctl = sirq_read_extctl(data);
	extctl |= INTC_EXTCTL_EN;
	sirq_write_extctl(data, extctl);

	raw_spin_unlock_irqrestore(&chip_data->lock, flags);
	irq_chip_unmask_parent(data);
}

/* PAD_PULLCTL needs to be defined in pinctrl */
static int owl_sirq_set_type(struct irq_data *data, unsigned int flow_type)
{
	struct owl_sirq_chip_data *chip_data = data->chip_data;
	unsigned int extctl, type;
	unsigned long flags;

	switch (flow_type) {
	case IRQF_TRIGGER_LOW:
		type = INTC_EXTCTL_TYPE_LOW;
		break;
	case IRQF_TRIGGER_HIGH:
		type = INTC_EXTCTL_TYPE_HIGH;
		break;
	case IRQF_TRIGGER_FALLING:
		type = INTC_EXTCTL_TYPE_FALLING;
		chip_data->sirq[data->hwirq].type_edge = true;
		break;
	case IRQF_TRIGGER_RISING:
		type = INTC_EXTCTL_TYPE_RISING;
		chip_data->sirq[data->hwirq].type_edge = true;
		break;
	default:
		return  -EINVAL;
	}

	raw_spin_lock_irqsave(&chip_data->lock, flags);

	extctl = sirq_read_extctl(data);
	extctl &= ~INTC_EXTCTL_TYPE_MASK;
	extctl |= type;
	sirq_write_extctl(data, extctl);

	raw_spin_unlock_irqrestore(&chip_data->lock, flags);
	data = data->parent_data;
	return irq_chip_set_type_parent(data, flow_type);
}

static struct irq_chip owl_sirq_chip = {
	.name		= "owl-sirq",
	.irq_ack	= owl_sirq_ack,
	.irq_mask	= owl_sirq_mask,
	.irq_unmask	= owl_sirq_unmask,
	.irq_set_type	= owl_sirq_set_type,
	.irq_eoi	= irq_chip_eoi_parent,
	.irq_retrigger	= irq_chip_retrigger_hierarchy,
};

static int owl_sirq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				 unsigned int nr_irqs, void *arg)
{
	struct irq_fwspec *fwspec = arg;
	struct irq_fwspec parent_fwspec = {
		.param_count	= 3,
		.param[0]	= GIC_SPI,
		.param[1]	= fwspec->param[0] + INTC_GIC_INTERRUPT_PIN,
		.param[2]	= fwspec->param[1],
		.fwnode		= domain->parent->fwnode,
	};

	if (WARN_ON(nr_irqs != 1))
		return -EINVAL;

	irq_domain_set_hwirq_and_chip(domain, virq, fwspec->param[0],
				      &owl_sirq_chip,
				      domain->host_data);

	return irq_domain_alloc_irqs_parent(domain, virq, nr_irqs,
					    &parent_fwspec);
}

static const struct irq_domain_ops sirq_domain_ops = {
	.alloc	= owl_sirq_domain_alloc,
	.free	= irq_domain_free_irqs_common,
};

static void owl_sirq_clk_init(int offset, int hwirq)
{
	unsigned int val;

	/* register default clock is 32Khz, change to 24Mhz only when defined */
	val = readl_relaxed(sirq_data->base + offset);
	if (sirq_data->shared_reg)
		val |= INTC_EXTCTL_CLK_SEL << (2 - hwirq) * 8;
	else
		val |= INTC_EXTCTL_CLK_SEL;

	writel_relaxed(val, sirq_data->base + offset);
}

static int __init owl_sirq_of_init(struct device_node *node,
					struct device_node *parent)
{
	struct irq_domain *domain, *domain_parent;
	int ret = 0, i, sirq_cnt = 0;
	struct owl_sirq_chip_data *chip_data;

	sirq_cnt = of_property_count_u32_elems(node, "actions,sirq-offset");
	if (sirq_cnt <= 0) {
		pr_err("owl_sirq: register offset not specified\n");
		return -EINVAL;
	}

	chip_data = kzalloc(sizeof(*chip_data), GFP_KERNEL);
	if (!chip_data)
		return -ENOMEM;
	sirq_data = chip_data;

	chip_data->sirq = kcalloc(sirq_cnt, sizeof(*chip_data->sirq),
				GFP_KERNEL);
	if (!chip_data->sirq)
		goto out_free;

	raw_spin_lock_init(&chip_data->lock);
	chip_data->base = of_iomap(node, 0);
	if (!chip_data->base) {
		pr_err("owl_sirq: unable to map sirq register\n");
		ret = -ENXIO;
		goto out_free;
	}

	chip_data->shared_reg = of_property_read_bool(node,
						"actions,sirq-shared-reg");
	for (i = 0; i < sirq_cnt; i++) {
		u32 value;

		ret = of_property_read_u32_index(node, "actions,sirq-offset",
						i, &value);
		if (ret)
			goto out_unmap;

		get_sirq_offset(i) = (u16)value;

		ret = of_property_read_u32_index(node, "actions,sirq-clk-sel",
						i, &value);
		if (ret || !value)
			continue;

		/* external interrupt controller can be either connect to 32Khz/
		 * 24Mhz external/internal clock. This shall be configured for
		 * per SIRQ line. It can be defined from DT, failing defaults to
		 * 24Mhz clock.
		 */
		owl_sirq_clk_init(get_sirq_offset(i), i);
	}

	domain_parent = irq_find_host(parent);
	if (!domain_parent) {
		pr_err("owl_sirq: interrupt-parent not found\n");
		goto out_unmap;
	}

	domain = irq_domain_add_hierarchy(domain_parent, 0,
			sirq_cnt, node,
			&sirq_domain_ops, chip_data);
	if (!domain) {
		ret = -ENOMEM;
		goto out_unmap;
	}

	return 0;

out_unmap:
	iounmap(chip_data->base);
out_free:
	kfree(chip_data);
	kfree(chip_data->sirq);
	return ret;
}

IRQCHIP_DECLARE(owl_sirq, "actions,owl-sirq", owl_sirq_of_init);
