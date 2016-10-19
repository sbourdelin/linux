/*
 * Copyright (c) 2015 Endless Mobile, Inc.
 * Author: Carlo Caione <carlo@endlessm.com>
 * Copyright (c) 2016 BayLibre, SAS.
 * Author: Jerome Brunet <jbrunet@baylibre.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 * The full GNU General Public License is included in this distribution
 * in the file called COPYING.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/io.h>
#include <linux/module.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/irqchip.h>
#include <linux/of.h>
#include <linux/of_address.h>

#define IRQ_FREE (-1)

#define REG_EDGE_POL	0x00
#define REG_PIN_03_SEL	0x04
#define REG_PIN_47_SEL	0x08
#define REG_FILTER_SEL	0x0c

#define REG_EDGE_POL_MASK(x)	(BIT(x) | BIT(16 + (x)))
#define REG_EDGE_POL_EDGE(x)	BIT(x)
#define REG_EDGE_POL_LOW(x)	BIT(16 + (x))
#define REG_PIN_SEL_SHIFT(x)	(((x) % 4) * 8)
#define REG_FILTER_SEL_SHIFT(x)	((x) * 4)

struct meson_gpio_irq_params {
	unsigned int nhwirq;
	irq_hw_number_t *source;
	int nsource;
};

struct meson_gpio_irq_domain {
	void __iomem *base;
	int *map;
	const struct meson_gpio_irq_params *params;
};

struct meson_gpio_irq_chip_data {
	void __iomem *base;
	int index;
};

static irq_hw_number_t meson_parent_hwirqs[] = {
	64, 65, 66, 67, 68, 69, 70, 71,
};

static const struct meson_gpio_irq_params meson8_params = {
	.nhwirq  = 134,
	.source  = meson_parent_hwirqs,
	.nsource = ARRAY_SIZE(meson_parent_hwirqs),
};

static const struct meson_gpio_irq_params meson8b_params = {
	.nhwirq  = 119,
	.source  = meson_parent_hwirqs,
	.nsource = ARRAY_SIZE(meson_parent_hwirqs),
};

static const struct meson_gpio_irq_params meson_gxbb_params = {
	.nhwirq  = 133,
	.source  = meson_parent_hwirqs,
	.nsource = ARRAY_SIZE(meson_parent_hwirqs),
};

static const struct of_device_id meson_irq_gpio_matches[] = {
	{
		.compatible = "amlogic,meson8-gpio-intc",
		.data = &meson8_params
	},
	{
		.compatible = "amlogic,meson8b-gpio-intc",
		.data = &meson8b_params
	},
	{
		.compatible = "amlogic,meson-gxbb-gpio-intc",
		.data = &meson_gxbb_params
	},
	{}
};

static void meson_gpio_irq_update_bits(void __iomem *base, unsigned int reg,
				       u32 mask, u32 val)
{
	u32 tmp;

	tmp = readl(base + reg);
	tmp &= ~mask;
	tmp |= val;

	writel(tmp, base + reg);
}

static int meson_gpio_irq_get_index(struct meson_gpio_irq_domain *domain_data,
				    int hwirq)
{
	int i;

	for (i = 0; i < domain_data->params->nsource; i++) {
		if (domain_data->map[i] == hwirq)
			return i;
	}

	return -1;
}

static int mesion_gpio_irq_map_source(struct meson_gpio_irq_domain *domain_data,
				      irq_hw_number_t hwirq,
				      irq_hw_number_t *source)
{
	int index;
	unsigned int reg;

	index = meson_gpio_irq_get_index(domain_data, IRQ_FREE);
	if (index < 0) {
		pr_err("No irq available\n");
		return -ENOSPC;
	}

	domain_data->map[index] = hwirq;

	reg = (index < 4) ? REG_PIN_03_SEL : REG_PIN_47_SEL;
	meson_gpio_irq_update_bits(domain_data->base, reg,
				   0xff << REG_PIN_SEL_SHIFT(index),
				   hwirq << REG_PIN_SEL_SHIFT(index));

	*source = domain_data->params->source[index];

	pr_debug("hwirq %lu assigned to channel %d - source %lu\n",
		 hwirq, index, *source);

	return index;
}

static int meson_gpio_irq_type_setup(unsigned int type, void __iomem *base,
				     int index)
{
	u32 val = 0;

	type &= IRQ_TYPE_SENSE_MASK;

	if (type == IRQ_TYPE_EDGE_BOTH)
		return -EINVAL;

	if (type & (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING))
		val |= REG_EDGE_POL_EDGE(index);

	if (type & (IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_EDGE_FALLING))
		val |= REG_EDGE_POL_LOW(index);

	meson_gpio_irq_update_bits(base, REG_EDGE_POL,
				   REG_EDGE_POL_MASK(index), val);

	return 0;
}

static unsigned int meson_gpio_irq_type_output(unsigned int type)
{
	unsigned int sense = type & IRQ_TYPE_SENSE_MASK;

	type &= ~IRQ_TYPE_SENSE_MASK;

	/*
	 * If the polarity of interrupt is low, the controller will
	 * invert the signal for gic
	 */
	if (sense & (IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW))
		type |= IRQ_TYPE_LEVEL_HIGH;
	else if (sense & (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING))
		type |= IRQ_TYPE_EDGE_RISING;

	return type;
}

static int meson_gpio_irq_set_type(struct irq_data *data, unsigned int type)
{
	struct meson_gpio_irq_chip_data *cd = irq_data_get_irq_chip_data(data);
	int ret;

	pr_debug("set type of hwirq %lu to %u\n", data->hwirq, type);

	ret = meson_gpio_irq_type_setup(type, cd->base, cd->index);
	if (ret)
		return ret;

	return irq_chip_set_type_parent(data,
					meson_gpio_irq_type_output(type));
}

static struct irq_chip meson_gpio_irq_chip = {
	.name			= "meson-gpio-irqchip",
	.irq_mask		= irq_chip_mask_parent,
	.irq_unmask		= irq_chip_unmask_parent,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_set_type		= meson_gpio_irq_set_type,
	.irq_retrigger		= irq_chip_retrigger_hierarchy,
#ifdef CONFIG_SMP
	.irq_set_affinity	= irq_chip_set_affinity_parent,
#endif
};

static int meson_gpio_irq_domain_translate(struct irq_domain *domain,
					   struct irq_fwspec *fwspec,
					   unsigned long *hwirq,
					   unsigned int *type)
{
	if (is_of_node(fwspec->fwnode)) {
		if (fwspec->param_count != 2)
			return -EINVAL;

		*hwirq	= fwspec->param[0];
		*type	= fwspec->param[1];

		return 0;
	}

	return -EINVAL;
}

static int meson_gpio_irq_allocate_gic_irq(struct irq_domain *domain,
					   unsigned int virq,
					   irq_hw_number_t source,
					   unsigned int type)
{
	struct irq_fwspec fwspec;

	if (!irq_domain_get_of_node(domain->parent))
		return -EINVAL;

	fwspec.fwnode = domain->parent->fwnode;
	fwspec.param_count = 3;
	fwspec.param[0] = 0;	/* SPI */
	fwspec.param[1] = source;
	fwspec.param[2] = meson_gpio_irq_type_output(type);

	return irq_domain_alloc_irqs_parent(domain, virq, 1, &fwspec);
}

static int meson_gpio_irq_domain_alloc(struct irq_domain *domain,
				       unsigned int virq,
				       unsigned int nr_irqs,
				       void *data)
{
	struct irq_fwspec *fwspec = data;
	struct meson_gpio_irq_domain *domain_data = domain->host_data;
	struct meson_gpio_irq_chip_data *cd;
	unsigned long hwirq, source;
	unsigned int type;
	int i, index, ret;

	ret = meson_gpio_irq_domain_translate(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	pr_debug("irq %d, nr_irqs %d, hwirqs %lu\n", virq, nr_irqs, hwirq);

	for (i = 0; i < nr_irqs; i++) {
		index = mesion_gpio_irq_map_source(domain_data, hwirq + i,
						   &source);
		if (index < 0)
			return index;

		ret = meson_gpio_irq_type_setup(type, domain_data->base,
						index);
		if (ret)
			return ret;

		cd = kzalloc(sizeof(*cd), GFP_KERNEL);
		if (!cd)
			return -ENOMEM;

		cd->base = domain_data->base;
		cd->index = index;

		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
					      &meson_gpio_irq_chip, cd);

		ret = meson_gpio_irq_allocate_gic_irq(domain, virq + i,
						      source, type);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static void meson_gpio_irq_domain_free(struct irq_domain *domain,
				       unsigned int virq,
				       unsigned int nr_irqs)
{
	struct meson_gpio_irq_domain *domain_data = domain->host_data;
	struct meson_gpio_irq_chip_data *cd;
	struct irq_data *irq_data;
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_data = irq_domain_get_irq_data(domain, virq + i);
		cd = irq_data_get_irq_chip_data(irq_data);

		domain_data->map[cd->index] = IRQ_FREE;
		kfree(cd);
	}

	irq_domain_free_irqs_parent(domain, virq, nr_irqs);

}

static const struct irq_domain_ops meson_gpio_irq_domain_ops = {
	.alloc		= meson_gpio_irq_domain_alloc,
	.free		= meson_gpio_irq_domain_free,
	.translate	= meson_gpio_irq_domain_translate,
};

static int __init
meson_gpio_irq_init_domain(struct device_node *node,
			   struct meson_gpio_irq_domain *domain_data,
			   const struct meson_gpio_irq_params *params)
{
	int i;
	int nsource = params->nsource;
	int *map;

	map = kcalloc(nsource, sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	for (i = 0; i < nsource; i++)
		map[i] = IRQ_FREE;

	domain_data->map = map;
	domain_data->params = params;

	return 0;
}

static int __init meson_gpio_irq_of_init(struct device_node *node,
					 struct device_node *parent)
{
	struct irq_domain *domain, *parent_domain;
	const struct of_device_id *match;
	const struct meson_gpio_irq_params *params;
	struct meson_gpio_irq_domain *domain_data;
	int ret;

	match = of_match_node(meson_irq_gpio_matches, node);
	if (!match)
		return -ENODEV;
	params = match->data;

	if (!parent) {
		pr_err("missing parent interrupt node\n");
		return -ENODEV;
	}

	parent_domain = irq_find_host(parent);
	if (!parent_domain) {
		pr_err("unable to obtain parent domain\n");
		return -ENXIO;
	}

	domain_data = kzalloc(sizeof(*domain_data), GFP_KERNEL);
	if (!domain_data)
		return -ENOMEM;

	domain_data->base = of_iomap(node, 0);
	if (!domain_data->base) {
		ret = -ENOMEM;
		goto out_free_dev;
	}

	ret = meson_gpio_irq_init_domain(node, domain_data, params);
	if (ret < 0)
		goto out_free_dev_content;

	domain = irq_domain_add_hierarchy(parent_domain, 0, params->nhwirq,
					  node, &meson_gpio_irq_domain_ops,
					  domain_data);

	if (!domain) {
		pr_err("failed to allocated domain\n");
		ret = -ENOMEM;
		goto out_free_dev_content;
	}

	pr_info("%d to %d gpio interrupt mux initialized\n",
		params->nhwirq, params->nsource);

	return 0;

out_free_dev_content:
	kfree(domain_data->map);
	iounmap(domain_data->base);

out_free_dev:
	kfree(domain_data);

	return ret;
}

IRQCHIP_DECLARE(meson8_gpio_intc, "amlogic,meson8-gpio-intc",
		meson_gpio_irq_of_init);
IRQCHIP_DECLARE(meson8b_gpio_intc, "amlogic,meson8b-gpio-intc",
		meson_gpio_irq_of_init);
IRQCHIP_DECLARE(gxbb_gpio_intc, "amlogic,meson-gxbb-gpio-intc",
		meson_gpio_irq_of_init);
