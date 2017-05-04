#ifndef __ASM_POWERPC_CPUFEATURES_H
#define __ASM_POWERPC_CPUFEATURES_H

/*
 *  Copyright 2017, IBM Corporation
 *  cpufeatures is the new way to discover CPU features with /cpus/features
 *  devicetree. This supersedes PVR based discovery ("cputable"), and older
 *  device tree feature advertisement.
 */

#include <linux/types.h>
#include <asm/asm-compat.h>
#include <asm/feature-fixups.h>
#include <uapi/asm/cputable.h>

extern void cpufeatures_setup_cpu(void);

/* Device-tree visible constants follow */
#define ISA_V2_07B      2070
#define ISA_V3_0B       3000

#define USABLE_PR               (1U << 0)
#define USABLE_OS               (1U << 1)
#define USABLE_HV               (1U << 2)

#define HV_SUPPORT_HFSCR        (1U << 0)
#define OS_SUPPORT_FSCR         (1U << 0)

/* For parsing, we define all bits set as "NONE" case */
#define HV_SUPPORT_NONE		0xffffffffU
#define OS_SUPPORT_NONE		0xffffffffU

struct dt_cpu_feature {
	const char *name;
	uint32_t isa;
	uint32_t usable_privilege;
	uint32_t hv_support;
	uint32_t os_support;
	uint32_t hfscr_bit_nr;
	uint32_t fscr_bit_nr;
	uint32_t hwcap_bit_nr;
	/* fdt parsing */
	unsigned long node;
	int enabled;
	int disabled;
};

extern void cpufeatures_setup_start(u32 isa);
extern int cpufeatures_process_feature(struct dt_cpu_feature *f);
extern void cpufeatures_setup_finished(void);

/* kernel/prom.c */
extern int early_init_devtree_check_cpu_features_exists(void);

#endif
