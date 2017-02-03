/*
 * Copyright (C) 2016 Thomas Gleixner.
 * Copyright (C) 2016-2017 Christoph Hellwig.
 */
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include "internals.h"

static cpumask_var_t node_to_present_cpumask[MAX_NUMNODES] __read_mostly;

static void irq_spread_init_one(struct cpumask *irqmsk, struct cpumask *nmsk,
				int cpus_per_vec)
{
	const struct cpumask *siblmsk;
	int cpu, sibl;

	for ( ; cpus_per_vec > 0; ) {
		cpu = cpumask_first(nmsk);

		/* Should not happen, but I'm too lazy to think about it */
		if (cpu >= nr_cpu_ids)
			return;

		cpumask_clear_cpu(cpu, nmsk);
		cpumask_set_cpu(cpu, irqmsk);
		cpus_per_vec--;

		/* If the cpu has siblings, use them first */
		siblmsk = topology_sibling_cpumask(cpu);
		for (sibl = -1; cpus_per_vec > 0; ) {
			sibl = cpumask_next(sibl, siblmsk);
			if (sibl >= nr_cpu_ids)
				break;
			if (!cpumask_test_and_clear_cpu(sibl, nmsk))
				continue;
			cpumask_set_cpu(sibl, irqmsk);
			cpus_per_vec--;
		}
	}
}

static int get_nodes_in_cpumask(const struct cpumask *mask, nodemask_t *nodemsk)
{
	int n, nodes = 0;

	/* Calculate the number of nodes in the supplied affinity mask */
	for_each_node(n) {
		if (cpumask_intersects(mask, node_to_present_cpumask[n])) {
			node_set(n, *nodemsk);
			nodes++;
		}
	}
	return nodes;
}

/**
 * irq_create_affinity_masks - Create affinity masks for multiqueue spreading
 * @nvecs:	The total number of vectors
 * @affd:	Description of the affinity requirements
 *
 * Returns the masks pointer or NULL if allocation failed.
 */
struct cpumask *
irq_create_affinity_masks(int nvecs, const struct irq_affinity *affd)
{
	int n, nodes, vecs_per_node, cpus_per_vec, extra_vecs, curvec;
	int affv = nvecs - affd->pre_vectors - affd->post_vectors;
	int last_affv = affv + affd->pre_vectors;
	nodemask_t nodemsk = NODE_MASK_NONE;
	struct cpumask *masks;
	cpumask_var_t nmsk;

	if (!zalloc_cpumask_var(&nmsk, GFP_KERNEL))
		return NULL;

	masks = kcalloc(nvecs, sizeof(*masks), GFP_KERNEL);
	if (!masks)
		goto out;

	/* Fill out vectors at the beginning that don't need affinity */
	for (curvec = 0; curvec < affd->pre_vectors; curvec++)
		cpumask_copy(masks + curvec, irq_default_affinity);

	nodes = get_nodes_in_cpumask(cpu_present_mask, &nodemsk);

	/*
	 * If the number of nodes in the mask is greater than or equal the
	 * number of vectors we just spread the vectors across the nodes.
	 */
	if (affv <= nodes) {
		for_each_node_mask(n, nodemsk) {
			cpumask_copy(masks + curvec,
				     node_to_present_cpumask[n]);
			if (++curvec == last_affv)
				break;
		}
		goto done;
	}

	/* Spread the vectors per node */
	vecs_per_node = affv / nodes;
	/* Account for rounding errors */
	extra_vecs = affv - (nodes * vecs_per_node);

	for_each_node_mask(n, nodemsk) {
		int ncpus, v, vecs_to_assign = vecs_per_node;

		/* Get the cpus on this node which are in the mask */
		cpumask_and(nmsk, cpu_present_mask, node_to_present_cpumask[n]);

		/* Calculate the number of cpus per vector */
		ncpus = cpumask_weight(nmsk);

		for (v = 0; curvec < last_affv && v < vecs_to_assign;
		     curvec++, v++) {
			cpus_per_vec = ncpus / vecs_to_assign;

			/* Account for extra vectors to compensate rounding errors */
			if (extra_vecs) {
				cpus_per_vec++;
				if (!--extra_vecs)
					vecs_per_node++;
			}
			irq_spread_init_one(masks + curvec, nmsk, cpus_per_vec);
		}

		if (curvec >= last_affv)
			break;
	}

done:
	/* Fill out vectors at the end that don't need affinity */
	for (; curvec < nvecs; curvec++)
		cpumask_copy(masks + curvec, irq_default_affinity);
out:
	free_cpumask_var(nmsk);
	return masks;
}

/**
 * irq_calc_affinity_vectors - Calculate the optimal number of vectors
 * @maxvec:	The maximum number of vectors available
 * @affd:	Description of the affinity requirements
 */
int irq_calc_affinity_vectors(int maxvec, const struct irq_affinity *affd)
{
	int resv = affd->pre_vectors + affd->post_vectors;
	int vecs = maxvec - resv;

	return min_t(int, cpumask_weight(cpu_present_mask), vecs) + resv;
}

static void __irq_affinity_set(unsigned int irq, struct irq_desc *desc,
		cpumask_t *mask)
{
	struct irq_data *data = irq_desc_get_irq_data(desc);
	struct irq_chip *chip = irq_data_get_irq_chip(data);
	int ret;

	if (!irqd_can_move_in_process_context(data) && chip->irq_mask)
		chip->irq_mask(data);
	ret = chip->irq_set_affinity(data, mask, true);
	WARN_ON_ONCE(ret);

	/*
	 * We unmask if the irq was not marked masked by the core code.
	 * That respects the lazy irq disable behaviour.
	 */
	if (!irqd_can_move_in_process_context(data) &&
	    !irqd_irq_masked(data) && chip->irq_unmask)
		chip->irq_unmask(data);
}

static void irq_affinity_online_irq(unsigned int irq, struct irq_desc *desc,
		unsigned int cpu)
{
	const struct cpumask *affinity;
	struct irq_data *data;
	struct irq_chip *chip;
	unsigned long flags;
	cpumask_var_t mask;

	if (!desc)
		return;

	raw_spin_lock_irqsave(&desc->lock, flags);

	data = irq_desc_get_irq_data(desc);
	affinity = irq_data_get_affinity_mask(data);
	if (!irqd_affinity_is_managed(data) ||
	    !irq_has_action(irq) ||
	    !cpumask_test_cpu(cpu, affinity))
		goto out_unlock;

	/*
	 * The interrupt descriptor might have been cleaned up
	 * already, but it is not yet removed from the radix tree
	 */
	chip = irq_data_get_irq_chip(data);
	if (!chip)
		goto out_unlock;

	if (WARN_ON_ONCE(!chip->irq_set_affinity))
		goto out_unlock;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL)) {
		pr_err("failed to allocate memory for cpumask\n");
		goto out_unlock;
	}

	cpumask_and(mask, affinity, cpu_online_mask);
	cpumask_set_cpu(cpu, mask);
	if (irqd_has_set(data, IRQD_AFFINITY_SUSPENDED)) {
		irq_startup(desc, false);
		irqd_clear(data, IRQD_AFFINITY_SUSPENDED);
	} else {
		__irq_affinity_set(irq, desc, mask);
	}

	free_cpumask_var(mask);
out_unlock:
	raw_spin_unlock_irqrestore(&desc->lock, flags);
}

int irq_affinity_online_cpu(unsigned int cpu)
{
	struct irq_desc *desc;
	unsigned int irq;

	for_each_irq_desc(irq, desc)
		irq_affinity_online_irq(irq, desc, cpu);
	return 0;
}

static void irq_affinity_offline_irq(unsigned int irq, struct irq_desc *desc,
		unsigned int cpu)
{
	const struct cpumask *affinity;
	struct irq_data *data;
	struct irq_chip *chip;
	unsigned long flags;
	cpumask_var_t mask;

	if (!desc)
		return;

	raw_spin_lock_irqsave(&desc->lock, flags);

	data = irq_desc_get_irq_data(desc);
	affinity = irq_data_get_affinity_mask(data);
	if (!irqd_affinity_is_managed(data) ||
	    !irq_has_action(irq) ||
	    irqd_has_set(data, IRQD_AFFINITY_SUSPENDED) ||
	    !cpumask_test_cpu(cpu, affinity))
		goto out_unlock;

	/*
	 * Complete the irq move. This cpu is going down and for
	 * non intr-remapping case, we can't wait till this interrupt
	 * arrives at this cpu before completing the irq move.
	 */
	irq_force_complete_move(desc);

	/*
	 * The interrupt descriptor might have been cleaned up
	 * already, but it is not yet removed from the radix tree
	 */
	chip = irq_data_get_irq_chip(data);
	if (!chip)
		goto out_unlock;

	if (WARN_ON_ONCE(!chip->irq_set_affinity))
		goto out_unlock;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL)) {
		pr_err("failed to allocate memory for cpumask\n");
		goto out_unlock;
	}

	cpumask_copy(mask, affinity);
	cpumask_clear_cpu(cpu, mask);
	if (cpumask_empty(mask)) {
		irqd_set(data, IRQD_AFFINITY_SUSPENDED);
		irq_shutdown(desc);
	} else {
		__irq_affinity_set(irq, desc, mask);
	}

	free_cpumask_var(mask);
out_unlock:
	raw_spin_unlock_irqrestore(&desc->lock, flags);
}

int irq_affinity_offline_cpu(unsigned int cpu)
{
	struct irq_desc *desc;
	unsigned int irq;

	for_each_irq_desc(irq, desc)
		irq_affinity_offline_irq(irq, desc, cpu);
	return 0;
}

static int __init irq_build_cpumap(void)
{
	int node, cpu;

	for (node = 0; node < nr_node_ids; node++) {
		if (!zalloc_cpumask_var(&node_to_present_cpumask[node],
				GFP_KERNEL))
			panic("can't allocate early memory\n");
	}

	for_each_present_cpu(cpu) {
		node = cpu_to_node(cpu);
		cpumask_set_cpu(cpu, node_to_present_cpumask[node]);
	}

	return 0;
}

subsys_initcall(irq_build_cpumap);
