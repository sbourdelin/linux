#ifndef PPC_POWERNV_IMC_PMU_DEF_H
#define PPC_POWERNV_IMC_PMU_DEF_H

/*
 * IMC Nest Performance Monitor counter support.
 *
 * Copyright (C) 2017 Madhavan Srinivasan, IBM Corporation.
 *           (C) 2017 Anju T Sudhakar, IBM Corporation.
 *           (C) 2017 Hemant K Shaw, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or later version.
 */

#include <linux/perf_event.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/io.h>
#include <asm/opal.h>

/*
 * For static allocation of some of the structures.
 */
#define IMC_MAX_PMUS			32

/*
 * This macro is used for memory buffer allocation of
 * event names and event string
 */
#define IMC_MAX_NAME_VAL_LEN		96

/*
 * Currently Microcode supports a max of 256KB of counter memory
 * in the reserved memory region. Max pages to mmap (considering 4K PAGESIZE).
 */
#define IMC_MAX_PAGES			64

/*
 *Compatbility macros for IMC devices
 */
#define IMC_DTB_COMPAT			"ibm,opal-in-memory-counters"
#define IMC_DTB_UNIT_COMPAT		"ibm,imc-counters"

/*
 * Structure to hold memory address information for imc units.
 */
struct imc_mem_info {
	u32 id;
	u64 *vbase[IMC_MAX_PAGES];
};

/*
 * Place holder for nest pmu events and values.
 */
struct imc_events {
	char *ev_name;
	char *ev_value;
};

#define IMC_FORMAT_ATTR		0
#define IMC_CPUMASK_ATTR	1
#define IMC_EVENT_ATTR		2
#define IMC_NULL_ATTR		3

/*
 * Device tree parser code detects IMC pmu support and
 * registers new IMC pmus. This structure will hold the
 * pmu functions, events, counter memory information
 * and attrs for each imc pmu and will be referenced at
 * the time of pmu registration.
 */
struct imc_pmu {
	struct pmu pmu;
	int domain;
	/*
	 * flag to notify whether the memory is mmaped
	 * or allocated by kernel.
	 */
	int imc_counter_mmaped;
	struct imc_mem_info *mem_info;
	struct imc_events *events;
	u32 counter_mem_size;
	/*
	 * Attribute groups for the PMU. Slot 0 used for
	 * format attribute, slot 1 used for cpusmask attribute,
	 * slot 2 used for event attribute. Slot 3 keep as
	 * NULL.
	 */
	const struct attribute_group *attr_groups[4];
};

/*
 * Domains for IMC PMUs
 */
#define IMC_DOMAIN_NEST		1
#define IMC_DOMAIN_CORE		2

extern struct imc_pmu *per_nest_pmu_arr[IMC_MAX_PMUS];
extern struct imc_pmu *core_imc_pmu;
extern int imc_control(unsigned long type, bool operation);
extern int __init init_imc_pmu(struct imc_events *events, int idx, struct imc_pmu *pmu_ptr);
#endif /* PPC_POWERNV_IMC_PMU_DEF_H */
