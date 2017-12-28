/*
 * Copyright 2017 NXP
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
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
