/*
 * ARM ACPI PMU support
 *
 * Copyright (C) 2015 Red Hat Inc.
 * Copyright (C) 2016 ARM Ltd.
 * Author: Mark Salter <msalter@redhat.com>
 *	   Jeremy Linton <jeremy.linton@arm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#define pr_fmt(fmt) "ACPI-PMU: " fmt

#include <asm/cpu.h>
#include <linux/acpi.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/list.h>
#include <linux/perf/arm_pmu.h>
#include <linux/platform_device.h>

struct pmu_irq {
	int  gsi;
	int  trigger;
	bool registered;
};

struct pmu_types {
	struct list_head list;
	int		 cpu_type;
	int		 cpu_count;
};

static struct pmu_irq pmu_irqs[NR_CPUS];

/*
 * Called from acpi_verify_and_map_madt()'s MADT parsing during boot.
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

static void __init arm_pmu_acpi_handle_alloc_failure(struct list_head *pmus)
{
	struct pmu_types *pmu, *safe_temp;

	list_for_each_entry_safe(pmu, safe_temp, pmus, list) {
		list_del(&pmu->list);
		kfree(pmu);
	}
}

/* Count number and type of CPU cores in the system. */
static bool __init arm_pmu_acpi_determine_cpu_types(struct list_head *pmus)
{
	int i;
	bool unused_madt_entries = false;

	for_each_possible_cpu(i) {
		struct cpuinfo_arm64 *cinfo = per_cpu_ptr(&cpu_data, i);
		u32 partnum = MIDR_PARTNUM(cinfo->reg_midr);
		struct pmu_types *pmu;

		if (cinfo->reg_midr == 0) {
			unused_madt_entries = true;
			continue;
		}

		list_for_each_entry(pmu, pmus, list) {
			if (pmu->cpu_type == partnum) {
				pmu->cpu_count++;
				break;
			}
		}

		/* we didn't find the CPU type, add an entry to identify it */
		if (&pmu->list == pmus) {
			pmu = kzalloc(sizeof(struct pmu_types), GFP_KERNEL);
			if (!pmu) {
				pr_err("Unable to allocate pmu_types\n");
				/*
				 * Instead of waiting to cleanup possible
				 * allocation failures in the caller clean
				 * them up immediately. Functionally this
				 * doesn't make any difference, except in
				 * genuine heterogeneous systems where it
				 * guarantees the whole subsystem is
				 * disabled rather than running with just
				 * a single set of homogeneous CPU's PMU
				 * active. That assumes there aren't
				 * any further memory allocation failures.
				 */
				arm_pmu_acpi_handle_alloc_failure(pmus);
				break;
			} else {
				pmu->cpu_type = partnum;
				pmu->cpu_count++;
				list_add_tail(&pmu->list, pmus);
			}
		}
	}

	return unused_madt_entries;
}

/*
 * Registers the group of PMU interfaces which correspond to the 'last_cpu_id'.
 * This group utilizes 'count' resources in the 'res'.
 */
static int __init arm_pmu_acpi_register_pmu(int count, struct resource *res,
					    int last_cpu_id)
{
	int i;
	int err = -ENOMEM;
	bool free_gsi = false;
	struct platform_device *pdev;

	if (count) {
		pdev = platform_device_alloc(ARMV8_PMU_PDEV_NAME, last_cpu_id);
		if (pdev) {
			err = platform_device_add_resources(pdev, res, count);
			if (!err) {
				err = platform_device_add(pdev);
				if (err) {
					pr_warn("Unable to register PMU device\n");
					free_gsi = true;
				}
			} else {
				pr_warn("Unable to add resources to device\n");
				free_gsi = true;
				platform_device_put(pdev);
			}
		} else {
			pr_warn("Unable to allocate platform device\n");
			free_gsi = true;
		}
	}

	/* unmark (and possibly unregister) registered GSIs */
	for_each_possible_cpu(i) {
		if (pmu_irqs[i].registered) {
			if (free_gsi)
				acpi_unregister_gsi(pmu_irqs[i].gsi);
			pmu_irqs[i].registered = false;
		}
	}

	return err;
}

int arm_pmu_acpi_retrieve_irq(struct resource *res, int cpu)
{
	int irq = -ENODEV;

	if (pmu_irqs[cpu].registered) {
		pr_info("CPU %d's interrupt is already registered\n", cpu);
	} else {
		irq = acpi_register_gsi(NULL, pmu_irqs[cpu].gsi,
					pmu_irqs[cpu].trigger,
					ACPI_ACTIVE_HIGH);
		pmu_irqs[cpu].registered = true;
		res->start = irq;
		res->end = irq;
		res->flags = IORESOURCE_IRQ;
		if (pmu_irqs[cpu].trigger == ACPI_EDGE_SENSITIVE)
			res->flags |= IORESOURCE_IRQ_HIGHEDGE;
		else
			res->flags |= IORESOURCE_IRQ_HIGHLEVEL;
	}
	return irq;
}

/*
 * For the given cpu/pmu type, walk all known GSIs, register them, and add
 * them to the resource structure. Return the number of GSI's contained
 * in the res structure, and the id of the last CPU/PMU we added.
 */
static int __init arm_pmu_acpi_gsi_res(struct pmu_types *pmus,
				       struct resource *res, int *last_cpu_id)
{
	int i, count;

	/* lets group all the PMU's from similar CPU's together */
	count = 0;
	for_each_possible_cpu(i) {
		struct cpuinfo_arm64 *cinfo = per_cpu_ptr(&cpu_data, i);

		if (pmus->cpu_type == MIDR_PARTNUM(cinfo->reg_midr)) {
			if ((pmu_irqs[i].gsi == 0) && (cinfo->reg_midr != 0)) {
				pr_info("CPU %d is assigned interrupt 0\n", i);
				continue;
			}
			/* likely not online */
			if (!cinfo->reg_midr)
				continue;

			arm_pmu_acpi_retrieve_irq(&res[count], i);
			count++;
			(*last_cpu_id) = cinfo->reg_midr;
		}
	}
	return count;
}

static int __init pmu_acpi_init(void)
{
	struct resource	*res;
	int err = -ENOMEM;
	int count, cpu_id;
	struct pmu_types *pmu, *safe_temp;
	bool unused_madt_entries;
	LIST_HEAD(pmus);

	if (acpi_disabled)
		return 0;

	unused_madt_entries = arm_pmu_acpi_determine_cpu_types(&pmus);

	list_for_each_entry_safe(pmu, safe_temp, &pmus, list) {
		if (unused_madt_entries)
			pmu->cpu_count = num_possible_cpus();

		res = kcalloc(pmu->cpu_count,
				  sizeof(struct resource), GFP_KERNEL);

		/* for a given PMU type collect all the GSIs. */
		if (res) {
			count = arm_pmu_acpi_gsi_res(pmu, res,
						     &cpu_id);
			/*
			 * register this set of interrupts
			 * with a new PMU device
			 */
			if (count) {
				if (unused_madt_entries)
					count = num_possible_cpus();
				err = arm_pmu_acpi_register_pmu(count, res,
								cpu_id);
				if (!err)
					pr_info("Register %d devices for %X\n",
						count, pmu->cpu_type);
				kfree(res);
			}
		} else {
			pr_warn("PMU unable to allocate interrupt resource\n");
		}

		list_del(&pmu->list);
		kfree(pmu);
	}

	return err;
}

arch_initcall(pmu_acpi_init);
