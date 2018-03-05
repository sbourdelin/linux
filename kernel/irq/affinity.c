// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2016 Thomas Gleixner.
 * Copyright (C) 2016-2017 Christoph Hellwig.
 */
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/cpu.h>

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

static cpumask_var_t *alloc_node_to_cpumask(void)
{
	cpumask_var_t *masks;
	int node;

	masks = kcalloc(nr_node_ids, sizeof(cpumask_var_t), GFP_KERNEL);
	if (!masks)
		return NULL;

	for (node = 0; node < nr_node_ids; node++) {
		if (!zalloc_cpumask_var(&masks[node], GFP_KERNEL))
			goto out_unwind;
	}

	return masks;

out_unwind:
	while (--node >= 0)
		free_cpumask_var(masks[node]);
	kfree(masks);
	return NULL;
}

static void free_node_to_cpumask(cpumask_var_t *masks)
{
	int node;

	for (node = 0; node < nr_node_ids; node++)
		free_cpumask_var(masks[node]);
	kfree(masks);
}

static void build_node_to_cpumask(cpumask_var_t *masks)
{
	int cpu;

	for_each_possible_cpu(cpu)
		cpumask_set_cpu(cpu, masks[cpu_to_node(cpu)]);
}

static int get_nodes_in_cpumask(const cpumask_var_t *node_to_cpumask,
				const struct cpumask *mask, nodemask_t *nodemsk)
{
	int n, nodes = 0;

	/* Calculate the number of nodes in the supplied affinity mask */
	for_each_node(n) {
		if (cpumask_intersects(mask, node_to_cpumask[n])) {
			node_set(n, *nodemsk);
			nodes++;
		}
	}
	return nodes;
}

static int irq_build_affinity_masks(const struct irq_affinity *affd,
				    const int start_vec, const int affv,
				    const cpumask_var_t *node_to_cpumask,
				    const struct cpumask *cpu_mask,
				    struct cpumask *nmsk,
				    struct cpumask *masks)
{
	int last_affv = affv + affd->pre_vectors;
	int curvec = start_vec;
	nodemask_t nodemsk = NODE_MASK_NONE;
	int n, nodes, cpus_per_vec, extra_vecs, done = 0;

	if (!cpumask_weight(cpu_mask))
		return 0;

	nodes = get_nodes_in_cpumask(node_to_cpumask, cpu_mask, &nodemsk);

	/*
	 * If the number of nodes in the mask is greater than or equal the
	 * number of vectors we just spread the vectors across the nodes.
	 */
	if (affv <= nodes) {
		for_each_node_mask(n, nodemsk) {
			cpumask_copy(masks + curvec,
				     node_to_cpumask[n]);
			if (++done == affv)
				break;
			if (++curvec == last_affv)
				curvec = affd->pre_vectors;
		}
		goto out;
	}

	for_each_node_mask(n, nodemsk) {
		int ncpus, v, vecs_to_assign, vecs_per_node;

		/* Spread the vectors per node */
		vecs_per_node = (affv - (curvec - affd->pre_vectors)) / nodes;

		/* Get the cpus on this node which are in the mask */
		cpumask_and(nmsk, cpu_mask, node_to_cpumask[n]);

		/* Calculate the number of cpus per vector */
		ncpus = cpumask_weight(nmsk);
		vecs_to_assign = min(vecs_per_node, ncpus);

		/* Account for rounding errors */
		extra_vecs = ncpus - vecs_to_assign * (ncpus / vecs_to_assign);

		for (v = 0; curvec < last_affv && v < vecs_to_assign;
		     curvec++, v++) {
			cpus_per_vec = ncpus / vecs_to_assign;

			/* Account for extra vectors to compensate rounding errors */
			if (extra_vecs) {
				cpus_per_vec++;
				--extra_vecs;
			}
			irq_spread_init_one(masks + curvec, nmsk, cpus_per_vec);
		}

		done += v;
		if (done >= affv)
			break;
		if (curvec >= last_affv)
			curvec = affd->pre_vectors;
		--nodes;
	}

out:
	return done;
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
	int affv = nvecs - affd->pre_vectors - affd->post_vectors;
	int curvec, vecs_offline, vecs_online;
	struct cpumask *masks;
	cpumask_var_t nmsk, cpu_mask, *node_to_cpumask;

	/*
	 * If there aren't any vectors left after applying the pre/post
	 * vectors don't bother with assigning affinity.
	 */
	if (nvecs == affd->pre_vectors + affd->post_vectors)
		return NULL;

	if (!zalloc_cpumask_var(&nmsk, GFP_KERNEL))
		return NULL;

	masks = kcalloc(nvecs, sizeof(*masks), GFP_KERNEL);
	if (!masks)
		goto out;

	if (!alloc_cpumask_var(&cpu_mask, GFP_KERNEL))
		goto out;

	node_to_cpumask = alloc_node_to_cpumask();
	if (!node_to_cpumask)
		goto out_free_cpu_mask;

	/* Fill out vectors at the beginning that don't need affinity */
	for (curvec = 0; curvec < affd->pre_vectors; curvec++)
		cpumask_copy(masks + curvec, irq_default_affinity);

	/* Stabilize the cpumasks */
	get_online_cpus();
	build_node_to_cpumask(node_to_cpumask);
	/* spread on online CPUs starting from the vector of affd->pre_vectors */
	vecs_online = irq_build_affinity_masks(affd, curvec, affv,
					       node_to_cpumask,
					       cpu_online_mask, nmsk, masks);

	/* spread on offline CPUs starting from the next vector to be handled */
	if (vecs_online >= affv)
		curvec = affd->pre_vectors;
	else
		curvec = affd->pre_vectors + vecs_online;
	cpumask_andnot(cpu_mask, cpu_possible_mask, cpu_online_mask);
	vecs_offline = irq_build_affinity_masks(affd, curvec, affv,
						node_to_cpumask,
					        cpu_mask, nmsk, masks);
	put_online_cpus();

	/* Fill out vectors at the end that don't need affinity */
	if (vecs_online + vecs_offline >= affv)
		curvec = affv + affd->pre_vectors;
	else
		curvec = affd->pre_vectors + vecs_online + vecs_offline;
	for (; curvec < nvecs; curvec++)
		cpumask_copy(masks + curvec, irq_default_affinity);
	free_node_to_cpumask(node_to_cpumask);
out_free_cpu_mask:
	free_cpumask_var(cpu_mask);
out:
	free_cpumask_var(nmsk);
	return masks;
}

/**
 * irq_calc_affinity_vectors - Calculate the optimal number of vectors
 * @minvec:	The minimum number of vectors available
 * @maxvec:	The maximum number of vectors available
 * @affd:	Description of the affinity requirements
 */
int irq_calc_affinity_vectors(int minvec, int maxvec, const struct irq_affinity *affd)
{
	int resv = affd->pre_vectors + affd->post_vectors;
	int vecs = maxvec - resv;
	int ret;

	if (resv > minvec)
		return 0;

	get_online_cpus();
	ret = min_t(int, cpumask_weight(cpu_possible_mask), vecs) + resv;
	put_online_cpus();
	return ret;
}
