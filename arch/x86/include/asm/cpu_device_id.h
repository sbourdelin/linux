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
 * Match specific microcodes
 *
 * vendor/family/model/stepping must be all set.
 * min_ucode is optional and can be 0.
 */

struct x86_ucode_id {
	u8 vendor;
	u8 family;
	u16 model;
	u16 stepping;
	u32 min_ucode;
};

#define INTEL_MIN_UCODE(mod, step, rev) {			\
	.vendor = X86_VENDOR_INTEL,				\
	.family = 6,						\
	.model = mod,						\
	.stepping = step,					\
	.min_ucode = rev,					\
}

extern const struct x86_ucode_id *
x86_match_ucode(const struct x86_ucode_id *match);

#endif
