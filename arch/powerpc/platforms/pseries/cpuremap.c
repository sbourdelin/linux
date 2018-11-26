// SPDX-License-Identifier: GPL-2.0
#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <asm/prom.h>
#include <asm/topology.h>

struct cpuremap_cpu {
	int thread_index;
		/* Set to thread_index from ibm,ppc-interrupt-server#s arrays
		 * Don't clear when release'ed
		 */
	int node;
	bool in_use;
		/* Set to true when reserve'ed
		 * Don't clear when release'ed
		*/
};

struct cpuremap_struct {
	int num_nodes;
	int num_cores;
	int num_threads_per_core;
	struct cpuremap_cpu *threads;
} cpuremap_data;


void cpuremap_init(void)
{
	int i, k;

	/* Identify necessary constants & alloc memory at boot */
	cpuremap_data.num_threads_per_core = 8;
	cpuremap_data.num_cores = 32;
	cpuremap_data.num_nodes =
		nr_cpu_ids /
		(cpuremap_data.num_threads_per_core * cpuremap_data.num_cores);
	cpuremap_data.threads = kcalloc(nr_cpu_ids, sizeof(struct cpuremap_cpu), GFP_KERNEL);

	k = cpuremap_data.num_nodes *
		cpuremap_data.num_threads_per_core *
		cpuremap_data.num_cores;
	for (i = 0; i < k; k++)
		cpuremap_data.threads[i].thread_index = CPUREMAP_NO_THREAD;
}

int cpuremap_thread_to_cpu(int thread_index)
{
	int i, k;

	/* Return NO_CPU if not found */
	for (i = thread_index, k = 0; k < nr_cpu_ids; k++) {
		if (cpuremap_data.threads[i].in_use &&
		    (cpuremap_data.threads[i].thread_index == thread_index)) {
			cpuremap_data.threads[i].in_use = true;
			cpuremap_data.threads[i].thread_index = thread_index;
			return i;
		}
		if (i >= nr_cpu_ids)
			i = 0;
	}
	return CPUREMAP_NO_CPU;
}

int cpuremap_cpu_to_thread(int cpu)
{
	/* Return NO_THREAD if not found */
	if (cpuremap_data.threads[cpu].in_use)
		return cpuremap_data.threads[cpu].thread_index;
	return CPUREMAP_NO_THREAD;
}

int cpuremap_map_cpu(int thread_index, int in_core_ndx, int node)
{
	int first_thread, i, k;

	/* Return NO_CPU if fails */
	first_thread = (node *
		(cpuremap_data.num_threads_per_core *
		 cpuremap_data.num_cores)) + in_core_ndx;

	/* Alternative 0: Compressed map of cpus+nodes+threads
	 *   assuming that no system will be fully built out.
	 * Alternative 1: Fully compact.  Allocate new cpu ids
	 *   as needed.  No 'pretty' separation between nodes.
	 * Alternative 2: Also map incoming nodes from pHyp
	 *   to virtual nodes for purposes of new cpu ids.
	 */

	if (first_thread > nr_cpu_ids)
		first_thread = 0 + in_core_ndx;
	for (i = first_thread, k = 0; k < nr_cpu_ids; k++) {
		if (!cpuremap_data.threads[i].in_use || (cpuremap_data.threads[i].thread_index == thread_index)) {
			cpuremap_data.threads[i].thread_index = thread_index;
			cpuremap_data.threads[i].node = node;
			return i;
		}
		if (i >= nr_cpu_ids)
			i = 0;
	}
	return CPUREMAP_NO_CPU;
}

int cpuremap_reserve_cpu(int cpu)
{
	if (!cpuremap_data.threads[cpu].in_use) {
		cpuremap_data.threads[cpu].in_use = true;
		return cpu;
	}
	return CPUREMAP_NO_CPU;
}

int cpuremap_release_cpu(int cpu)
{
	if (cpuremap_data.threads[cpu].in_use) {
		cpuremap_data.threads[cpu].in_use = false;
		return cpu;
	}
	return CPUREMAP_NO_CPU;
}

int cpuremap_free_cpu(int cpu)
{
	/* Return NO_CPU if fails */
	if (cpuremap_data.threads[cpu].in_use) {
		cpuremap_data.threads[cpu].in_use = false;
		return cpu;
	}
	return CPUREMAP_NO_CPU;
}
