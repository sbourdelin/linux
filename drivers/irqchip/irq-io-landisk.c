/*
 * IO-DATA LANDISK CPLD IRQ driver
 *
 * Copyright 2016 Yoshinori Sato <ysato@users.sourceforge.jp>
 */

#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/io.h>

static void landisk_mask_irq(struct irq_data *data)
{
	u8 mask = __raw_readb(data->chip_data + 5);

	mask &= !(1 << (data->irq - 5));
	__raw_writeb(mask, data->chip_data + 5);
}

static void landisk_unmask_irq(struct irq_data *data)
{
	u8 mask = __raw_readb(data->chip_data + 5);

	mask |= (1 << (data->irq - 5));
	__raw_writeb(mask, data->chip_data + 5);
}

static struct irq_chip cpld_irq_chip = {
	.name		= "LANDISK-CPLD",
	.irq_unmask	= landisk_unmask_irq,
	.irq_mask	= landisk_mask_irq,
};

static int cpld_map(struct irq_domain *d, unsigned int virq,
		    irq_hw_number_t hw_irq_num)
{
	irq_set_chip_and_handler(virq, &cpld_irq_chip,
				 handle_simple_irq);
	irq_set_chip_data(virq, d->host_data);

	return 0;
}

static struct irq_domain_ops irq_ops = {
	.xlate	= irq_domain_xlate_twocell,
	.map	= cpld_map,
};

static int __init landisk_intc_of_init(struct device_node *intc,
				    struct device_node *parent)
{
	struct irq_domain *domain, *pdomain;
	int num_irqpin;
	void *baseaddr;

	baseaddr = of_iomap(intc, 0);
	pdomain = irq_find_host(parent);
	of_get_property(intc, "interrupt-map", &num_irqpin);
	num_irqpin /= sizeof(u32) * 3;
	domain = irq_domain_create_hierarchy(pdomain, 0, num_irqpin,
					     of_node_to_fwnode(intc),
					     &irq_ops, baseaddr);
	BUG_ON(!domain);
	irq_domain_associate_many(domain, 0, 0, 8);
	return 0;
}

IRQCHIP_DECLARE(cpld_intc, "iodata,landisk-intc", landisk_intc_of_init);
