// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018 Intel Corporation

#include <linux/bitfield.h>
#include <linux/mfd/core.h>
#include <linux/mfd/intel-peci-client.h>
#include <linux/module.h>
#include <linux/peci.h>
#include <linux/of_device.h>

enum cpu_gens {
	CPU_GEN_HSX = 0, /* Haswell Xeon */
	CPU_GEN_BRX,     /* Broadwell Xeon */
	CPU_GEN_SKX,     /* Skylake Xeon */
};

static struct mfd_cell peci_functions[] = {
	{
		.name = "peci-cputemp",
	},
	{
		.name = "peci-dimmtemp",
	},
	/* TODO: Add additional PECI sideband functions into here */
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

static int peci_client_get_cpu_gen_info(struct peci_mfd *priv)
{
	u32 cpu_id;
	int i, rc;

	rc = peci_get_cpu_id(priv->adapter, priv->addr, &cpu_id);
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

	priv->gen_info = &cpu_gen_info_table[i];

	return 0;
}

bool peci_temp_need_update(struct temp_data *temp)
{
	if (temp->valid &&
	    time_before(jiffies, temp->last_updated + UPDATE_INTERVAL))
		return false;

	return true;
}
EXPORT_SYMBOL_GPL(peci_temp_need_update);

void peci_temp_mark_updated(struct temp_data *temp)
{
	temp->valid = 1;
	temp->last_updated = jiffies;
}
EXPORT_SYMBOL_GPL(peci_temp_mark_updated);

int peci_client_command(struct peci_mfd *priv, enum peci_cmd cmd, void *vmsg)
{
	return peci_command(priv->adapter, cmd, vmsg);
}
EXPORT_SYMBOL_GPL(peci_client_command);

int peci_client_rd_pkg_cfg_cmd(struct peci_mfd *priv, u8 mbx_idx,
			       u16 param, u8 *data)
{
	struct peci_rd_pkg_cfg_msg msg;
	int rc;

	msg.addr = priv->addr;
	msg.index = mbx_idx;
	msg.param = param;
	msg.rx_len = 4;

	rc = peci_command(priv->adapter, PECI_CMD_RD_PKG_CFG, &msg);
	if (!rc)
		memcpy(data, msg.pkg_config, 4);

	return rc;
}
EXPORT_SYMBOL_GPL(peci_client_rd_pkg_cfg_cmd);

static int peci_client_probe(struct peci_client *client)
{
	struct device *dev = &client->dev;
	struct peci_mfd *priv;
	int rc;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dev_set_drvdata(dev, priv);
	priv->client = client;
	priv->dev = dev;
	priv->adapter = client->adapter;
	priv->addr = client->addr;
	priv->cpu_no = client->addr - PECI_BASE_ADDR;

	snprintf(priv->name, PECI_NAME_SIZE, "peci_client.cpu%d", priv->cpu_no);

	rc = peci_client_get_cpu_gen_info(priv);
	if (rc)
		return rc;

	rc = devm_mfd_add_devices(priv->dev, priv->cpu_no, peci_functions,
				  ARRAY_SIZE(peci_functions), NULL, 0, NULL);
	if (rc < 0) {
		dev_err(priv->dev, "devm_mfd_add_devices failed: %d\n", rc);
		return rc;
	}

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id peci_client_of_table[] = {
	{ .compatible = "intel,peci-client" },
	{ }
};
MODULE_DEVICE_TABLE(of, peci_client_of_table);
#endif

static const struct peci_device_id peci_client_ids[] = {
	{ .name = "peci-client", .driver_data = 0 },
	{ }
};
MODULE_DEVICE_TABLE(peci, peci_client_ids);

static struct peci_driver peci_client_driver = {
	.probe    = peci_client_probe,
	.id_table = peci_client_ids,
	.driver   = {
			.name           = "peci-client",
			.of_match_table =
				of_match_ptr(peci_client_of_table),
	},
};
module_peci_driver(peci_client_driver);

MODULE_AUTHOR("Jae Hyun Yoo <jae.hyun.yoo@linux.intel.com>");
MODULE_DESCRIPTION("PECI client MFD driver");
MODULE_LICENSE("GPL v2");
