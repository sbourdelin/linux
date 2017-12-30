/*
 * Copyright 2017 NXP
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef __IMX_SIP_SMC_H_
#define __IMX_SIP_SMC_H_

#include <linux/arm-smccc.h>

#define IMX_SIP_SMC_VAL(func) ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
						 ARM_SMCCC_SMC_32, \
						 ARM_SMCCC_OWNER_SIP, \
						 (func))

#define IMX_L2C310		0x1

#define IMX_SIP_SMC_L2C310	IMX_SIP_SMC_VAL(IMX_L2C310)

#endif
