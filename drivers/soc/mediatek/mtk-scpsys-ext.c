// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 MediaTek Inc.
 * Author: Owen Chen <owen.chen@mediatek.com>
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/mfd/syscon.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/infracfg.h>
#include <linux/soc/mediatek/scpsys-ext.h>
#include <dt-bindings/power/mt6765-power.h>

#define MAX_CLKS		10
#define INFRA			"infracfg"
#define SMIC			"smi_comm"

static LIST_HEAD(ext_clk_map_list);
static LIST_HEAD(ext_attr_map_list);

static struct regmap *infracfg;
static struct regmap *smi_comm;

enum regmap_type {
	IFR_TYPE,
	SMI_TYPE,
	MAX_REGMAP_TYPE,
};

/**
 * struct ext_reg_ctrl - set multiple register for bus protect
 * @regmap: The bus protect regmap, 1: infracfg, 2: other master regmap
 *                  such as SMI.
 * @set_ofs: The set register offset to set corresponding bit to 1.
 * @clr_ofs: The clr register offset to clear corresponding bit to 0.
 * @sta_ofs: The status register offset to show bus protect enable/disable.
 */
struct ext_reg_ctrl {
	enum regmap_type type;
	u32 set_ofs;
	u32 clr_ofs;
	u32 sta_ofs;
};

/**
 * struct ext_clk_ctrl - enable multiple clks for bus protect
 * @clk: The clk need to enable before pwr on/bus protect.
 * @scpd_n: The name present the scpsys domain where the clks belongs to.
 * @clk_list: The list node linked to ext_clk_map_list.
 */
struct ext_clk_ctrl {
	struct clk *clk;
	const char *scpd_n;
	struct list_head clk_list;
};

struct bus_mask_ops {
	int	(*set)(struct regmap *regmap, u32 set_ofs,
		       u32 sta_ofs, u32 mask);
	int	(*release)(struct regmap *regmap, u32 clr_ofs,
			   u32 sta_ofs, u32 mask);
};

static struct scpsys_ext_attr *__get_attr_node(const char *scpd_n)
{
	struct scpsys_ext_attr *attr;

	if (!scpd_n)
		return ERR_PTR(-EINVAL);

	list_for_each_entry(attr, &ext_attr_map_list, attr_list) {
		if (attr->scpd_n && !strcmp(scpd_n, attr->scpd_n))
			return attr;
	}

	return ERR_PTR(-EINVAL);
}

static struct scpsys_ext_attr *__get_attr_parent(const char *parent_n)
{
	struct scpsys_ext_attr *attr;

	if (!parent_n)
		return ERR_PTR(-EINVAL);

	list_for_each_entry(attr, &ext_attr_map_list, attr_list) {
		if (attr->scpd_n && !strcmp(parent_n, attr->scpd_n))
			return attr;
	}

	return ERR_PTR(-EINVAL);
}

int bus_ctrl_set_release(struct scpsys_ext_attr *attr, bool set)
{
	int i;
	int ret = 0;

	for (i = 0; i < MAX_STEP_NUM && attr->mask[i].mask; i++) {
		struct ext_reg_ctrl *rc = attr->mask[i].regs;
		struct regmap *regmap;

		if (rc->type == IFR_TYPE)
			regmap = infracfg;
		else if (rc->type == SMI_TYPE)
			regmap = smi_comm;
		else
			return -EINVAL;

		if (set)
			ret = attr->mask[i].ops->set(regmap,
						rc->set_ofs,
						rc->sta_ofs,
						attr->mask[i].mask);
		else
			ret = attr->mask[i].ops->release(regmap,
						rc->clr_ofs,
						rc->sta_ofs,
						attr->mask[i].mask);
	}

	return ret;
}

int bus_ctrl_set(struct scpsys_ext_attr *attr)
{
	return bus_ctrl_set_release(attr, CMD_ENABLE);
}

int bus_ctrl_release(struct scpsys_ext_attr *attr)
{
	return bus_ctrl_set_release(attr, CMD_DISABLE);
}

int bus_clk_enable_disable(struct scpsys_ext_attr *attr, bool enable)
{
	int i = 0;
	int ret = 0;
	struct ext_clk_ctrl *cc;
	struct clk *clk[MAX_CLKS];

	list_for_each_entry(cc, &ext_clk_map_list, clk_list) {
		if (!strcmp(cc->scpd_n, attr->scpd_n)) {
			if (enable)
				ret = clk_prepare_enable(cc->clk);
			else
				clk_disable_unprepare(cc->clk);

			if (ret) {
				pr_err("Failed to  %s %s\n",
				       enable ? "enable" : "disable",
				       __clk_get_name(cc->clk));
				goto err;
			} else {
				clk[i] = cc->clk;
				i++;
			}
		}
	}

	return ret;

err:
	for (--i; i >= 0; i--)
		if (enable)
			clk_disable_unprepare(clk[i]);
		else
			clk_prepare_enable(clk[i]);
	return ret;
}

int bus_clk_enable(struct scpsys_ext_attr *attr)
{
	struct scpsys_ext_attr *attr_p;
	int ret = 0;

	attr_p = __get_attr_parent(attr->parent_n);
	if (!IS_ERR(attr_p)) {
		ret = bus_clk_enable_disable(attr_p, CMD_ENABLE);
		if (ret)
			return ret;
	}

	return bus_clk_enable_disable(attr, CMD_ENABLE);
}

int bus_clk_disable(struct scpsys_ext_attr *attr)
{
	struct scpsys_ext_attr *attr_p;
	int ret = 0;

	ret = bus_clk_enable_disable(attr, CMD_DISABLE);
	if (ret)
		return ret;

	attr_p = __get_attr_parent(attr->parent_n);
	if (!IS_ERR(attr_p))
		ret = bus_clk_enable_disable(attr_p, CMD_DISABLE);

	return ret;
}

const struct bus_mask_ops bus_mask_set_clr_ctrl = {
	.set = &mtk_generic_set_cmd,
	.release = &mtk_generic_clr_cmd,
};

const struct bus_ext_ops ext_bus_ctrl = {
	.enable = &bus_ctrl_set,
	.disable = &bus_ctrl_release,
};

const struct bus_ext_ops ext_cg_ctrl = {
	.enable = &bus_clk_enable,
	.disable = &bus_clk_disable,
};

/*
 * scpsys bus driver init
 */
struct regmap *syscon_regmap_lookup_by_phandle_idx(struct device_node *np,
						   const char *property,
						   int index)
{
	struct device_node *syscon_np;
	struct regmap *regmap;

	if (property)
		syscon_np = of_parse_phandle(np, property, index);
	else
		syscon_np = np;

	if (!syscon_np)
		return ERR_PTR(-ENODEV);

	regmap = syscon_node_to_regmap(syscon_np);
	of_node_put(syscon_np);

	return regmap;
}

int scpsys_ext_regmap_init(struct platform_device *pdev)
{
	infracfg = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						   INFRA);
	if (IS_ERR(infracfg)) {
		dev_err(&pdev->dev,
			"Cannot find bus infracfg controller: %ld\n",
			PTR_ERR(infracfg));
		return PTR_ERR(infracfg);
	}

	smi_comm = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						   SMIC);
	if (IS_ERR(smi_comm)) {
		dev_err(&pdev->dev,
			"Cannot find bus smi_comm controller: %ld\n",
			PTR_ERR(smi_comm));
		return PTR_ERR(smi_comm);
	}

	return 0;
}

static int add_clk_to_list(struct platform_device *pdev,
			   const char *name,
			   const char *scpd_n)
{
	struct clk *clk;
	struct ext_clk_ctrl *cc;

	clk = devm_clk_get(&pdev->dev, name);
	if (IS_ERR(clk)) {
		dev_err(&pdev->dev, "Failed add clk %ld\n", PTR_ERR(clk));
		return PTR_ERR(clk);
	}

	cc = kzalloc(sizeof(*cc), GFP_KERNEL);
	cc->clk = clk;
	cc->scpd_n = kstrdup(scpd_n, GFP_KERNEL);

	list_add(&cc->clk_list, &ext_clk_map_list);

	return 0;
}

static int add_cg_to_list(struct platform_device *pdev)
{
	int i = 0;

	struct device_node *node = pdev->dev.of_node;

	if (!node) {
		dev_err(&pdev->dev, "Cannot find topcksys node: %ld\n",
			PTR_ERR(node));
		return PTR_ERR(node);
	}

	do {
		const char *ck_name;
		char *temp_str;
		char *tok[2] = {NULL};
		int cg_idx = 0;
		int idx = 0;
		int ret = 0;

		ret = of_property_read_string_index(node, "clock-names", i,
						    &ck_name);
		if (ret < 0)
			break;

		temp_str = kmalloc_array(strlen(ck_name), sizeof(char),
					 GFP_KERNEL | __GFP_ZERO);
		memcpy(temp_str, ck_name, strlen(ck_name));
		temp_str[strlen(ck_name)] = '\0';
		do {
			tok[idx] = strsep(&temp_str, "-");
			idx++;
		} while (temp_str);

		if (idx == 2) {
			if (kstrtouint(tok[1], 10, &cg_idx))
				return -EINVAL;

			if (add_clk_to_list(pdev, ck_name, tok[0]))
				return -EINVAL;
		}
		kfree(temp_str);
		i++;
	} while (1);

	return 0;
}

int scpsys_ext_clk_init(struct platform_device *pdev)
{
	int ret = 0;

	ret = add_cg_to_list(pdev);
	if (ret)
		goto err;

err:
	return ret;
}

int scpsys_ext_attr_init(const struct scpsys_ext_data *data)
{
	int i, count = 0;

	for (i = 0; i < data->num_attr; i++) {
		struct scpsys_ext_attr *node = data->attr + i;

		if (!node)
			continue;

		list_add(&node->attr_list, &ext_attr_map_list);
		count++;
	}

	if (!count)
		return -EINVAL;

	return 0;
}

/*
 * MT6765 extend power domain support
 */

#define INFRA_TOPAXI_PROTECTEN_SET_MT6765	0x02A0
#define INFRA_TOPAXI_PROTECTEN_STA1_MT6765	0x0228
#define INFRA_TOPAXI_PROTECTEN_CLR_MT6765	0x02A4

#define INFRA_TOPAXI_PROTECTEN_1_SET_MT6765	0x02A8
#define INFRA_TOPAXI_PROTECTEN_STA1_1_MT6765	0x0258
#define INFRA_TOPAXI_PROTECTEN_1_CLR_MT6765	0x02AC

#define SMI_COMMON_SMI_CLAMP_MT6765		0x03C0
#define SMI_COMMON_SMI_CLAMP_SET_MT6765	0x03C4
#define SMI_COMMON_SMI_CLAMP_CLR_MT6765	0x03C8

static struct ext_reg_ctrl infra_bus_regs_0_mt6765 = {
	.type = IFR_TYPE,
	.set_ofs = INFRA_TOPAXI_PROTECTEN_SET_MT6765,
	.clr_ofs = INFRA_TOPAXI_PROTECTEN_CLR_MT6765,
	.sta_ofs = INFRA_TOPAXI_PROTECTEN_STA1_MT6765,
};

#define BUS_IFR0_MT6765(_mask) {				\
		.regs = &infra_bus_regs_0_mt6765,		\
		.mask = _mask,				\
		.ops = &bus_mask_set_clr_ctrl,		\
	}

static struct ext_reg_ctrl infra_bus_regs_1_mt6765 = {
	.type = IFR_TYPE,
	.set_ofs = INFRA_TOPAXI_PROTECTEN_1_SET_MT6765,
	.clr_ofs = INFRA_TOPAXI_PROTECTEN_1_CLR_MT6765,
	.sta_ofs = INFRA_TOPAXI_PROTECTEN_STA1_1_MT6765,
};

#define BUS_IFR1_MT6765(_mask) {				\
		.regs = &infra_bus_regs_1_mt6765,		\
		.mask = _mask,				\
		.ops = &bus_mask_set_clr_ctrl,		\
	}

static struct ext_reg_ctrl smi_bus_regs_0_mt6765 = {
	.type = SMI_TYPE,
	.set_ofs = SMI_COMMON_SMI_CLAMP_SET_MT6765,
	.clr_ofs = SMI_COMMON_SMI_CLAMP_CLR_MT6765,
	.sta_ofs = SMI_COMMON_SMI_CLAMP_MT6765,
};

#define BUS_SMI0_MT6765(_mask) {				\
		.regs = &smi_bus_regs_0_mt6765,		\
		.mask = _mask,				\
		.ops = &bus_mask_set_clr_ctrl,		\
	}

static struct scpsys_ext_attr scp_ext_attr_mt6765[] = {
	[MT6765_POWER_DOMAIN_ISP] = {
		.scpd_n = "isp",
		.mask =  {
			BUS_IFR1_MT6765(BIT(20)),
			BUS_SMI0_MT6765(BIT(2)),
		},
		.parent_n = "mm",
		.bus_ops = &ext_bus_ctrl,
		.cg_ops = &ext_cg_ctrl,
	},
	[MT6765_POWER_DOMAIN_MM] = {
		.scpd_n = "mm",
		.mask = {
			BUS_IFR1_MT6765(BIT(16) | BIT(17)),
			BUS_IFR0_MT6765(BIT(10) | BIT(11)),
			BUS_IFR0_MT6765(BIT(1) | BIT(2)),
		},
		.bus_ops = &ext_bus_ctrl,
		.cg_ops = &ext_cg_ctrl,
	},
	[MT6765_POWER_DOMAIN_CONN] = {
		.scpd_n = "conn",
		.mask = {
			BUS_IFR0_MT6765(BIT(13)),
			BUS_IFR1_MT6765(BIT(18)),
			BUS_IFR0_MT6765(BIT(14) | BIT(16)),
		},
		.bus_ops = &ext_bus_ctrl,
	},
	[MT6765_POWER_DOMAIN_MFG] = {
		.scpd_n = "mfg",
		.mask = {
			BUS_IFR0_MT6765(BIT(25)),
			BUS_IFR0_MT6765(BIT(21) | BIT(22)),
		},
		.bus_ops = &ext_bus_ctrl,
	},
	[MT6765_POWER_DOMAIN_CAM] = {
		.scpd_n = "cam",
		.mask = {
			BUS_IFR1_MT6765(BIT(19) | BIT(21)),
			BUS_IFR0_MT6765(BIT(20)),
			BUS_SMI0_MT6765(BIT(3)),
		},
		.parent_n = "mm",
		.bus_ops = &ext_bus_ctrl,
		.cg_ops = &ext_cg_ctrl,
	},
};

static const struct scpsys_ext_data scp_ext_data_mt6765 = {
	.attr = scp_ext_attr_mt6765,
	.num_attr = ARRAY_SIZE(scp_ext_attr_mt6765),
	.get_attr = __get_attr_node,
};

static const struct of_device_id of_scpsys_ext_match_tbl[] = {
	{
		.compatible = "mediatek,mt6765-scpsys",
		.data = &scp_ext_data_mt6765,
	}, {
		/* sentinel */
	}
};

struct scpsys_ext_data *scpsys_ext_init(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct scpsys_ext_data *data;
	int ret;

	match = of_match_device(of_scpsys_ext_match_tbl, &pdev->dev);

	if (!match) {
		dev_err(&pdev->dev, "no match\n");
		return ERR_CAST(match);
	}

	data = (struct scpsys_ext_data *)match->data;
	if (IS_ERR(data)) {
		dev_err(&pdev->dev, "no match scpext data\n");
		return ERR_CAST(data);
	}

	ret = scpsys_ext_attr_init(data);
	if (ret) {
		dev_err(&pdev->dev,
			"Failed to init bus attr: %d\n",
			ret);
		return ERR_PTR(ret);
	}

	ret = scpsys_ext_regmap_init(pdev);
	if (ret) {
		dev_err(&pdev->dev,
			"Failed to init bus register: %d\n",
			ret);
		return ERR_PTR(ret);
	}

	ret = scpsys_ext_clk_init(pdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to init bus clks: %d\n",
			ret);
		return ERR_PTR(ret);
	}

	return data;
}
