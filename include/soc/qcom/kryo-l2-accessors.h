/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __SOC_ARCH_QCOM_KRYO_L2_ACCESSORS_H
#define __SOC_ARCH_QCOM_KRYO_L2_ACCESSORS_H

void kryo_l2_set_indirect_reg(u64 reg, u64 val);
u64 kryo_l2_get_indirect_reg(u64 reg);

#endif
