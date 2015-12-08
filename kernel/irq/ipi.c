/*
 * linux/kernel/irq/ipi.c
 *
 * Copyright (C) 2015 Imagination Technologies Ltd
 * Author: Qais Yousef <qais.yousef@imgtec.com>
 *
 * This file contains driver APIs to the IPI subsystem.
 */

#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/slab.h>

/**
 * irq_alloc_ipi_mapping - allocate memory for struct ipi_mapping
 * @nr_cpus: number of CPUs the mapping will have
 *
 * Will allocate and setup ipi_mapping structure.
 *
 * Returns a valid ipi_mapping pointer on success and NULL on error.
 */
struct ipi_mapping *irq_alloc_ipi_mapping(unsigned int nr_cpus)
{
	struct ipi_mapping *map;
	size_t size;

	size = sizeof(struct ipi_mapping) + BITS_TO_LONGS(nr_cpus) * sizeof(long);

	map = kzalloc(size, GFP_KERNEL);
	if (!map)
		return NULL;

	map->nr_cpus = nr_cpus;

	memset(map->cpumap, INVALID_HWIRQ, size);

	return map;
}

/**
 * irq_free_ipi_mapping - release mempry associated with ipi_mapping struct
 * @map: pointer to struct ipi_mapping to be freed
 *
 * Release the memory allocated for sturct ipi_mapping to the system.
 */
void irq_free_ipi_mapping(struct ipi_mapping *map)
{
	kfree(map);
}

/**
 * irq_map_ipi - create a CPU to HWIRQ mapping for an IPI
 * @map: pointer to the mapping structure
 * @cpu: the CPU to map
 * @hwirq: the HWIRQ to associate with @cpu
 *
 * Returns zero on success and negative error number on failure.
 */
int irq_map_ipi(struct ipi_mapping *map,
		unsigned int cpu, irq_hw_number_t hwirq)
{
	if (cpu >= map->nr_cpus)
		return -EINVAL;

	map->cpumap[cpu] = hwirq;
	map->nr_hwirqs++;

	return 0;
}

/**
 * irq_unmap_ipi - remove the CPU mapping of an IPI
 * @map: pointer to the mapping structure
 * @cpu: the CPU to be unmapped
 *
 * Mark the IPI mapping of a CPU to INVALID_HWIRQ.
 *
 * Returns zero on success and negative error number on failure.
 */
int irq_unmap_ipi(struct ipi_mapping *map, unsigned int cpu)
{
	if (cpu >= map->nr_cpus)
		return -EINVAL;

	if (map->cpumap[cpu] == INVALID_HWIRQ)
		return -EINVAL;

	map->cpumap[cpu] = INVALID_HWIRQ;
	map->nr_hwirqs--;

	return 0;
}

/**
 * irq_ipi_mapping_get_hwirq - get the value of hwirq associated with @cpu
 * @map: pointer to the mapping structure
 * @cpu: the CPU to get its associated hwirq
 *
 * Return the hwiq asocaited with a @cpu
 *
 * Returns hwirq value on success and INVALID_HWIRQ on failure.
 */
irq_hw_number_t irq_ipi_mapping_get_hwirq(struct ipi_mapping *map,
					  unsigned int cpu)
{
	if (cpu >= map->nr_cpus)
		return INVALID_HWIRQ;

	return map->cpumap[cpu];
}

/**
 * irq_reserve_ipi() - setup an IPI to destination cpumask
 * @domain: IPI domain
 * @dest: cpumask of cpus to receive the IPI
 *
 * Allocate a virq that can be used to send IPI to any CPU in dest mask.
 *
 * On success it'll return linux irq number and 0 on failure
 */
unsigned int irq_reserve_ipi(struct irq_domain *domain,
			     const struct cpumask *dest)
{
	struct irq_data *data;
	unsigned int nr_irqs, offset = 0;
	int prev_cpu = -1, cpu;
	int virq, i;

	if (domain == NULL) {
		pr_warn("Must provide a valid IPI domain!\n");
		return 0;
	}

	if (!irq_domain_is_ipi(domain)) {
		pr_warn("Not an IPI domain!\n");
		return 0;
	}

	if (!cpumask_subset(dest, cpu_possible_mask)) {
		pr_warn("Can't reserve an IPI outside cpu_possible_mask range\n");
		return 0;
	}

	nr_irqs = cpumask_weight(dest);
	if (!nr_irqs) {
		pr_warn("Can't reserve an IPI for an empty mask\n");
		return 0;
	}

	if (irq_domain_is_ipi_single(domain))
		nr_irqs = 1;

	/*
	 * Disallow holes in the ipi mask.
	 * Holes makes it difficult to manage code in generic way. So we always
	 * assume a consecutive ipi mask. It's easy for the user to split
	 * an ipi mask with a hole into 2 consecutive ipi masks and manage
	 * which virq to use locally than adding generic support that would
	 * complicate the generic code.
	 */
	for_each_cpu(cpu, dest) {
		if (prev_cpu == -1) {
			/* while at it save the offset */
			offset = cpu;
			prev_cpu = cpu;
			continue;
		}

		if (prev_cpu - cpu > 1) {
			pr_err("Can't allocate IPIs using non consecutive mask\n");
			return 0;
		}

		prev_cpu = cpu;
	}

	virq = irq_domain_alloc_descs(-1, nr_irqs, 0, NUMA_NO_NODE);
	if (virq <= 0) {
		pr_warn("Can't reserve IPI, failed to alloc descs\n");
		return 0;
	}

	virq = __irq_domain_alloc_irqs(domain, virq, nr_irqs, NUMA_NO_NODE,
					(void *) dest, true);
	if (virq <= 0) {
		pr_warn("Can't reserve IPI, failed to alloc irqs\n");
		goto free_descs;
	}

	for (i = 0; i < nr_irqs; i++) {
		data = irq_get_irq_data(virq + i);
		cpumask_copy(data->common->affinity, dest);
		data->common->ipi_offset = offset;
	}

	return virq;

free_descs:
	irq_free_descs(virq, nr_irqs);
	return 0;
}

/**
 * irq_destroy_ipi() - unreserve an IPI that was previously allocated
 * @irq: linux irq number to be destroyed
 *
 * Return the IPIs allocated with irq_reserve_ipi() to the system destroying all
 * virqs associated with them.
 */
void irq_destroy_ipi(unsigned int irq)
{
	struct irq_data *data = irq_get_irq_data(irq);
	struct cpumask *ipimask = data ? irq_data_get_affinity_mask(data) : NULL;
	struct irq_domain *domain;
	unsigned int nr_irqs;

	if (!irq || !data || !ipimask)
		return;

	domain = data->domain;
	if (WARN_ON(domain == NULL))
		return;

	if (!irq_domain_is_ipi(domain)) {
		pr_warn("Not an IPI domain!\n");
		return;
	}

	if (irq_domain_is_ipi_per_cpu(domain))
		nr_irqs = cpumask_weight(ipimask);
	else
		nr_irqs = 1;

	irq_domain_free_irqs(irq, nr_irqs);
}
