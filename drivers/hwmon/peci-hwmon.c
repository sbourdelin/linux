// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018 Intel Corporation

#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/peci.h>

#include "peci-hwmon.h"

#if IS_ENABLED(CONFIG_X86)
#include <asm/intel-family.h>
#else
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

enum cpu_gens {
	CPU_GEN_HSX = 0, /* Haswell Xeon */
	CPU_GEN_BRX,     /* Broadwell Xeon */
	CPU_GEN_SKX,     /* Skylake Xeon */
};

static const struct cpu_gen_info cpu_gen_info_table[] = {
	[CPU_GEN_HSX] = {
		.family        = 6, /* Family code */
		.model         = INTEL_FAM6_HASWELL_X,
		.core_max      = CORE_MAX_ON_HSX,
		.chan_rank_max = CHAN_RANK_MAX_ON_HSX,
		.dimm_idx_max  = DIMM_IDX_MAX_ON_HSX },
	[CPU_GEN_BRX] = {
		.family        = 6, /* Family code */
		.model         = INTEL_FAM6_BROADWELL_X,
		.core_max      = CORE_MAX_ON_BDX,
		.chan_rank_max = CHAN_RANK_MAX_ON_BDX,
		.dimm_idx_max  = DIMM_IDX_MAX_ON_BDX },
	[CPU_GEN_SKX] = {
		.family        = 6, /* Family code */
		.model         = INTEL_FAM6_SKYLAKE_X,
		.core_max      = CORE_MAX_ON_SKX,
		.chan_rank_max = CHAN_RANK_MAX_ON_SKX,
		.dimm_idx_max  = DIMM_IDX_MAX_ON_SKX },
};

int peci_hwmon_get_cpu_gen_info(struct peci_adapter *adapter, u8 addr,
				const struct cpu_gen_info **gen_info)
{
	u32 cpu_id;
	int i, rc;

	rc = peci_get_cpu_id(adapter, addr, &cpu_id);
	if (rc)
		return rc;

	for (i = 0; i < ARRAY_SIZE(cpu_gen_info_table); i++) {
		if (FIELD_GET(CPU_ID_FAMILY_MASK, cpu_id) +
			FIELD_GET(CPU_ID_EXT_FAMILY_MASK, cpu_id) ==
				cpu_gen_info_table[i].family &&
		    FIELD_GET(CPU_ID_MODEL_MASK, cpu_id) ==
			FIELD_GET(LOWER_NIBBLE_MASK,
				  cpu_gen_info_table[i].model) &&
		    FIELD_GET(CPU_ID_EXT_MODEL_MASK, cpu_id) ==
			FIELD_GET(UPPER_NIBBLE_MASK,
				  cpu_gen_info_table[i].model)) {
			break;
		}
	}

	if (i >= ARRAY_SIZE(cpu_gen_info_table))
		return -ENODEV;

	*gen_info = &cpu_gen_info_table[i];

	return 0;
}
EXPORT_SYMBOL_GPL(peci_hwmon_get_cpu_gen_info);

bool peci_hwmon_need_update(struct temp_data *temp)
{
	if (temp->valid &&
	    time_before(jiffies, temp->last_updated + UPDATE_INTERVAL))
		return false;

	return true;
}
EXPORT_SYMBOL_GPL(peci_hwmon_need_update);

void peci_hwmon_mark_updated(struct temp_data *temp)
{
	temp->valid = 1;
	temp->last_updated = jiffies;
}
EXPORT_SYMBOL_GPL(peci_hwmon_mark_updated);

int peci_hwmon_rd_pkg_cfg_cmd(struct peci_adapter *adapter, u8 addr,
			      u8 mbx_idx, u16 param, u8 *data)
{
	struct peci_rd_pkg_cfg_msg msg;
	int rc;

	msg.addr = addr;
	msg.index = mbx_idx;
	msg.param = param;
	msg.rx_len = 4;

	rc = peci_command(adapter, PECI_CMD_RD_PKG_CFG, &msg);
	if (!rc)
		memcpy(data, msg.pkg_config, 4);

	return rc;
}
EXPORT_SYMBOL_GPL(peci_hwmon_rd_pkg_cfg_cmd);

MODULE_AUTHOR("Jae Hyun Yoo <jae.hyun.yoo@linux.intel.com>");
MODULE_DESCRIPTION("PECI hwmon module");
MODULE_LICENSE("GPL v2");
