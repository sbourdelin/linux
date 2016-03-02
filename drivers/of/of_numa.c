/*
 * OF NUMA Parsing support.
 *
 * Copyright (C) 2015 - 2016 Cavium Inc.
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

#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/nodemask.h>

#include <asm/numa.h>

/* define default numa node to 0 */
#define DEFAULT_NODE 0

/*
 * Even though we connect cpus to numa domains later in SMP
 * init, we need to know the node ids now for all cpus.
*/
static void __init of_find_cpu_nodes(void)
{
	u32 nid;
	int r;
	struct device_node *np = NULL;

	for (;;) {
		np = of_find_node_by_type(np, "cpu");
		if (!np)
			break;

		r = of_property_read_u32(np, "numa-node-id", &nid);
		if (r)
			continue;

		pr_debug("NUMA: CPU on %u\n", nid);
		if (nid >= MAX_NUMNODES)
			pr_warn("NUMA: Node id %u exceeds maximum value\n",
				nid);
		else
			node_set(nid, numa_nodes_parsed);
	}
}

static void __init of_parse_memory_nodes(void)
{
	struct device_node *np = NULL;
	int na, ns;
	const __be32 *prop;
	unsigned int psize;
	u32 nid;
	u64 base, size;
	int r;

	for (;;) {
		np = of_find_node_by_type(np, "memory");
		if (!np)
			break;

		r = of_property_read_u32(np, "numa-node-id", &nid);
		if (r)
			continue;

		prop = of_get_property(np, "reg", &psize);
		if (!prop)
			continue;

		psize /= sizeof(__be32);
		na = of_n_addr_cells(np);
		ns = of_n_size_cells(np);

		if (psize < na + ns) {
			pr_err("NUMA: memory reg property too small\n");
			continue;
		}
		base = of_read_number(prop, na);
		size = of_read_number(prop + na, ns);

		pr_debug("NUMA:  base = %llx len = %llx, node = %u\n",
			 base, size, nid);

		if (numa_add_memblk(nid, base, size) < 0)
			break;
	}

	of_node_put(np);
}

static int __init parse_distance_map_v1(struct device_node *map)
{
	const __be32 *matrix;
	unsigned int matrix_size;
	int entry_count;
	int i;
	int nr_size_cells = OF_ROOT_NODE_SIZE_CELLS_DEFAULT;

	pr_info("NUMA: parsing numa-distance-map-v1\n");

	matrix = of_get_property(map, "distance-matrix", &matrix_size);
	if (!matrix) {
		pr_err("NUMA: No distance-matrix property in distance-map\n");
		return -EINVAL;
	}

	entry_count = matrix_size / (sizeof(__be32) * 3 * nr_size_cells);

	for (i = 0; i < entry_count; i++) {
		u32 nodea, nodeb, distance;

		nodea = of_read_number(matrix, nr_size_cells);
		matrix += nr_size_cells;
		nodeb = of_read_number(matrix, nr_size_cells);
		matrix += nr_size_cells;
		distance = of_read_number(matrix, nr_size_cells);
		matrix += nr_size_cells;

		numa_set_distance(nodea, nodeb, distance);
		pr_debug("NUMA:  distance[node%d -> node%d] = %d\n",
			 nodea, nodeb, distance);

		/* Set default distance of node B->A same as A->B */
		if (nodeb > nodea)
			numa_set_distance(nodeb, nodea, distance);
	}

	return 0;
}

static int __init of_parse_distance_map(void)
{
	int ret = -EINVAL;
	struct device_node *np = of_find_node_by_name(NULL, "distance-map");

	if (!np)
		return ret;

	if (of_device_is_compatible(np, "numa-distance-map-v1")) {
		ret = parse_distance_map_v1(np);
		goto out;
	}

	pr_err("NUMA: invalid distance-map device node\n");
out:
	of_node_put(np);
	return ret;
}

int of_node_to_nid(struct device_node *device)
{
	struct device_node *np;
	u32 nid;
	int r = -ENODATA;

	np = of_node_get(device);

	while (np) {
		struct device_node *parent;

		r = of_property_read_u32(np, "numa-node-id", &nid);
		if (r != -EINVAL)
			break;

		/* property doesn't exist in this node, look in parent */
		parent = of_get_parent(np);
		of_node_put(np);
		np = parent;
	}
	if (np && r)
		pr_warn("NUMA: Invalid \"numa-node-id\" property in node %s\n",
			np->name);
	of_node_put(np);

	if (!r) {
		if (nid >= MAX_NUMNODES)
			pr_warn("NUMA: Node id %u exceeds maximum value\n",
				nid);
		else
			return nid;
	}

	return NUMA_NO_NODE;
}

int __init of_numa_init(void)
{
	of_find_cpu_nodes();
	of_parse_memory_nodes();
	return of_parse_distance_map();
}
