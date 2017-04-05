#ifndef __ASM_POWERPC_CPUFEATURES_H
#define __ASM_POWERPC_CPUFEATURES_H

/*
 *  Copyright 2017, IBM Corporation
 *  cpufeatures is the new way to discover CPU features with /cpus/features
 *  devicetree. This supersedes PVR based discovery ("cputable"), and other
 *  devic tree feature advertisement.
 */

#include <linux/types.h>
#include <asm/asm-compat.h>
#include <asm/feature-fixups.h>
#include <uapi/asm/cputable.h>

extern void cpufeatures_setup_cpu(void);

/* Types for device tree parsing */
#define USABLE_PR               (1U << 0)
#define USABLE_OS               (1U << 1)
#define USABLE_HV               (1U << 2)

#define HV_SUPPORT_NONE         0
#define HV_SUPPORT_CUSTOM       1
#define HV_SUPPORT_HFSCR        2

#define OS_SUPPORT_NONE         0
#define OS_SUPPORT_CUSTOM       1
#define OS_SUPPORT_FSCR         2

#define ISA_BASE                0
#define ISA_V207                2070
#define ISA_V3                  3000

struct dt_cpu_feature {
	const char *name;
	uint32_t isa;
	uint32_t usable_mask;
	uint32_t hv_support;
	uint32_t os_support;
	uint32_t hfscr_bit_nr;
	uint32_t fscr_bit_nr;
	uint32_t hwcap_bit_nr;
	/* fdt parsing */
	unsigned long node;
	uint32_t phandle;
	int enabled;
	int disabled;
};

extern void cpufeatures_setup_start(u32 isa);
extern int cpufeatures_process_feature(struct dt_cpu_feature *f);
extern void cpufeatures_setup_finished(void);

/* kernel/prom.c */
extern int early_init_devtree_check_cpu_features_exists(void);

#endif
