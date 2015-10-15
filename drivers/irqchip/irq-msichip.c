/*
 * Non-functionnal example for a wired irq <-> MSI bridge
 *
 * Copyright (C) 2015 ARM Limited, All Rights Reserved.
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * DT fragment to represent the MSI bridge:

	intc: msichip {
		compatible = "dummy,msichip";
		num-msis = 32;
		interrupt-controller;
		interrupt-parent = <&gic>;
		#interrupt-cells = <0x2>;
		msi-parent = <&its 1234>;
	};

 * DT fragment to represent the device connected to the bridge:

	dummy-dev {
		compatible = "dummy,device";
		interrupt-parent = <intc>;
		interrupts = <0x5 0x1>;
	};

 * When "dummy,device" gets probed, it dumps the hierarchy for the
 * interrupt it has allocated:

	dummydev dummy-dev: Allocated IRQ35
	dummydev dummy-dev: Probing OK
	dummydev dummy-dev: IRQ35 hwirq 5 domain msichip_domain_ops
	dummydev dummy-dev: IRQ35 hwirq 0 domain msi_domain_ops
	dummydev dummy-dev: IRQ35 hwirq 8192 domain its_domain_ops
	dummydev dummy-dev: IRQ35 hwirq 8192 domain gic_irq_domain_ops
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

static void msichip_mask(struct irq_data *data)
{
	/* Do something */
}

static void msichip_unmask(struct irq_data *data)
{
	/* Do something */
}

static void msichip_eoi(struct irq_data *data)
{
	/* Do something */
}

static int msichip_set_type(struct irq_data *data, unsigned int type)
{
	/* Do something */
	return 0;
}

static int msichip_retrigger(struct irq_data *data)
{
	/* Do something */
	return 0;
}

static int msichip_set_affinity(struct irq_data *data,
				const struct cpumask *dest, bool force)
{
	/* Do something */
	return 0;
}

static struct irq_chip msichip_chip = {
	.name			= "MSICHIP",
	.irq_mask		= msichip_mask,
	.irq_unmask		= msichip_unmask,
	.irq_eoi		= msichip_eoi,
	.irq_set_type		= msichip_set_type,
	.irq_retrigger		= msichip_retrigger,
	.irq_set_affinity	= msichip_set_affinity,
};

static int msichip_domain_translate(struct irq_domain *d,
				    struct irq_fwspec *fwspec,
				    unsigned long *hwirq,
				    unsigned int *type)
{
	if (is_of_node(fwspec->fwnode)) {
		if (fwspec->param_count != 2)
			return -EINVAL;

		*hwirq = fwspec->param[0];
		*type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;
		return 0;
	}

	return -EINVAL;
}

static int msichip_domain_alloc(struct irq_domain *domain, unsigned int virq,
				   unsigned int nr_irqs, void *arg)
{
	int i, err;
	irq_hw_number_t hwirq;
	unsigned int type;
	struct irq_fwspec *fwspec = arg;
	void *data;

	err = msichip_domain_translate(domain, fwspec, &hwirq, &type);
	if (err)
		return err;

	err = platform_msi_domain_alloc(domain, virq, nr_irqs);
	if (err)
		return err;

	data = platform_msi_get_host_data(domain);
	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
					      &msichip_chip, data);

	return 0;
}

static const struct irq_domain_ops msichip_domain_ops = {
	.translate	= msichip_domain_translate,
	.alloc		= msichip_domain_alloc,
	.free		= irq_domain_free_irqs_common,
};

struct msichip_data {
	/* Add whatever you fancy here */
	struct platform_device *pdev;
};

static void msichip_write_msi_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	/* Do the right thing */
}

static int msichip_probe(struct platform_device *pdev)
{
	struct irq_domain *domain;
	struct msichip_data *msichip_data;
	u32 num_msis;

	dev_info(&pdev->dev, "Probing\n");
	msichip_data = kzalloc(sizeof(*msichip_data), GFP_KERNEL);
	if (!msichip_data)
		return -ENOMEM;

	msichip_data->pdev = pdev;

	/* If there is no "num-msi" property, assume 64... */
	if (of_property_read_u32(pdev->dev.of_node, "num-msis", &num_msis) < 0)
		num_msis = 64;

	dev_info(&pdev->dev, "allocating %d MSIs\n", num_msis);

	domain = platform_msi_create_device_domain(&pdev->dev, num_msis,
						   msichip_write_msi_msg,
						   &msichip_domain_ops,
						   msichip_data);

	if (!domain){
		kfree(msichip_data);
		return -ENOMEM;
	}

	dev_info(&pdev->dev, "Probing OK\n");
	return 0;
}

static const struct of_device_id msichip_of_match[] = {
	{ .compatible = "dummy,msichip", },
	{ },
};
MODULE_DEVICE_TABLE(of, msichip_of_match);

static struct platform_driver msichip_driver = {
	.driver = {
		.name		= "msichip",
		.of_match_table	= msichip_of_match,
	},
	.probe			= msichip_probe,
};
/* Do not define this as an irqchip */
module_platform_driver(msichip_driver);



/* Driver for a dummy device connected to the MSI bridge */
static irqreturn_t dummydev_handler(int irq, void *dummy)
{
	return IRQ_HANDLED;
}

static void dummydev_dump_hierarchy(struct device *dev, int irq)
{
	struct irq_data *data = irq_get_irq_data(irq);

	while(data) {
		dev_info(dev, "IRQ%d hwirq %d domain %ps\n",
			 data->irq, (int)data->hwirq, data->domain->ops);
		data = data->parent_data;
	}
}

static int dummydev_probe(struct platform_device *pdev)
{
	int irq;

	dev_info(&pdev->dev, "Probing\n");
	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (!irq) {
		dev_err(&pdev->dev, "irq allocation failed, defering\n");
		return -EPROBE_DEFER;
	}

	dev_info(&pdev->dev, "Allocated IRQ%d\n", irq);

	if (request_irq(irq, dummydev_handler, 0, "dummydev", pdev))
		return -EINVAL;

	dev_info(&pdev->dev, "Probing OK\n");

	dummydev_dump_hierarchy(&pdev->dev, irq);
	return 0;
}

static const struct of_device_id dummydev_of_match[] = {
	{ .compatible = "dummy,device", },
	{ },
};
MODULE_DEVICE_TABLE(of, dummydev_of_match);

static struct platform_driver dummydev_driver = {
	.driver = {
		.name		= "dummydev",
		.of_match_table	= dummydev_of_match,
	},
	.probe			= dummydev_probe,
};

module_platform_driver(dummydev_driver);
