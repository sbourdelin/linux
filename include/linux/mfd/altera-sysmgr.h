/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2018 Intel Corporation
 * Copyright (C) 2012 Freescale Semiconductor, Inc.
 * Copyright (C) 2012 Linaro Ltd.
 */

#ifndef __LINUX_MFD_ALTERA_SYSMGR_H__
#define __LINUX_MFD_ALTERA_SYSMGR_H__

#include <linux/err.h>
#include <linux/errno.h>

struct device_node;

#ifdef CONFIG_MFD_ALTERA_SYSMGR
struct regmap *altr_sysmgr_node_to_regmap(struct device_node *np);
struct regmap *altr_sysmgr_regmap_lookup_by_compatible(const char *s);
struct regmap *altr_sysmgr_regmap_lookup_by_pdevname(const char *s);
struct regmap *altr_sysmgr_regmap_lookup_by_phandle(struct device_node *np,
						    const char *property);

/*
 * Functions specified by ARM SMC Calling convention:
 *
 * FAST call executes atomic operations, returns when the requested operation
 * has completed.
 * STD call starts a operation which can be preempted by a non-secure
 * interrupt.
 *
 * a0..a7 is used as register names in the descriptions below, on arm32
 * that translates to r0..r7 and on arm64 to w0..w7.
 */

#define INTEL_SIP_SMC_STD_CALL_VAL(func_num) \
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_STD_CALL, ARM_SMCCC_SMC_64, \
	ARM_SMCCC_OWNER_SIP, (func_num))

#define INTEL_SIP_SMC_FAST_CALL_VAL(func_num) \
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64, \
	ARM_SMCCC_OWNER_SIP, (func_num))

#define INTEL_SIP_SMC_RETURN_UNKNOWN_FUNCTION		0xFFFFFFFF
#define INTEL_SIP_SMC_STATUS_OK				0x0
#define INTEL_SIP_SMC_REG_ERROR				0x5

/*
 * Request INTEL_SIP_SMC_REG_READ
 *
 * Read a protected register using SMCCC
 *
 * Call register usage:
 * a0: INTEL_SIP_SMC_REG_READ.
 * a1: register address.
 * a2-7: not used.
 *
 * Return status:
 * a0: INTEL_SIP_SMC_STATUS_OK, INTEL_SIP_SMC_REG_ERROR, or
 *     INTEL_SIP_SMC_RETURN_UNKNOWN_FUNCTION
 * a1: Value in the register
 * a2-3: not used.
 */
#define INTEL_SIP_SMC_FUNCID_REG_READ 7
#define INTEL_SIP_SMC_REG_READ \
	INTEL_SIP_SMC_FAST_CALL_VAL(INTEL_SIP_SMC_FUNCID_REG_READ)

/*
 * Request INTEL_SIP_SMC_REG_WRITE
 *
 * Write a protected register using SMCCC
 *
 * Call register usage:
 * a0: INTEL_SIP_SMC_REG_WRITE.
 * a1: register address
 * a2: value to program into register.
 * a3-7: not used.
 *
 * Return status:
 * a0: INTEL_SIP_SMC_STATUS_OK, INTEL_SIP_SMC_REG_ERROR, or
 *     INTEL_SIP_SMC_RETURN_UNKNOWN_FUNCTION
 * a1-3: not used.
 */
#define INTEL_SIP_SMC_FUNCID_REG_WRITE 8
#define INTEL_SIP_SMC_REG_WRITE \
	INTEL_SIP_SMC_FAST_CALL_VAL(INTEL_SIP_SMC_FUNCID_REG_WRITE)

#else
static inline struct regmap *altr_sysmgr_node_to_regmap(struct device_node *np)
{
	return ERR_PTR(-ENOTSUPP);
}

static inline struct regmap *
altr_sysmgr_regmap_lookup_by_compatible(const char *s)
{
	return ERR_PTR(-ENOTSUPP);
}

static inline struct regmap *
altr_sysmgr_regmap_lookup_by_pdevname(const char *s)
{
	return ERR_PTR(-ENOTSUPP);
}

static inline struct regmap *
altr_sysmgr_regmap_lookup_by_phandle(struct device_node *np,
				     const char *property)
{
	return ERR_PTR(-ENOTSUPP);
}
#endif

#endif /* __LINUX_MFD_ALTERA_SYSMGR_H__ */
