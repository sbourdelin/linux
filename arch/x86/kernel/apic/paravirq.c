/*
 * An irq_domain for interrupts injected by the hypervisor using
 * Intel VT-x technology.
 *
 * Copyright (C) 2016 Alexander Popov <alex.popov@linux.com>.
 *
 * This file is released under the GPLv2.
 */

#include <linux/init.h>
#include <linux/irq.h>
#include <asm/irqdomain.h>
#include <asm/paravirq.h>

static struct irq_domain *paravirq_domain;

static struct irq_chip paravirq_chip = {
	.name			= "PARAVIRQ",
	.irq_ack		= irq_chip_ack_parent,
};

static int paravirq_domain_alloc(struct irq_domain *domain,
			unsigned int virq, unsigned int nr_irqs, void *arg)
{
	int ret = 0;

	BUG_ON(domain != paravirq_domain);

	if (nr_irqs != 1)
		return -EINVAL;

	ret = irq_domain_set_hwirq_and_chip(paravirq_domain,
					virq, virq, &paravirq_chip, NULL);
	if (ret) {
		pr_warn("setting chip, hwirq for irq %u failed\n", virq);
		return ret;
	}

	__irq_set_handler(virq, handle_edge_irq, 0, "edge");

	return 0;
}

static void paravirq_domain_free(struct irq_domain *domain,
					unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *irq_data;

	BUG_ON(domain != paravirq_domain);
	BUG_ON(nr_irqs != 1);

	irq_data = irq_domain_get_irq_data(paravirq_domain, virq);
	if (irq_data)
		irq_domain_reset_irq_data(irq_data);
	else
		pr_warn("irq %u is not in paravirq irq_domain\n", virq);
}

static const struct irq_domain_ops paravirq_domain_ops = {
	.alloc	= paravirq_domain_alloc,
	.free	= paravirq_domain_free,
};

int paravirq_alloc_irq(void)
{
	struct irq_alloc_info info;

	if (!paravirq_domain)
		return -ENODEV;

	if (!paravirq_chip.irq_mask || !paravirq_chip.irq_unmask)
		return -EINVAL;

	init_irq_alloc_info(&info, NULL);

	return irq_domain_alloc_irqs(paravirq_domain, 1, NUMA_NO_NODE, &info);
}
EXPORT_SYMBOL(paravirq_alloc_irq);

void paravirq_free_irq(unsigned int virq)
{
	struct irq_data *irq_data;

	if (!paravirq_domain) {
		pr_warn("paravirq irq_domain is not initialized\n");
		return;
	}

	irq_data = irq_domain_get_irq_data(paravirq_domain, virq);
	if (irq_data)
		irq_domain_free_irqs(virq, 1);
	else
		pr_warn("irq %u is not in paravirq irq_domain\n", virq);
}
EXPORT_SYMBOL(paravirq_free_irq);

int paravirq_init_chip(void (*irq_mask)(struct irq_data *data),
				void (*irq_unmask)(struct irq_data *data))
{
	if (!paravirq_domain)
		return -ENODEV;

	if (paravirq_chip.irq_mask || paravirq_chip.irq_unmask)
		return -EEXIST;

	if (!irq_mask || !irq_unmask)
		return -EINVAL;

	paravirq_chip.irq_mask = irq_mask;
	paravirq_chip.irq_unmask = irq_unmask;

	return 0;
}
EXPORT_SYMBOL(paravirq_init_chip);

void arch_init_paravirq_domain(struct irq_domain *parent)
{
	paravirq_domain = irq_domain_add_tree(NULL, &paravirq_domain_ops, NULL);
	if (!paravirq_domain) {
		pr_warn("failed to initialize paravirq irq_domain\n");
		return;
	}

	paravirq_domain->name = paravirq_chip.name;
	paravirq_domain->parent = parent;
	paravirq_domain->flags |= IRQ_DOMAIN_FLAG_AUTO_RECURSIVE;
}

