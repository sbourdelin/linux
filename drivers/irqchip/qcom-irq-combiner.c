/* Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * Driver for interrupt combiners in the Top-level Control and Status
 * Registers (TCSR) hardware block in Qualcomm Technologies chips.
 * An interrupt combiner in this block combines a set of interrupts by
 * OR'ing the individual interrupt signals into a summary interrupt
 * signal routed to a parent interrupt controller, and provides read-
 * only, 32-bit registers to query the status of individual interrupts.
 * The status bit for IRQ n is bit (n % 32) within register (n / 32)
 * of the given combiner. Thus, each combiner can be described as a set
 * of register offsets and the number of IRQs managed.
 */

#include <linux/acpi.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>

#define REG_SIZE 32

struct combiner_reg {
	void __iomem *addr;
	unsigned long mask;
};

struct combiner {
	struct irq_chip     irq_chip;
	struct irq_domain   *domain;
	int                 parent_irq;
	u32                 nirqs;
	u32                 nregs;
	struct combiner_reg regs[0];
};

static inline u32 irq_register(int irq)
{
	return irq / REG_SIZE;
}

static inline u32 irq_bit(int irq)
{
	return irq % REG_SIZE;

}

static inline int irq_nr(u32 reg, u32 bit)
{
	return reg * REG_SIZE + bit;
}

/*
 * Handler for the cascaded IRQ.
 */
static void combiner_handle_irq(struct irq_desc *desc)
{
	struct combiner *combiner = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u32 reg;

	chained_irq_enter(chip, desc);

	for (reg = 0; reg < combiner->nregs; reg++) {
		int virq;
		int hwirq;
		u32 bit;
		u32 status;

		if (combiner->regs[reg].mask == 0)
			continue;

		status = readl_relaxed(combiner->regs[reg].addr);
		status &= combiner->regs[reg].mask;

		while (status) {
			bit = __ffs(status);
			status &= ~(1 << bit);
			hwirq = irq_nr(reg, bit);
			virq = irq_find_mapping(combiner->domain, hwirq);
			if (virq >= 0)
				generic_handle_irq(virq);

		}
	}

	chained_irq_exit(chip, desc);
}

/*
 * irqchip callbacks
 */

static void combiner_irq_chip_mask_irq(struct irq_data *data)
{
	struct combiner *combiner = irq_data_get_irq_chip_data(data);
	struct combiner_reg *reg = combiner->regs + irq_register(data->hwirq);

	clear_bit(irq_bit(data->hwirq), &reg->mask);
}

static void combiner_irq_chip_unmask_irq(struct irq_data *data)
{
	struct combiner *combiner = irq_data_get_irq_chip_data(data);
	struct combiner_reg *reg = combiner->regs + irq_register(data->hwirq);

	set_bit(irq_bit(data->hwirq), &reg->mask);
}

#ifdef CONFIG_SMP
static int combiner_irq_chip_set_affinity(struct irq_data *data,
					 const struct cpumask *mask, bool force)
{
	struct combiner *combiner = irq_data_get_irq_chip_data(data);
	struct irq_data *pdata = irq_get_irq_data(combiner->parent_irq);

	if (pdata->chip && pdata->chip->irq_set_affinity)
		return pdata->chip->irq_set_affinity(pdata, mask, force);
	else
		return -EINVAL;
}
#endif

/*
 * domain callbacks
 */

static int combiner_irq_map(struct irq_domain *domain, unsigned int irq,
				   irq_hw_number_t hwirq)
{
	struct combiner *combiner = domain->host_data;

	if (hwirq >= combiner->nirqs)
		return -EINVAL;

	irq_set_chip_and_handler(irq, &combiner->irq_chip, handle_level_irq);
	irq_set_chip_data(irq, combiner);
	irq_set_parent(irq, combiner->parent_irq);
	irq_set_noprobe(irq);
	return 0;
}

static void combiner_irq_unmap(struct irq_domain *domain, unsigned int irq)
{
	irq_set_chip_and_handler(irq, NULL, NULL);
	irq_set_chip_data(irq, NULL);
	irq_set_parent(irq, -1);
}

static struct irq_domain_ops domain_ops = {
	.map = combiner_irq_map,
	.unmap = combiner_irq_unmap
};

/*
 * Probing and initialization.
 *
 * Combiner devices reside inside the TCSR block so the resulting DSDT
 * topology is:
 *
 * Device (TCS0)
 * {
 *         Name (_HID, "QCOM80B0") // Qualcomm TCSR controller
 *         Name (_UID, 0)
 *
 *         Method (_CRS, 0x0, Serialized) {
 *                 Name (RBUF, ResourceTemplate ()
 *                 {
 *                         Memory32Fixed (ReadWrite, 0x2E10000, 0x00001000)
 *                 })
 *                 Return (RBUF)
 *         }
 *
 *         Device (QIC0)
 *         {
 *                 Name(_HID,"QCOM80B1") // Qualcomm TCSR IRQ combiner
 *                 ...
 *         } // end Device QIC0
 *         ...
 * }
 *
 * Thus all combiner devices same the same memory mapping from the parent
 * device.
 */

static int __init combiner_probe(struct platform_device *pdev)
{
	struct platform_device *tcsr_pdev;
	struct combiner *combiner;
	void __iomem *tcsr_base;
	size_t alloc_sz;
	u32 nregs;
	u32 nirqs;

	tcsr_pdev = to_platform_device(pdev->dev.parent);
	tcsr_base = platform_get_drvdata(tcsr_pdev);
	if (!tcsr_base)
		return -ENODEV;

	if (device_property_read_u32(&pdev->dev, "qcom,combiner-nr-irqs",
				     &nirqs)) {
		dev_err(&pdev->dev, "Error reading number of IRQs\n");
		return -EINVAL;
	}

	nregs = device_property_read_u32_array(&pdev->dev, "qcom,combiner-regs",
					       NULL, 0);
	if (nregs < DIV_ROUND_UP(nirqs, REG_SIZE)) {
		dev_err(&pdev->dev, "Error reading regs property\n");
		return -EINVAL;
	}

	alloc_sz = sizeof(*combiner) + sizeof(struct combiner_reg) * nregs;
	combiner = devm_kzalloc(&pdev->dev, alloc_sz, GFP_KERNEL);
	if (combiner) {
		int i;
		u32 regs[nregs];

		if (device_property_read_u32_array(&pdev->dev,
						   "qcom,combiner-regs",
						   regs, nregs)) {
			dev_err(&pdev->dev, "Error reading regs property\n");
			return -EINVAL;
		}

		combiner->nirqs = nirqs;
		combiner->nregs = nregs;
		for (i = 0; i < nregs; i++) {
			combiner->regs[i].addr = tcsr_base + regs[i];
			combiner->regs[i].mask = 0;
		}
	} else {
		return -ENOMEM;
	}

	combiner->irq_chip.irq_mask   = combiner_irq_chip_mask_irq;
	combiner->irq_chip.irq_unmask = combiner_irq_chip_unmask_irq;
#ifdef CONFIG_SMP
	combiner->irq_chip.irq_set_affinity = combiner_irq_chip_set_affinity;
#endif

	combiner->parent_irq = platform_get_irq(pdev, 0);
	if (combiner->parent_irq <= 0) {
		dev_err(&pdev->dev, "Error getting IRQ resource\n");
		return -EINVAL;
	}

	combiner->domain = irq_domain_create_linear(pdev->dev.fwnode, nirqs,
						    &domain_ops, combiner);
	if (!combiner->domain)
		/* Errors printed by irq_domain_create_linear */
		return -ENODEV;

	irq_set_chained_handler_and_data(combiner->parent_irq,
					 combiner_handle_irq, combiner);

	if (device_property_read_string(&pdev->dev, "qcom,combiner-name",
					&combiner->irq_chip.name) != 0)
		combiner->irq_chip.name = "qcom-irq-combiner";

	dev_info(&pdev->dev, "Initialized with [p=%d,n=%d,r=%p]\n",
		 combiner->parent_irq, nirqs, combiner->regs[0].addr);
	return 0;
}

static const struct acpi_device_id qcom_irq_combiner_acpi_match[] = {
	{ "QCOM80B1", },
	{ }
};

static struct platform_driver qcom_irq_combiner_probe = {
	.driver = {
		.name = "qcom-irq-combiner",
		.owner = THIS_MODULE,
		.acpi_match_table = ACPI_PTR(qcom_irq_combiner_acpi_match),
	},
	.probe = combiner_probe,
};

static int __init register_qcom_irq_combiner(void)
{
	return platform_driver_register(&qcom_irq_combiner_probe);
}
arch_initcall(register_qcom_irq_combiner);

static int __init tcsr_probe(struct platform_device *pdev)
{
	struct resource *mr;
	void __iomem *tcsr_base;

	mr = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (mr == NULL) {
		dev_err(&pdev->dev, "Error getting memory resource\n");
		return -EINVAL;
	}

	tcsr_base = devm_ioremap_resource(&pdev->dev, mr);
	if (IS_ERR(tcsr_base)) {
		dev_err(&pdev->dev, "Error mapping memory resource\n");
		return PTR_ERR(tcsr_base);
	}

	dev_info(&pdev->dev, "Initialized TCSR block @%pa\n", &mr->start);
	platform_set_drvdata(pdev, tcsr_base);
	return 0;
}

static const struct acpi_device_id qcom_tcsr_acpi_match[] = {
	{ "QCOM80B0", },
	{ }
};

static struct platform_driver qcom_tcsr_probe = {
	.driver = {
		.name = "qcom-tcsr",
		.owner = THIS_MODULE,
		.acpi_match_table = ACPI_PTR(qcom_tcsr_acpi_match),
	},
	.probe = tcsr_probe,
};

static int __init register_qcom_tcsr(void)
{
	return platform_driver_register(&qcom_tcsr_probe);
}
arch_initcall(register_qcom_tcsr);
