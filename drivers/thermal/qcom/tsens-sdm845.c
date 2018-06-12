// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, Linaro Limited
 */

#include "tsens.h"

static const struct tsens_ops ops_sdm845 = {
	.init		= init_common,
	.get_temp	= get_temp_tsens_v2,
};

const struct tsens_data data_sdm845 = {
	.ops		= &ops_sdm845,
};
