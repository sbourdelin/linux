/*
 * Maxim Integrated MAX2175 RF to Bits tuner driver
 *
 * This driver & most of the hard coded values are based on the reference
 * application delivered by Maxim for this chip.
 *
 * Copyright (C) 2016 Maxim Integrated Products
 * Copyright (C) 2016 Renesas Electronics Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef __MAX2175_H__
#define __MAX2175_H__

#include <linux/types.h>

enum max2175_region {
	MAX2175_REGION_EU = 0,	/* Europe */
	MAX2175_REGION_NA,	/* North America */
};

#define MAX2175_EU_XTAL_FREQ	(36864000)	/* In Hz */
#define MAX2175_NA_XTAL_FREQ	(40186125)	/* In Hz */

enum max2175_band {
	MAX2175_BAND_AM = 0,
	MAX2175_BAND_FM,
	MAX2175_BAND_VHF,
	MAX2175_BAND_L,
};

/* NOTE: Any addition/deletion in the below enum should be reflected in
 * V4L2_CID_MAX2175_RX_MODE ctrl strings
 */
enum max2175_modetag {
	/* EU modes */
	MAX2175_DAB_1_2 = 0,

	/* Other possible modes to add in future
	 * MAX2175_DAB_1_0,
	 * MAX2175_DAB_1_3,
	 * MAX2175_EU_FM_2_2,
	 * MAX2175_EU_FM_1_0,
	 * MAX2175_EU_FMHD_4_0,
	 * MAX2175_EU_AM_1_0,
	 * MAX2175_EU_AM_2_2,
	 */

	/* NA modes */
	MAX2175_NA_FM_1_0 = 0,

	/* Other possible modes to add in future
	 * MAX2175_NA_FM_1_2,
	 * MAX2175_NA_FMHD_1_0,
	 * MAX2175_NA_FMHD_1_2,
	 * MAX2175_NA_AM_1_0,
	 * MAX2175_NA_AM_1_2,
	 */
};

/* Supported I2S modes */
enum {
	MAX2175_I2S_MODE0 = 0,
	MAX2175_I2S_MODE1,
	MAX2175_I2S_MODE2,
	MAX2175_I2S_MODE3,
	MAX2175_I2S_MODE4,
};

/* Coefficient table groups */
enum {
	MAX2175_CH_MSEL = 0,
	MAX2175_EQ_MSEL,
	MAX2175_AA_MSEL,
};

/* HSLS LO injection polarity */
enum {
	MAX2175_LO_BELOW_DESIRED = 0,
	MAX2175_LO_ABOVE_DESIRED,
};

/* Channel FSM modes */
enum max2175_csm_mode {
	MAX2175_CSM_MODE_LOAD_TO_BUFFER = 0,
	MAX2175_CSM_MODE_PRESET_TUNE,
	MAX2175_CSM_MODE_SEARCH,
	MAX2175_CSM_MODE_AF_UPDATE,
	MAX2175_CSM_MODE_JUMP_FAST_TUNE,
	MAX2175_CSM_MODE_CHECK,
	MAX2175_CSM_MODE_LOAD_AND_SWAP,
	MAX2175_CSM_MODE_END,
	MAX2175_CSM_MODE_BUFFER_PLUS_PRESET_TUNE,
	MAX2175_CSM_MODE_BUFFER_PLUS_SEARCH,
	MAX2175_CSM_MODE_BUFFER_PLUS_AF_UPDATE,
	MAX2175_CSM_MODE_BUFFER_PLUS_JUMP_FAST_TUNE,
	MAX2175_CSM_MODE_BUFFER_PLUS_CHECK,
	MAX2175_CSM_MODE_BUFFER_PLUS_LOAD_AND_SWAP,
	MAX2175_CSM_MODE_NO_ACTION
};

/* Rx mode */
struct max2175_rxmode {
	enum max2175_band band;		/* Associated band */
	u32 freq;			/* Default freq in Hz */
	u8 i2s_word_size;		/* Bit value */
	u8 i2s_modes[4];		/* Supported modes */
};

/* Register map */
struct max2175_regmap {
	u8 idx;				/* Register index */
	u8 val;				/* Register value */
};

#endif /* __MAX2175_H__ */
