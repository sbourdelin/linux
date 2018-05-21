/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018 Intel Corporation */

#ifndef __PECI_HWMON_H
#define __PECI_HWMON_H

#include <linux/peci.h>

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

int  peci_hwmon_get_cpu_gen_info(struct peci_adapter *adapter, u8 addr,
				 const struct cpu_gen_info **gen_info);
bool peci_hwmon_need_update(struct temp_data *temp);
void peci_hwmon_mark_updated(struct temp_data *temp);
int  peci_hwmon_rd_pkg_cfg_cmd(struct peci_adapter *adapter, u8 addr,
			       u8 mbx_idx, u16 param, u8 *data);

#endif /* __PECI_HWMON_H */
