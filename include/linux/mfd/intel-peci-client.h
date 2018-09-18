/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018 Intel Corporation */

#ifndef __LINUX_MFD_PECI_CLIENT_H
#define __LINUX_MFD_PECI_CLIENT_H

#include <linux/peci.h>

#if IS_ENABLED(CONFIG_X86)
#include <asm/intel-family.h>
#else
/**
 * Architectures other than x86 cannot include the header file so define these
 * at here. These are needed for detecting type of client x86 CPUs behind a PECI
 * connection.
 */
#define INTEL_FAM6_HASWELL_X   0x3F
#define INTEL_FAM6_BROADWELL_X 0x4F
#define INTEL_FAM6_SKYLAKE_X   0x55
#endif

#define LOWER_NIBBLE_MASK      GENMASK(3, 0)
#define UPPER_NIBBLE_MASK      GENMASK(7, 4)

#define CPU_ID_MODEL_MASK      GENMASK(7, 4)
#define CPU_ID_FAMILY_MASK     GENMASK(11, 8)
#define CPU_ID_EXT_MODEL_MASK  GENMASK(19, 16)
#define CPU_ID_EXT_FAMILY_MASK GENMASK(27, 20)

#define CORE_MAX_ON_HSX        18 /* Max number of cores on Haswell */
#define CHAN_RANK_MAX_ON_HSX   8  /* Max number of channel ranks on Haswell */
#define DIMM_IDX_MAX_ON_HSX    3  /* Max DIMM index per channel on Haswell */

#define CORE_MAX_ON_BDX        24 /* Max number of cores on Broadwell */
#define CHAN_RANK_MAX_ON_BDX   4  /* Max number of channel ranks on Broadwell */
#define DIMM_IDX_MAX_ON_BDX    3  /* Max DIMM index per channel on Broadwell */

#define CORE_MAX_ON_SKX        28 /* Max number of cores on Skylake */
#define CHAN_RANK_MAX_ON_SKX   6  /* Max number of channel ranks on Skylake */
#define DIMM_IDX_MAX_ON_SKX    2  /* Max DIMM index per channel on Skylake */

#define CORE_NUMS_MAX          CORE_MAX_ON_SKX
#define CHAN_RANK_MAX          CHAN_RANK_MAX_ON_HSX
#define DIMM_IDX_MAX           DIMM_IDX_MAX_ON_HSX
#define DIMM_NUMS_MAX          (CHAN_RANK_MAX * DIMM_IDX_MAX)

#define TEMP_TYPE_PECI         6 /* Sensor type 6: Intel PECI */

#define UPDATE_INTERVAL        HZ

struct temp_data {
	uint  valid;
	s32   value;
	ulong last_updated;
};

struct cpu_gen_info {
	u16  family;
	u8   model;
	uint core_max;
	uint chan_rank_max;
	uint dimm_idx_max;
};

struct peci_mfd {
	struct peci_client *client;
	struct device *dev;
	struct peci_adapter *adapter;
	char name[PECI_NAME_SIZE];
	u8 addr;
	uint cpu_no;
	const struct cpu_gen_info *gen_info;
};

bool peci_temp_need_update(struct temp_data *temp);
void peci_temp_mark_updated(struct temp_data *temp);
int  peci_client_command(struct peci_mfd *mfd, enum peci_cmd cmd, void *vmsg);
int  peci_client_rd_pkg_cfg_cmd(struct peci_mfd *mfd, u8 mbx_idx,
				u16 param, u8 *data);

#endif /* __LINUX_MFD_PECI_CLIENT_H */
