/*
 * linux/kernel/irq/ipi.c
 *
 * Copyright (C) 2015 Imagination Technologies Ltd
 * Author: Qais Yousef <qais.yousef@imgtec.com>
 *
 * This file contains driver APIs to the IPI subsystem.
 */

#include <linux/irq.h>
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
