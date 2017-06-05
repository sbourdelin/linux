/*
 * Heterogeneous memory representation in sysfs
 *
 * Copyright (c) 2017, Intel Corporation.
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

#include <acpi/acpi_numa.h>
#include <linux/acpi.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include "hmem.h"

static LIST_HEAD(target_list);
static LIST_HEAD(initiator_list);
LIST_HEAD(locality_list);

static bool bad_hmem;

static int add_performance_attributes(struct memory_target *tgt)
{
	struct attribute_group performance_attribute_group = {
		.attrs = performance_attributes,
	};
	struct kobject *init_kobj, *tgt_kobj;
	struct device *init_dev, *tgt_dev;
	char via_init[128], via_tgt[128];
	int ret;

	if (!tgt->local_init)
		return 0;

	init_dev = &tgt->local_init->dev;
	tgt_dev = &tgt->dev;
	init_kobj = &init_dev->kobj;
	tgt_kobj = &tgt_dev->kobj;

	snprintf(via_init, 128, "via_%s", dev_name(init_dev));
	snprintf(via_tgt, 128, "via_%s", dev_name(tgt_dev));

	/* Create entries for initiator/target pair in the target.  */
	performance_attribute_group.name = via_init;
	ret = sysfs_create_group(tgt_kobj, &performance_attribute_group);
	if (ret < 0)
		return ret;

	ret = sysfs_add_link_to_group(tgt_kobj, via_init, init_kobj,
			dev_name(init_dev));
	if (ret < 0)
		goto err;

	ret = sysfs_add_link_to_group(tgt_kobj, via_init, tgt_kobj,
			dev_name(tgt_dev));
	if (ret < 0)
		goto err;

	/* Create a link in the initiator to the performance attributes. */
	ret = sysfs_add_group_link(init_kobj, tgt_kobj, via_init, via_tgt);
	if (ret < 0)
		goto err;

	tgt->has_perf_attributes = true;
	return 0;
err:
	/* Removals of links that haven't been added yet are harmless. */
	sysfs_remove_link_from_group(tgt_kobj, via_init, dev_name(init_dev));
	sysfs_remove_link_from_group(tgt_kobj, via_init, dev_name(tgt_dev));
	sysfs_remove_group(tgt_kobj, &performance_attribute_group);
	return ret;
}

static void remove_performance_attributes(struct memory_target *tgt)
{
	struct attribute_group performance_attribute_group = {
		.attrs = performance_attributes,
	};
	struct kobject *init_kobj, *tgt_kobj;
	struct device *init_dev, *tgt_dev;
	char via_init[128], via_tgt[128];

	if (!tgt->local_init)
		return;

	init_dev = &tgt->local_init->dev;
	tgt_dev = &tgt->dev;
	init_kobj = &init_dev->kobj;
	tgt_kobj = &tgt_dev->kobj;

	snprintf(via_init, 128, "via_%s", dev_name(init_dev));
	snprintf(via_tgt, 128, "via_%s", dev_name(tgt_dev));

	performance_attribute_group.name = via_init;

	/* Remove entries for initiator/target pair in the target.  */
	sysfs_remove_link_from_group(tgt_kobj, via_init, dev_name(init_dev));
	sysfs_remove_link_from_group(tgt_kobj, via_init, dev_name(tgt_dev));

	/* Remove the initiator's link to the performance attributes. */
	sysfs_remove_link(init_kobj, via_tgt);

	sysfs_remove_group(tgt_kobj, &performance_attribute_group);
}

static int link_node_for_kobj(unsigned int node, struct kobject *kobj)
{
	if (node_devices[node])
		return sysfs_create_link(kobj, &node_devices[node]->dev.kobj,
				kobject_name(&node_devices[node]->dev.kobj));

	return 0;
}

static void remove_node_for_kobj(unsigned int node, struct kobject *kobj)
{
	if (node_devices[node])
		sysfs_remove_link(kobj,
				kobject_name(&node_devices[node]->dev.kobj));
}

#define HMEM_CLASS_NAME	"hmem"

static struct bus_type hmem_subsys = {
	/*
	 * .dev_name is set before device_register() based on the type of
	 * device we are registering.
	 */
	.name = HMEM_CLASS_NAME,
};

/* memory initiators */
static int link_cpu_under_mem_init(struct memory_initiator *init)
{
	struct device *cpu_dev;
	int cpu;

	for_each_online_cpu(cpu) {
		cpu_dev = get_cpu_device(cpu);
		if (!cpu_dev)
			continue;

		if (pxm_to_node(init->pxm) == cpu_to_node(cpu)) {
			return sysfs_create_link(&init->dev.kobj,
					&cpu_dev->kobj,
					kobject_name(&cpu_dev->kobj));
		}

	}
	return 0;
}

static void remove_cpu_under_mem_init(struct memory_initiator *init)
{
	struct device *cpu_dev;
	int cpu;

	for_each_online_cpu(cpu) {
		cpu_dev = get_cpu_device(cpu);
		if (!cpu_dev)
			continue;

		if (pxm_to_node(init->pxm) == cpu_to_node(cpu)) {
			sysfs_remove_link(&init->dev.kobj,
					kobject_name(&cpu_dev->kobj));
			return;
		}

	}
}

static void release_memory_initiator(struct device *dev)
{
	struct memory_initiator *init = to_memory_initiator(dev);

	list_del(&init->list);
	kfree(init);
}

static void __init remove_memory_initiator(struct memory_initiator *init)
{
	if (init->is_registered) {
		remove_cpu_under_mem_init(init);
		remove_node_for_kobj(pxm_to_node(init->pxm), &init->dev.kobj);
		device_unregister(&init->dev);
	} else
		release_memory_initiator(&init->dev);
}

static int __init register_memory_initiator(struct memory_initiator *init)
{
	int ret;

	hmem_subsys.dev_name = "mem_init";
	init->dev.bus = &hmem_subsys;
	init->dev.id = pxm_to_node(init->pxm);
	init->dev.release = release_memory_initiator;
	init->dev.groups = memory_initiator_attribute_groups;

	ret = device_register(&init->dev);
	if (ret < 0)
		return ret;

	init->is_registered = true;

	ret = link_cpu_under_mem_init(init);
	if (ret < 0)
		return ret;

	return link_node_for_kobj(pxm_to_node(init->pxm), &init->dev.kobj);
}

static struct memory_initiator * __init add_memory_initiator(int pxm)
{
	struct memory_initiator *init;

	if (pxm_to_node(pxm) == NUMA_NO_NODE) {
		pr_err("HMEM: No NUMA node for PXM %d\n", pxm);
		bad_hmem = true;
		return ERR_PTR(-EINVAL);
	}

	init = kzalloc(sizeof(*init), GFP_KERNEL);
	if (!init) {
		bad_hmem = true;
		return ERR_PTR(-ENOMEM);
	}

	init->pxm = pxm;

	list_add_tail(&init->list, &initiator_list);
	return init;
}

/* memory targets */
static void release_memory_target(struct device *dev)
{
	struct memory_target *tgt = to_memory_target(dev);

	list_del(&tgt->list);
	kfree(tgt);
}

static void __init remove_memory_target(struct memory_target *tgt)
{
	if (tgt->has_perf_attributes)
		remove_performance_attributes(tgt);

	if (tgt->is_registered) {
		remove_node_for_kobj(pxm_to_node(tgt->ma->proximity_domain),
				&tgt->dev.kobj);
		device_unregister(&tgt->dev);
	} else
		release_memory_target(&tgt->dev);
}

static int __init register_memory_target(struct memory_target *tgt)
{
	int ret;

	if (!tgt->ma || !tgt->spa) {
		pr_err("HMEM: Incomplete memory target found\n");
		return -EINVAL;
	}

	hmem_subsys.dev_name = "mem_tgt";
	tgt->dev.bus = &hmem_subsys;
	tgt->dev.id = pxm_to_node(tgt->ma->proximity_domain);
	tgt->dev.release = release_memory_target;
	tgt->dev.groups = memory_target_attribute_groups;

	ret = device_register(&tgt->dev);
	if (ret < 0)
		return ret;

	tgt->is_registered = true;

	return link_node_for_kobj(pxm_to_node(tgt->ma->proximity_domain),
			&tgt->dev.kobj);
}

static int __init add_memory_target(struct acpi_srat_mem_affinity *ma)
{
	struct memory_target *tgt;

	if (pxm_to_node(ma->proximity_domain) == NUMA_NO_NODE) {
		pr_err("HMEM: No NUMA node for PXM %d\n", ma->proximity_domain);
		bad_hmem = true;
		return -EINVAL;
	}

	tgt = kzalloc(sizeof(*tgt), GFP_KERNEL);
	if (!tgt) {
		bad_hmem = true;
		return -ENOMEM;
	}

	tgt->ma = ma;

	list_add_tail(&tgt->list, &target_list);
	return 0;
}

/* ACPI parsing code, starting with the HMAT */
static int __init hmem_noop_parse(struct acpi_table_header *table)
{
	/* real work done by the hmat_parse_* and srat_parse_* routines */
	return 0;
}

static bool __init hmat_spa_matches_srat(struct acpi_hmat_address_range *spa,
		struct acpi_srat_mem_affinity *ma)
{
	if (spa->physical_address_base != ma->base_address ||
	    spa->physical_address_length != ma->length)
		return false;

	return true;
}

static void find_local_initiator(struct memory_target *tgt)
{
	struct memory_initiator *init;

	if (!(tgt->spa->flags & ACPI_HMAT_PROCESSOR_PD_VALID) ||
			pxm_to_node(tgt->spa->processor_PD) == NUMA_NO_NODE)
		return;

	list_for_each_entry(init, &initiator_list, list) {
		if (init->pxm == tgt->spa->processor_PD) {
			tgt->local_init = init;
			return;
		}
	}
}

/* ACPI HMAT parsing routines */
static int __init
hmat_parse_address_range(struct acpi_subtable_header *header,
		const unsigned long end)
{
	struct acpi_hmat_address_range *spa;
	struct memory_target *tgt;

	if (bad_hmem)
		return 0;

	spa = (struct acpi_hmat_address_range *)header;
	if (!spa) {
		pr_err("HMEM: NULL table entry\n");
		goto err;
	}

	if (spa->header.length != sizeof(*spa)) {
		pr_err("HMEM: Unexpected header length: %d\n",
				spa->header.length);
		goto err;
	}

	list_for_each_entry(tgt, &target_list, list) {
		if ((spa->flags & ACPI_HMAT_MEMORY_PD_VALID) &&
				spa->memory_PD == tgt->ma->proximity_domain) {
			if (!hmat_spa_matches_srat(spa, tgt->ma)) {
				pr_err("HMEM: SRAT and HMAT disagree on "
						"address range info\n");
				goto err;
			}
			tgt->spa = spa;
			find_local_initiator(tgt);
			return 0;
		}
	}

	return 0;
err:
	bad_hmem = true;
	return -EINVAL;
}

static int __init hmat_parse_locality(struct acpi_subtable_header *header,
		const unsigned long end)
{
	struct acpi_hmat_locality *hmat_loc;
	struct memory_locality *loc;

	if (bad_hmem)
		return 0;

	hmat_loc = (struct acpi_hmat_locality *)header;
	if (!hmat_loc) {
		pr_err("HMEM: NULL table entry\n");
		bad_hmem = true;
		return -EINVAL;
	}

	/* We don't report cached performance information in sysfs. */
	if (hmat_loc->flags == ACPI_HMAT_MEMORY ||
			hmat_loc->flags == ACPI_HMAT_LAST_LEVEL_CACHE) {
		loc = kzalloc(sizeof(*loc), GFP_KERNEL);
		if (!loc) {
			bad_hmem = true;
			return -ENOMEM;
		}

		loc->hmat_loc = hmat_loc;
		list_add_tail(&loc->list, &locality_list);
	}

	return 0;
}

static int __init hmat_parse_cache(struct acpi_subtable_header *header,
		const unsigned long end)
{
	struct acpi_hmat_cache *cache;
	struct memory_target *tgt;

	if (bad_hmem)
		return 0;

	cache = (struct acpi_hmat_cache *)header;
	if (!cache) {
		pr_err("HMEM: NULL table entry\n");
		goto err;
	}

	if (cache->header.length < sizeof(*cache)) {
		pr_err("HMEM: Unexpected header length: %d\n",
				cache->header.length);
		goto err;
	}

	list_for_each_entry(tgt, &target_list, list) {
		if (cache->memory_PD == tgt->ma->proximity_domain) {
			tgt->is_cached = true;
			return 0;
		}
	}

	pr_err("HMEM: Couldn't find cached target PXM %d\n", cache->memory_PD);
err:
	bad_hmem = true;
	return -EINVAL;
}

/*
 * SRAT parsing.  We use srat_disabled() and pxm_to_node() so we don't redo
 * any of the SRAT sanity checking done in drivers/acpi/numa.c.
 */
static int __init
srat_parse_processor_affinity(struct acpi_subtable_header *header,
		const unsigned long end)
{
	struct acpi_srat_cpu_affinity *cpu;
	struct memory_initiator *init;
	u32 pxm;

	if (bad_hmem)
		return 0;

	cpu = (struct acpi_srat_cpu_affinity *)header;
	if (!cpu) {
		pr_err("HMEM: NULL table entry\n");
		bad_hmem = true;
		return -EINVAL;
	}

	pxm = cpu->proximity_domain_lo;
	if (acpi_srat_revision >= 2)
		pxm |= *((unsigned int *)cpu->proximity_domain_hi) << 8;

	init = add_memory_initiator(pxm);
	if (IS_ERR(init))
		return PTR_ERR(init);

	init->cpu = cpu;
	return 0;
}

static int __init
srat_parse_x2apic_affinity(struct acpi_subtable_header *header,
		const unsigned long end)
{
	struct acpi_srat_x2apic_cpu_affinity *x2apic;
	struct memory_initiator *init;

	if (bad_hmem)
		return 0;

	x2apic = (struct acpi_srat_x2apic_cpu_affinity *)header;
	if (!x2apic) {
		pr_err("HMEM: NULL table entry\n");
		bad_hmem = true;
		return -EINVAL;
	}

	init = add_memory_initiator(x2apic->proximity_domain);
	if (IS_ERR(init))
		return PTR_ERR(init);

	init->x2apic = x2apic;
	return 0;
}

static int __init
srat_parse_gicc_affinity(struct acpi_subtable_header *header,
		const unsigned long end)
{
	struct acpi_srat_gicc_affinity *gicc;
	struct memory_initiator *init;

	if (bad_hmem)
		return 0;

	gicc = (struct acpi_srat_gicc_affinity *)header;
	if (!gicc) {
		pr_err("HMEM: NULL table entry\n");
		bad_hmem = true;
		return -EINVAL;
	}

	init = add_memory_initiator(gicc->proximity_domain);
	if (IS_ERR(init))
		return PTR_ERR(init);

	init->gicc = gicc;
	return 0;
}

static int __init
srat_parse_memory_affinity(struct acpi_subtable_header *header,
		const unsigned long end)
{
	struct acpi_srat_mem_affinity *ma;

	if (bad_hmem)
		return 0;

	ma = (struct acpi_srat_mem_affinity *)header;
	if (!ma) {
		pr_err("HMEM: NULL table entry\n");
		bad_hmem = true;
		return -EINVAL;
	}

	return add_memory_target(ma);
}

/*
 * Remove our sysfs entries, unregister our devices and free allocated memory.
 */
static void hmem_cleanup(void)
{
	struct memory_initiator *init, *init_iter;
	struct memory_locality *loc, *loc_iter;
	struct memory_target *tgt, *tgt_iter;

	list_for_each_entry_safe(tgt, tgt_iter, &target_list, list)
		remove_memory_target(tgt);

	list_for_each_entry_safe(init, init_iter, &initiator_list, list)
		remove_memory_initiator(init);

	list_for_each_entry_safe(loc, loc_iter, &locality_list, list) {
		list_del(&loc->list);
		kfree(loc);
	}
}

static int __init hmem_init(void)
{
	struct acpi_table_header *tbl;
	struct memory_initiator *init;
	struct memory_target *tgt;
	acpi_status status = AE_OK;
	int ret;

	if (srat_disabled())
		return 0;

	/*
	 * We take a permanent reference to both the HMAT and SRAT in ACPI
	 * memory so we can keep pointers to their subtables.  These tables
	 * already had references on them which would never be released, taken
	 * by acpi_sysfs_init(), so this shouldn't negatively impact anything.
	 */
	status = acpi_get_table(ACPI_SIG_SRAT, 0, &tbl);
	if (ACPI_FAILURE(status))
		return 0;

	status = acpi_get_table(ACPI_SIG_HMAT, 0, &tbl);
	if (ACPI_FAILURE(status))
		return 0;

	ret = subsys_system_register(&hmem_subsys, NULL);
	if (ret)
		return ret;

	if (!acpi_table_parse(ACPI_SIG_SRAT, hmem_noop_parse)) {
		struct acpi_subtable_proc srat_proc[4];

		memset(srat_proc, 0, sizeof(srat_proc));
		srat_proc[0].id = ACPI_SRAT_TYPE_CPU_AFFINITY;
		srat_proc[0].handler = srat_parse_processor_affinity;
		srat_proc[1].id = ACPI_SRAT_TYPE_X2APIC_CPU_AFFINITY;
		srat_proc[1].handler = srat_parse_x2apic_affinity;
		srat_proc[2].id = ACPI_SRAT_TYPE_GICC_AFFINITY;
		srat_proc[2].handler = srat_parse_gicc_affinity;
		srat_proc[3].id = ACPI_SRAT_TYPE_MEMORY_AFFINITY;
		srat_proc[3].handler = srat_parse_memory_affinity;

		acpi_table_parse_entries_array(ACPI_SIG_SRAT,
					sizeof(struct acpi_table_srat),
					srat_proc, ARRAY_SIZE(srat_proc), 0);
	}

	if (!acpi_table_parse(ACPI_SIG_HMAT, hmem_noop_parse)) {
		struct acpi_subtable_proc hmat_proc[3];

		memset(hmat_proc, 0, sizeof(hmat_proc));
		hmat_proc[0].id = ACPI_HMAT_TYPE_ADDRESS_RANGE;
		hmat_proc[0].handler = hmat_parse_address_range;
		hmat_proc[1].id = ACPI_HMAT_TYPE_CACHE;
		hmat_proc[1].handler = hmat_parse_cache;
		hmat_proc[2].id = ACPI_HMAT_TYPE_LOCALITY;
		hmat_proc[2].handler = hmat_parse_locality;

		acpi_table_parse_entries_array(ACPI_SIG_HMAT,
					sizeof(struct acpi_table_hmat),
					hmat_proc, ARRAY_SIZE(hmat_proc), 0);
	}

	if (bad_hmem) {
		ret = -EINVAL;
		goto err;
	}

	list_for_each_entry(init, &initiator_list, list) {
		ret = register_memory_initiator(init);
		if (ret)
			goto err;
	}

	list_for_each_entry(tgt, &target_list, list) {
		ret = register_memory_target(tgt);
		if (ret)
			goto err;

		ret = add_performance_attributes(tgt);
		if (ret)
			goto err;
	}

	return 0;
err:
	pr_err("HMEM: Error during initialization\n");
	hmem_cleanup();
	return ret;
}

static __exit void hmem_exit(void)
{
	hmem_cleanup();
}

module_init(hmem_init);
module_exit(hmem_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Intel Corporation");
