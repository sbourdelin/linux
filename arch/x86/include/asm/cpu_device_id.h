/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CPU_DEVICE_ID
#define _CPU_DEVICE_ID 1

/*
 * Declare drivers belonging to specific x86 CPUs
 * Similar in spirit to pci_device_id and related PCI functions
 */

#include <linux/mod_devicetable.h>

extern const struct x86_cpu_id *x86_match_cpu(const struct x86_cpu_id *match);

/*
 * Match specific microcode revisions.
 *
 * vendor/family/model/stepping must be all set.
 *
 * only checks against the boot cpu.  When mixed-stepping configs are
 * valid for a CPU model, add a quirk for every valid stepping and
 * do the fine-tuning in the quirk handler.
 */

struct x86_cpu_check {
	u8	vendor;
	u8	family;
	u8	model;
	u8	stepping;
	u32	microcode_rev;
};

#define INTEL_CHECK_UCODE(mod, step, rev) {			\
	.vendor = X86_VENDOR_INTEL,				\
	.family = 6,						\
	.model = mod,						\
	.stepping = step,					\
	.microcode_rev = rev,					\
}

extern bool x86_cpu_has_min_microcode_rev(const struct x86_cpu_check *table);

#endif
