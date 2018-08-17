/*
 * Copyright (c) 2017 Sagi Grimberg.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */
#include <linux/blk-mq.h>
#include <linux/blk-mq-rdma.h>
#include <rdma/ib_verbs.h>

static int blk_mq_rdma_map_queue(struct blk_mq_tag_set *set,
		struct ib_device *dev, int first_vec, unsigned int queue)
{
	const struct cpumask *mask;
	unsigned int cpu;
	bool mapped = false;

	mask = ib_get_vector_affinity(dev, first_vec + queue);
	if (!mask)
		return -ENOTSUPP;

	/* map with an unmapped cpu according to affinity mask */
	for_each_cpu(cpu, mask) {
		if (set->mq_map[cpu] == UINT_MAX) {
			set->mq_map[cpu] = queue;
			mapped = true;
			break;
		}
	}

	if (!mapped) {
		int n;

		/* map with an unmapped cpu in the same numa node */
		for_each_node(n) {
			const struct cpumask *node_cpumask = cpumask_of_node(n);

			if (!cpumask_intersects(mask, node_cpumask))
				continue;

			for_each_cpu(cpu, node_cpumask) {
				if (set->mq_map[cpu] == UINT_MAX) {
					set->mq_map[cpu] = queue;
					mapped = true;
					break;
				}
			}
		}
	}

	if (!mapped) {
		/* map with any unmapped cpu we can find */
		for_each_possible_cpu(cpu) {
			if (set->mq_map[cpu] == UINT_MAX) {
				set->mq_map[cpu] = queue;
				mapped = true;
				break;
			}
		}
	}

	WARN_ON_ONCE(!mapped);
	return 0;
}

/**
 * blk_mq_rdma_map_queues - provide a default queue mapping for rdma device
 * @set:	tagset to provide the mapping for
 * @dev:	rdma device associated with @set.
 * @first_vec:	first interrupt vectors to use for queues (usually 0)
 *
 * This function assumes the rdma device @dev has at least as many available
 * interrupt vetors as @set has queues.  It will then query vector affinity mask
 * and attempt to build irq affinity aware queue mappings. If optimal affinity
 * aware mapping cannot be acheived for a given queue, we look for any unmapped
 * cpu to map it. Lastly, we map naively all other unmapped cpus in the mq_map.
 *
 * In case either the driver passed a @dev with less vectors than
 * @set->nr_hw_queues, or @dev does not provide an affinity mask for a
 * vector, we fallback to the naive mapping.
 */
int blk_mq_rdma_map_queues(struct blk_mq_tag_set *set,
                struct ib_device *dev, int first_vec)
{
	unsigned int queue, cpu;

	/* reset cpu mapping */
	for_each_possible_cpu(cpu)
		set->mq_map[cpu] = UINT_MAX;

	for (queue = 0; queue < set->nr_hw_queues; queue++) {
		if (blk_mq_rdma_map_queue(set, dev, first_vec, queue))
			goto fallback;
	}

	/* map any remaining unmapped cpus */
	for_each_possible_cpu(cpu) {
		if (set->mq_map[cpu] == UINT_MAX)
			blk_mq_map_queue_cpu(set, cpu);;
	}

	return 0;
fallback:
	return blk_mq_map_queues(set);
}
EXPORT_SYMBOL_GPL(blk_mq_rdma_map_queues);
