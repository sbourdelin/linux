// SPDX-License-Identifier: GPL-2.0+
/*
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

#define INTC_EXTCTL_PENDING		BIT(0)
#define INTC_EXTCTL_CLK_SEL		BIT(4)
#define INTC_EXTCTL_EN			BIT(5)
#define	INTC_EXTCTL_TYPE_MASK		GENMASK(6, 7)
#define	INTC_EXTCTL_TYPE_HIGH		0
#define	INTC_EXTCTL_TYPE_LOW		BIT(6)
#define	INTC_EXTCTL_TYPE_RISING		BIT(7)
#define	INTC_EXTCTL_TYPE_FALLING	(BIT(6) | BIT(7))

struct owl_sirq_chip_data {
	void __iomem *base;
	raw_spinlock_t lock;
	/*
	 * Some SoC's share the register for all SIRQ lines, so maintain
	 * register is shared or not here. This value is from DT.
	 */
	bool shared_reg;
	u32 ext_irq_start;
	u32 ext_irq_end;
	u16 offset[3];
	u8 trigger;
};
static struct owl_sirq_chip_data *sirq_data;

static u32 sirq_read_extctl(struct owl_sirq_chip_data *data, u32 index)
{
	u32 val;

	val = readl_relaxed(data->base + data->offset[index]);
	if (data->shared_reg)
		val = (val >> (2 - index) * 8) & 0xff;

	return val;
}

static void sirq_write_extctl(struct owl_sirq_chip_data *data,
				u32 extctl, u32 index)
{
	u32 val;

	if (data->shared_reg) {
		val = readl_relaxed(data->base + data->offset[index]);
		val &= ~(0xff << (2 - index) * 8);
		extctl &= 0xff;
		extctl = (extctl << (2 - index) * 8) | val;
	}

	writel_relaxed(extctl, data->base + data->offset[index]);
}

static void sirq_clear_set_extctl(struct owl_sirq_chip_data *d,
					u32 clear, u32 set, u32 index)
{
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&d->lock, flags);
	val = sirq_read_extctl(d, index);
	val &= ~clear;
	val |= set;
	sirq_write_extctl(d, val, index);
	raw_spin_unlock_irqrestore(&d->lock, flags);
}

static void owl_sirq_eoi(struct irq_data *data)
{
	struct owl_sirq_chip_data *chip_data = data->chip_data;
	u32 index = data->hwirq - chip_data->ext_irq_start;

	/*
	 * Software must clear external interrupt pending, when interrupt type
	 * is edge triggered, so we need per SIRQ based clearing.
	 */
	if (chip_data->trigger & (1 << index))
		sirq_clear_set_extctl(chip_data, 0, INTC_EXTCTL_PENDING, index);
	irq_chip_eoi_parent(data);
}

static void owl_sirq_mask(struct irq_data *data)
{
	struct owl_sirq_chip_data *chip_data = data->chip_data;
	u32 index = data->hwirq - chip_data->ext_irq_start;

	sirq_clear_set_extctl(chip_data, INTC_EXTCTL_EN, 0, index);
	irq_chip_mask_parent(data);
}

static void owl_sirq_unmask(struct irq_data *data)
{
	struct owl_sirq_chip_data *chip_data = data->chip_data;
	u32 index = data->hwirq - chip_data->ext_irq_start;

	sirq_clear_set_extctl(chip_data, 0, INTC_EXTCTL_EN, index);
	irq_chip_unmask_parent(data);
}

/* PAD_PULLCTL needs to be defined in pinctrl */
static int owl_sirq_set_type(struct irq_data *data, unsigned int flow_type)
{
	struct owl_sirq_chip_data *chip_data = data->chip_data;
	u32 index = data->hwirq - chip_data->ext_irq_start;
	u32 type;

	switch (flow_type) {
	case IRQF_TRIGGER_LOW:
		type = INTC_EXTCTL_TYPE_LOW;
		chip_data->trigger &= ~(1 << index);
		flow_type = IRQF_TRIGGER_HIGH;
		break;
	case IRQF_TRIGGER_HIGH:
		type = INTC_EXTCTL_TYPE_HIGH;
		chip_data->trigger &= ~(1 << index);
		break;
	case IRQF_TRIGGER_FALLING:
		type = INTC_EXTCTL_TYPE_FALLING;
		chip_data->trigger |= 1 << index;
		flow_type = IRQF_TRIGGER_RISING;
		break;
	case IRQF_TRIGGER_RISING:
		type = INTC_EXTCTL_TYPE_RISING;
		chip_data->trigger |= 1 << index;
		break;
	default:
		return  -EINVAL;
	}

	sirq_clear_set_extctl(chip_data, INTC_EXTCTL_TYPE_MASK, type, index);
	return irq_chip_set_type_parent(data, flow_type);
}

static struct irq_chip owl_sirq_chip = {
	.name		= "owl-sirq",
	.irq_mask	= owl_sirq_mask,
	.irq_unmask	= owl_sirq_unmask,
	.irq_eoi	= owl_sirq_eoi,
	.irq_set_type	= owl_sirq_set_type,
	.irq_retrigger	= irq_chip_retrigger_hierarchy,
};

static int owl_sirq_domain_translate(struct irq_domain *d,
				       struct irq_fwspec *fwspec,
				       unsigned long *hwirq,
				       unsigned int *type)
{
	if (is_of_node(fwspec->fwnode)) {
		if (fwspec->param_count != 3)
			return -EINVAL;

		/* No PPI should point to this domain */
		if (fwspec->param[0] != 0)
			return -EINVAL;

		/* sirq support irq number check */
		if (fwspec->param[1] < sirq_data->ext_irq_start ||
		    fwspec->param[1] > sirq_data->ext_irq_end)
			return -EINVAL;

		*hwirq = fwspec->param[1];
		*type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;
		return 0;
	}

	return -EINVAL;
}

static int owl_sirq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				   unsigned int nr_irqs, void *arg)
{
	int i, ret;
	unsigned int type;
	irq_hw_number_t hwirq;
	struct irq_fwspec *fwspec = arg;
	struct irq_fwspec gic_fwspec = *fwspec;

	if (fwspec->param_count != 3)
		return -EINVAL;

	/* sysirq doesn't support PPI */
	if (fwspec->param[0])
		return -EINVAL;

	ret = owl_sirq_domain_translate(domain, arg, &hwirq, &type);
	if (ret)
		return ret;

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
	case IRQ_TYPE_LEVEL_HIGH:
		break;
	case IRQ_TYPE_EDGE_FALLING:
		type = IRQ_TYPE_EDGE_RISING;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		type = IRQ_TYPE_LEVEL_HIGH;
		break;
	default:
		return -EINVAL;
	}

	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
					      &owl_sirq_chip,
					      domain->host_data);

	gic_fwspec.param[2] = type;
	gic_fwspec.fwnode = domain->parent->fwnode;
	return irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, &gic_fwspec);
}


static const struct irq_domain_ops sirq_domain_ops = {
	.translate	= owl_sirq_domain_translate,
	.alloc		= owl_sirq_domain_alloc,
	.free		= irq_domain_free_irqs_common,
};

static int __init owl_sirq_of_init(struct device_node *node,
					struct device_node *parent)
{
	struct irq_domain *domain, *domain_parent;
	int ret = 0, i, sirq_cnt = 0;
	struct owl_sirq_chip_data *chip_data;

	chip_data = kzalloc(sizeof(*chip_data), GFP_KERNEL);
	if (!chip_data)
		return -ENOMEM;

	sirq_data = chip_data;
	raw_spin_lock_init(&chip_data->lock);
	chip_data->base = of_iomap(node, 0);
	if (!chip_data->base) {
		pr_err("owl_sirq: unable to map sirq register\n");
		ret = -ENXIO;
		goto out_free;
	}

	ret = of_property_read_u32_index(node, "actions,ext-irq-range", 0,
					 &chip_data->ext_irq_start);
	if (ret)
		goto out_unmap;

	ret = of_property_read_u32_index(node, "actions,ext-irq-range", 1,
					 &chip_data->ext_irq_end);
	if (ret)
		goto out_unmap;

	sirq_cnt = chip_data->ext_irq_end - chip_data->ext_irq_start + 1;
	chip_data->shared_reg = of_property_read_bool(node,
						"actions,sirq-shared-reg");
	for (i = 0; i < sirq_cnt; i++) {
		u32 value;

		ret = of_property_read_u32_index(node, "actions,sirq-reg-offset",
						i, &value);
		if (ret)
			goto out_unmap;

		chip_data->offset[i] = (u16)value;
		sirq_clear_set_extctl(chip_data, 0, INTC_EXTCTL_CLK_SEL, i);
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
	return ret;
}

IRQCHIP_DECLARE(owl_sirq, "actions,owl-sirq", owl_sirq_of_init);
