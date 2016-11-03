#ifndef PPC_POWERNV_IMA_PMU_DEF_H
#define PPC_POWERNV_IMA_PMU_DEF_H

/*
 * Nest Performance Monitor counter support.
 *
 * Copyright (C) 2016 Madhavan Srinivasan, IBM Corporation.
 *           (C) 2016 Hemant K Shaw, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <linux/perf_event.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/io.h>
#include <asm/opal.h>

#define IMA_MAX_CHIPS			32
#define IMA_MAX_PMUS			32
#define IMA_MAX_PMU_NAME_LEN		256

#define NEST_IMA_ENGINE_START		1
#define NEST_IMA_ENGINE_STOP		0
#define NEST_MAX_PAGES			16

#define NEST_IMA_PRODUCTION_MODE	1

#define IMA_DTB_COMPAT		"ibm,opal-in-memory-counters"
#define IMA_DTB_NEST_COMPAT	"ibm,ima-counters-chip"

/*
 * Structure to hold per chip specific memory address
 * information for nest pmus. Nest Counter data are exported
 * in per-chip reserved memory region by the PORE Engine.
 */
struct perchip_nest_info {
	u32 chip_id;
	u64 pbase;
	u64 vbase[NEST_MAX_PAGES];
	u64 size;
};

/*
 * Place holder for nest pmu events and values.
 */
struct ima_events {
	char *ev_name;
	char *ev_value;
};

/*
 * Device tree parser code detects IMA pmu support and
 * registers new IMA pmus. This structure will
 * hold the pmu functions and attrs for each ima pmu and
 * will be referenced at the time of pmu registration.
 */
struct ima_pmu {
	struct pmu pmu;
	int domain;
	const struct attribute_group *attr_groups[4];
};

/*
 * Domains for IMA PMUs
 */
#define IMA_DOMAIN_NEST		1

#define UNKNOWN_DOMAIN		-1

#endif /* PPC_POWERNV_IMA_PMU_DEF_H */
