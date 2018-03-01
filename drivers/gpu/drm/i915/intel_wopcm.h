/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2017-2018 Intel Corporation
 */

#ifndef _INTEL_WOPCM_H_
#define _INTEL_WOPCM_H_

#include <linux/types.h>

/**
 * struct intel_wopcm - overall WOPCM info and WOPCM regions.
 * @size: size of overall WOPCM.
 * @guc: GuC WOPCM Region info.
 */
struct intel_wopcm {
	u32 size;
	struct {
		/**
		 * @base: GuC WOPCM base which is offset from WOPCM base.
		 */
		u32 base;
		/**
		 * @size: size of the GuC WOPCM region.
		 */
		u32 size;
	} guc;
};

void intel_wopcm_init_early(struct intel_wopcm *wopcm);
int intel_wopcm_init(struct intel_wopcm *wopcm);

#endif
