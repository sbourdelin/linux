/*
 * PMU support
 *
 * Copyright (C) 2015 Red Hat Inc.
 * Author: Mark Salter <msalter@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include <linux/perf/arm_pmu.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>

struct pmu_irq {
	int gsi;
	int trigger;
};

static struct pmu_irq pmu_irqs[NR_CPUS] __initdata;

/*
 * Called from acpi_map_gic_cpu_interface()'s MADT parsing during boot.
 * This routine saves off the GSI's and their trigger state for use when we are
 * ready to build the PMU platform device.
*/
void __init arm_pmu_parse_acpi(int cpu, struct acpi_madt_generic_interrupt *gic)
{
	pmu_irqs[cpu].gsi = gic->performance_interrupt;
	if (gic->flags & ACPI_MADT_PERFORMANCE_IRQ_MODE)
		pmu_irqs[cpu].trigger = ACPI_EDGE_SENSITIVE;
	else
		pmu_irqs[cpu].trigger = ACPI_LEVEL_SENSITIVE;
}

static int __init pmu_acpi_init(void)
{
	struct platform_device *pdev;
	struct pmu_irq *pirq = pmu_irqs;
	struct resource	*res, *r;
	int err = -ENOMEM;
	int i, count, irq;

	if (acpi_disabled)
		return 0;

	/* Must have irq for boot cpu, at least */
	if (pirq->gsi == 0)
		return -EINVAL;

	irq = acpi_register_gsi(NULL, pirq->gsi, pirq->trigger,
				ACPI_ACTIVE_HIGH);

	if (irq_is_percpu(irq))
		count = 1;
	else
		for (i = 1, count = 1; i < NR_CPUS; i++)
			if (pmu_irqs[i].gsi)
				++count;

	pdev = platform_device_alloc(ARMV8_PMU_PDEV_NAME, -1);
	if (!pdev)
		goto err_free_gsi;

	res = kcalloc(count, sizeof(*res), GFP_KERNEL);
	if (!res)
		goto err_free_device;

	for (i = 0, r = res; i < count; i++, pirq++, r++) {
		if (i)
			irq = acpi_register_gsi(NULL, pirq->gsi, pirq->trigger,
						ACPI_ACTIVE_HIGH);
		r->start = r->end = irq;
		r->flags = IORESOURCE_IRQ;
		if (pirq->trigger == ACPI_EDGE_SENSITIVE)
			r->flags |= IORESOURCE_IRQ_HIGHEDGE;
		else
			r->flags |= IORESOURCE_IRQ_HIGHLEVEL;
	}

	err = platform_device_add_resources(pdev, res, count);
	if (!err)
		err = platform_device_add(pdev);
	kfree(res);
	if (!err)
		return 0;

err_free_device:
	platform_device_put(pdev);

err_free_gsi:
	for (i = 0; i < count; i++)
		acpi_unregister_gsi(pmu_irqs[i].gsi);

	return err;
}
arch_initcall(pmu_acpi_init);
