/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SOC_MEDIATEK_SCPSYS_EXT_H
#define __SOC_MEDIATEK_SCPSYS_EXT_H

#include <linux/platform_device.h>

#define CMD_ENABLE	1
#define CMD_DISABLE	0

#define MAX_STEP_NUM	4

/**
 * struct bus_mask - set mask and corresponding operation for bus protect
 * @regs: The register set of bus register control, including set/clr/sta.
 * @mask: The mask set for bus protect.
 * @flag: The flag to idetify which operation we take for bus protect.
 */
struct bus_mask {
	struct ext_reg_ctrl *regs;
	u32 mask;
	const struct bus_mask_ops *ops;
};

/**
 * struct scpsys_ext_attr - extended attribute for bus protect and further
 *                                           operand.
 *
 * @scpd_n: The name present the scpsys domain where the clks belongs to.
 * @mask: The mask set for bus protect.
 * @bus_ops: The operation we take for bus protect.
 * @cg_ops: The operation we take for cg on/off.
 * @attr_list: The list node linked to ext_attr_map_list.
 */
struct scpsys_ext_attr {
	const char *scpd_n;
	struct bus_mask mask[MAX_STEP_NUM];
	const char *parent_n;
	const struct bus_ext_ops *bus_ops;
	const struct bus_ext_ops *cg_ops;

	struct list_head attr_list;
};

struct scpsys_ext_data {
	struct scpsys_ext_attr *attr;
	u8 num_attr;
	struct scpsys_ext_attr * (*get_attr)(const char *scpd_n);
};

struct bus_ext_ops {
	int	(*enable)(struct scpsys_ext_attr *attr);
	int	(*disable)(struct scpsys_ext_attr *attr);
};

int mtk_generic_set_cmd(struct regmap *regmap, u32 set_ofs,
			u32 sta_ofs, u32 mask);
int mtk_generic_clr_cmd(struct regmap *regmap, u32 clr_ofs,
			u32 sta_ofs, u32 mask);
int mtk_generic_enable_cmd(struct regmap *regmap, u32 upd_ofs,
			   u32 sta_ofs, u32 mask);
int mtk_generic_disable_cmd(struct regmap *regmap, u32 upd_ofs,
			    u32 sta_ofs, u32 mask);

struct scpsys_ext_data *scpsys_ext_init(struct platform_device *pdev);

#endif /* __SOC_MEDIATEK_SCPSYS_EXT_H */
