// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 * Copyright (c) 2018, Linaro Limited
 */

#include "tsens.h"

static const struct tsens_ops ops_8996 = {
	.init		= init_common,
	.get_temp	= get_temp_tsens_v2,
};

const struct tsens_data data_8996 = {
	.ops		= &ops_8996,
};
