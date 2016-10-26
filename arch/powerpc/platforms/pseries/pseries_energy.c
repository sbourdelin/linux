/*
 * POWER platform energy management driver
 * Copyright (C) 2010 IBM Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This pseries platform device driver provides access to
 * platform energy management capabilities.
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/seq_file.h>
#include <linux/device.h>
#include <linux/cpu.h>
#include <linux/of.h>
#include <asm/cputhreads.h>
#include <asm/page.h>
#include <asm/hvcall.h>
#include <asm/firmware.h>


#define MODULE_VERS "1.0"
#define MODULE_NAME "pseries_energy"

/* Driver flags */

static int sysfs_entries;

/* Helper routines */

/* Helper Routines to convert between drc_index to cpu numbers */

void read_one_drc_info(int **info, char **dtype, char **dname,
			unsigned long int *drc_index_start_p,
			unsigned long int *num_sequential_elems_p,
			unsigned long int *sequential_inc_p,
			unsigned long int *last_drc_index_p)
{
	char *drc_type, *drc_name_prefix, *pc;
	u32 drc_index_start, num_sequential_elems;
	u32 sequential_inc, last_drc_index;

	drc_index_start = num_sequential_elems = 0;
	sequential_inc = last_drc_index = 0;

	/* Get drc-type:encode-string */
	pc = (char *)info;
	drc_type = pc;
	pc += (strlen(drc_type) + 1);

	/* Get drc-name-prefix:encode-string */
	drc_name_prefix = (char *)pc;
	pc += (strlen(drc_name_prefix) + 1);

	/* Get drc-index-start:encode-int */
	memcpy(&drc_index_start, pc, 4);
	drc_index_start = be32_to_cpu(drc_index_start);
	pc += 4;

	/* Get/skip drc-name-suffix-start:encode-int */
	pc += 4;

	/* Get number-sequential-elements:encode-int */
	memcpy(&num_sequential_elems, pc, 4);
	num_sequential_elems = be32_to_cpu(num_sequential_elems);
	pc += 4;

	/* Get sequential-increment:encode-int */
	memcpy(&sequential_inc, pc, 4);
	sequential_inc = be32_to_cpu(sequential_inc);
	pc += 4;

	/* Get/skip drc-power-domain:encode-int */
	pc += 4;

	/* Should now know end of current entry */
	last_drc_index = drc_index_start +
			((num_sequential_elems-1)*sequential_inc);

	(*info) = (int *)pc;

	if (dtype)
		*dtype = drc_type;
	if (dname)
		*dname = drc_name_prefix;
	if (drc_index_start_p)
		*drc_index_start_p = drc_index_start;
	if (num_sequential_elems_p)
		*num_sequential_elems_p = num_sequential_elems;
	if (sequential_inc_p)
		*sequential_inc_p = sequential_inc;
	if (last_drc_index_p)
		*last_drc_index_p = last_drc_index;
}
EXPORT_SYMBOL(read_one_drc_info);

static u32 cpu_to_drc_index(int cpu)
{
	struct device_node *dn = NULL;
	int i;
	int rc = 1;
	u32 ret = 0;

	dn = of_find_node_by_path("/cpus");
	if (dn == NULL)
		goto err;

	/* Convert logical cpu number to core number */
	i = cpu_core_index_of_thread(cpu);

	if (firmware_has_feature(FW_FEATURE_DRC_INFO)) {
		int *info = (int *)4;
		unsigned long int num_set_entries, j, check_val = i;
		unsigned long int drc_index_start = 0;
		unsigned long int last_drc_index = 0;
		unsigned long int num_sequential_elems = 0;
		unsigned long int sequential_inc = 0;
		char *dtype;
		char *dname;

		info = (int *)of_get_property(dn, "ibm,drc-info", NULL);
		if (info == NULL)
			goto err_of_node_put;

		num_set_entries = be32_to_cpu(*info++);

		for (j = 0; j < num_set_entries; j++) {

			read_one_drc_info(&info, &dtype, &dname,
					&drc_index_start,
					&num_sequential_elems,
					&sequential_inc, &last_drc_index);
			if (strcmp(dtype, "CPU"))
				goto err;

			if (check_val < last_drc_index)
				break;

			WARN_ON(((check_val-drc_index_start)%
					sequential_inc) != 0);
		}
		WARN_ON((num_sequential_elems == 0) | (sequential_inc == 0));

		ret = last_drc_index + (check_val*sequential_inc);
	} else {
		const int *indexes;

		indexes = of_get_property(dn, "ibm,drc-indexes", NULL);
		if (indexes == NULL)
			goto err_of_node_put;

		/*
		 * The first element indexes[0] is the number of drc_indexes
		 * returned in the list.  Hence i+1 will get the drc_index
		 * corresponding to core number i.
		 */
		WARN_ON(i > indexes[0]);
		ret = indexes[i + 1];
	}

	rc = 0;

err_of_node_put:
	of_node_put(dn);
err:
	if (rc)
		printk(KERN_WARNING "cpu_to_drc_index(%d) failed", cpu);
	return ret;
}

static int drc_index_to_cpu(u32 drc_index)
{
	struct device_node *dn = NULL;
	const int *indexes;
	int i, cpu = 0;
	int rc = 1;

	dn = of_find_node_by_path("/cpus");
	if (dn == NULL)
		goto err;

	if (firmware_has_feature(FW_FEATURE_DRC_INFO)) {
		int *info = (int *)dn;
		unsigned long int num_set_entries, j, ret;
		unsigned long int drc_index_start = 0;
		unsigned long int last_drc_index = 0;
		unsigned long int num_sequential_elems = 0;
		unsigned long int sequential_inc = 0;
		char *dtype, *dname;

		info = (int *)of_get_property(dn, "ibm,drc-info", NULL);
		if (info == NULL)
			goto err_of_node_put;

		num_set_entries = be32_to_cpu(*info++);

		for (j = 0; j < num_set_entries; j++) {
			read_one_drc_info(&info, &dtype, &dname,
					&drc_index_start,
					&num_sequential_elems,
					&sequential_inc, &last_drc_index);
			if (strcmp(dtype, "CPU"))
				goto err;

			WARN_ON(drc_index < drc_index_start);
			if (drc_index > last_drc_index)
				continue;

			WARN_ON(((drc_index-drc_index_start)%
					sequential_inc) != 0);

			ret = ((drc_index-drc_index_start)/sequential_inc);
			break;
		}
	} else {
		indexes = of_get_property(dn, "ibm,drc-indexes", NULL);
		if (indexes == NULL)
			goto err_of_node_put;
		/*
		 * First element in the array is the number of drc_indexes
		 * returned.  Search through the list to find the matching
		 * drc_index and get the core number
		 */
		for (i = 0; i < indexes[0]; i++) {
			if (indexes[i + 1] == drc_index)
				break;
		}
		/* Convert core number to logical cpu number */
		cpu = cpu_first_thread_of_core(i);
		rc = 0;
	}

err_of_node_put:
	of_node_put(dn);
err:
	if (rc)
		printk(KERN_WARNING "drc_index_to_cpu(%d) failed", drc_index);
	return cpu;
}

/*
 * pseries hypervisor call H_BEST_ENERGY provides hints to OS on
 * preferred logical cpus to activate or deactivate for optimized
 * energy consumption.
 */

#define FLAGS_MODE1	0x004E200000080E01UL
#define FLAGS_MODE2	0x004E200000080401UL
#define FLAGS_ACTIVATE  0x100

static ssize_t get_best_energy_list(char *page, int activate)
{
	int rc, cnt, i, cpu;
	unsigned long retbuf[PLPAR_HCALL9_BUFSIZE];
	unsigned long flags = 0;
	u32 *buf_page;
	char *s = page;

	buf_page = (u32 *) get_zeroed_page(GFP_KERNEL);
	if (!buf_page)
		return -ENOMEM;

	flags = FLAGS_MODE1;
	if (activate)
		flags |= FLAGS_ACTIVATE;

	rc = plpar_hcall9(H_BEST_ENERGY, retbuf, flags, 0, __pa(buf_page),
				0, 0, 0, 0, 0, 0);
	if (rc != H_SUCCESS) {
		free_page((unsigned long) buf_page);
		return -EINVAL;
	}

	cnt = retbuf[0];
	for (i = 0; i < cnt; i++) {
		cpu = drc_index_to_cpu(buf_page[2*i+1]);
		if ((cpu_online(cpu) && !activate) ||
		    (!cpu_online(cpu) && activate))
			s += sprintf(s, "%d,", cpu);
	}
	if (s > page) { /* Something to show */
		s--; /* Suppress last comma */
		s += sprintf(s, "\n");
	}

	free_page((unsigned long) buf_page);
	return s-page;
}

static ssize_t get_best_energy_data(struct device *dev,
					char *page, int activate)
{
	int rc;
	unsigned long retbuf[PLPAR_HCALL9_BUFSIZE];
	unsigned long flags = 0;

	flags = FLAGS_MODE2;
	if (activate)
		flags |= FLAGS_ACTIVATE;

	rc = plpar_hcall9(H_BEST_ENERGY, retbuf, flags,
				cpu_to_drc_index(dev->id),
				0, 0, 0, 0, 0, 0, 0);

	if (rc != H_SUCCESS)
		return -EINVAL;

	return sprintf(page, "%lu\n", retbuf[1] >> 32);
}

/* Wrapper functions */

static ssize_t cpu_activate_hint_list_show(struct device *dev,
			struct device_attribute *attr, char *page)
{
	return get_best_energy_list(page, 1);
}

static ssize_t cpu_deactivate_hint_list_show(struct device *dev,
			struct device_attribute *attr, char *page)
{
	return get_best_energy_list(page, 0);
}

static ssize_t percpu_activate_hint_show(struct device *dev,
			struct device_attribute *attr, char *page)
{
	return get_best_energy_data(dev, page, 1);
}

static ssize_t percpu_deactivate_hint_show(struct device *dev,
			struct device_attribute *attr, char *page)
{
	return get_best_energy_data(dev, page, 0);
}

/*
 * Create sysfs interface:
 * /sys/devices/system/cpu/pseries_activate_hint_list
 * /sys/devices/system/cpu/pseries_deactivate_hint_list
 *	Comma separated list of cpus to activate or deactivate
 * /sys/devices/system/cpu/cpuN/pseries_activate_hint
 * /sys/devices/system/cpu/cpuN/pseries_deactivate_hint
 *	Per-cpu value of the hint
 */

static struct device_attribute attr_cpu_activate_hint_list =
		__ATTR(pseries_activate_hint_list, 0444,
		cpu_activate_hint_list_show, NULL);

static struct device_attribute attr_cpu_deactivate_hint_list =
		__ATTR(pseries_deactivate_hint_list, 0444,
		cpu_deactivate_hint_list_show, NULL);

static struct device_attribute attr_percpu_activate_hint =
		__ATTR(pseries_activate_hint, 0444,
		percpu_activate_hint_show, NULL);

static struct device_attribute attr_percpu_deactivate_hint =
		__ATTR(pseries_deactivate_hint, 0444,
		percpu_deactivate_hint_show, NULL);

static int __init pseries_energy_init(void)
{
	int cpu, err;
	struct device *cpu_dev;

	if (!firmware_has_feature(FW_FEATURE_BEST_ENERGY)) {
		printk(KERN_INFO "Hypercall H_BEST_ENERGY not supported\n");
		return 0;
	}
	/* Create the sysfs files */
	err = device_create_file(cpu_subsys.dev_root,
				&attr_cpu_activate_hint_list);
	if (!err)
		err = device_create_file(cpu_subsys.dev_root,
				&attr_cpu_deactivate_hint_list);

	if (err)
		return err;
	for_each_possible_cpu(cpu) {
		cpu_dev = get_cpu_device(cpu);
		err = device_create_file(cpu_dev,
				&attr_percpu_activate_hint);
		if (err)
			break;
		err = device_create_file(cpu_dev,
				&attr_percpu_deactivate_hint);
		if (err)
			break;
	}

	if (err)
		return err;

	sysfs_entries = 1; /* Removed entries on cleanup */
	return 0;

}

static void __exit pseries_energy_cleanup(void)
{
	int cpu;
	struct device *cpu_dev;

	if (!sysfs_entries)
		return;

	/* Remove the sysfs files */
	device_remove_file(cpu_subsys.dev_root, &attr_cpu_activate_hint_list);
	device_remove_file(cpu_subsys.dev_root, &attr_cpu_deactivate_hint_list);

	for_each_possible_cpu(cpu) {
		cpu_dev = get_cpu_device(cpu);
		sysfs_remove_file(&cpu_dev->kobj,
				&attr_percpu_activate_hint.attr);
		sysfs_remove_file(&cpu_dev->kobj,
				&attr_percpu_deactivate_hint.attr);
	}
}

module_init(pseries_energy_init);
module_exit(pseries_energy_cleanup);
MODULE_DESCRIPTION("Driver for pSeries platform energy management");
MODULE_AUTHOR("Vaidyanathan Srinivasan");
MODULE_LICENSE("GPL");
