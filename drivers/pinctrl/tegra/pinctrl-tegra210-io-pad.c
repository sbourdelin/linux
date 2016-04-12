/*
 * Generic ADC thermal driver
 *
 * Copyright (C) 2016 NVIDIA CORPORATION. All rights reserved.
 *
 * Author: Laxman Dewangan <ldewangan@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <soc/tegra/pmc.h>

#include "../core.h"
#include "../pinconf.h"
#include "../pinctrl-utils.h"

enum tegra_io_rail_pads_params {
	TEGRA_IO_RAIL_VOLTAGE = PIN_CONFIG_END + 1,
	TEGRA_IO_PAD_DEEP_POWER_DOWN,
};

static const struct pinconf_generic_params tegra_io_pads_cfg_params[] = {
	{
		.property = "nvidia,io-rail-voltage",
		.param = TEGRA_IO_RAIL_VOLTAGE,
	}, {
		.property = "nvidia,io-pad-deep-power-down",
		.param = TEGRA_IO_PAD_DEEP_POWER_DOWN,
	},
};

struct tegra_io_pads_cfg_info {
	const char *name;
	const unsigned int pins[1];
	int io_rail_id;
};

#define TEGRA210_PAD_INFO_TABLE(_entry_)		\
	_entry_(0, "audio", AUDIO),			\
	_entry_(1, "audio-hv", AUDIO_HV),	\
	_entry_(2, "cam", CAM),			\
	_entry_(3, "csia", CSIA),			\
	_entry_(4, "csib", CSIB),			\
	_entry_(5, "csic", CSIC),			\
	_entry_(6, "csid", CSID),			\
	_entry_(7, "csie", CSIE),			\
	_entry_(8, "csif", CSIF),			\
	_entry_(9, "dbg", DBG),			\
	_entry_(10, "debug-nonao", DBG_NONAO), \
	_entry_(11, "dmic", DMIC),			\
	_entry_(12, "dp", DP),				\
	_entry_(13, "dsi", DSI),			\
	_entry_(14, "dsib", DSIB),			\
	_entry_(15, "dsic", DSIC),			\
	_entry_(16, "dsid", DSID),			\
	_entry_(17, "emmc", SDMMC4),			\
	_entry_(18, "emmc2", EMMC2),			\
	_entry_(19, "gpio", GPIO),			\
	_entry_(20, "hdmi", HDMI),			\
	_entry_(21, "hsic", HSIC),			\
	_entry_(22, "lvds", LVDS),			\
	_entry_(23, "mipi-bias", MIPI_BIAS),	\
	_entry_(24, "pex-bias", PEX_BIAS),	\
	_entry_(25, "pex-clk1", PEX_CLK1),	\
	_entry_(26, "pex-clk2", PEX_CLK2),	\
	_entry_(27, "pex-ctrl", PEX_CNTRL),	\
	_entry_(28, "sdmmc1", SDMMC1),		\
	_entry_(29, "sdmmc3", SDMMC3),		\
	_entry_(30, "spi", SPI),			\
	_entry_(31, "spi-hv", SPI_HV),		\
	_entry_(32, "uart", UART),			\
	_entry_(33, "usb-bias", USB_BIAS),	\
	_entry_(34, "usb0", USB0),			\
	_entry_(35, "usb1", USB1),			\
	_entry_(36, "usb2", USB2),			\
	_entry_(37, "usb3", USB3)

#define TEGRA_IO_PAD_INFO(_id, _name, _io_rail_id)			\
	{								\
		.name = _name,						\
		.pins = {(_id)},					\
		.io_rail_id = TEGRA_IO_RAIL_##_io_rail_id,		\
	}

static struct tegra_io_pads_cfg_info tegra210_io_pads_cfg_info[] = {
	TEGRA210_PAD_INFO_TABLE(TEGRA_IO_PAD_INFO),
};

#define TEGRA_IO_PAD_DESC(_id, _name, _io_rail_id)			\
	PINCTRL_PIN(_id, _name)

static const struct pinctrl_pin_desc tegra210_io_pads_pinctrl_desc[] = {
	TEGRA210_PAD_INFO_TABLE(TEGRA_IO_PAD_DESC),
};

struct tegra_io_pads_info {
	struct device *dev;
	struct pinctrl_dev *pctl;
	struct tegra_io_pads_cfg_info *pads_cfg;
	unsigned int num_pads_cfg;
};

static int tegra_iop_pinctrl_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct tegra_io_pads_info *tiopi = pinctrl_dev_get_drvdata(pctldev);

	return tiopi->num_pads_cfg;
}

static const char *tegra_iop_pinctrl_get_group_name(struct pinctrl_dev *pctldev,
						    unsigned int group)
{
	struct tegra_io_pads_info *tiopi = pinctrl_dev_get_drvdata(pctldev);

	return tiopi->pads_cfg[group].name;
}

static int tegra_iop_pinctrl_get_group_pins(struct pinctrl_dev *pctldev,
					    unsigned int group,
					    const unsigned int **pins,
					    unsigned int *num_pins)
{
	struct tegra_io_pads_info *tiopi = pinctrl_dev_get_drvdata(pctldev);

	*pins = tiopi->pads_cfg[group].pins;
	*num_pins = 1;

	return 0;
}

static const struct pinctrl_ops tegra_iop_pinctrl_ops = {
	.get_groups_count = tegra_iop_pinctrl_get_groups_count,
	.get_group_name = tegra_iop_pinctrl_get_group_name,
	.get_group_pins = tegra_iop_pinctrl_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_pin,
	.dt_free_map = pinctrl_utils_dt_free_map,
};

static int tegra_io_pad_pinconf_get(struct pinctrl_dev *pctldev,
				    unsigned int pin, unsigned long *config)
{
	struct tegra_io_pads_info *tiopi = pinctrl_dev_get_drvdata(pctldev);
	int param = pinconf_to_config_param(*config);
	struct tegra_io_pads_cfg_info *pad_cfg = &tiopi->pads_cfg[pin];
	int io_rail_id = pad_cfg->io_rail_id;
	int arg = 0;
	int ret;

	switch (param) {
	case TEGRA_IO_RAIL_VOLTAGE:
		ret = tegra_io_rail_voltage_get(io_rail_id);
		if (ret < 0)
			return ret;
		arg = ret;
		break;

	case TEGRA_IO_PAD_DEEP_POWER_DOWN:
		ret = tegra_io_rail_power_get_status(io_rail_id);
		if (ret < 0)
			return ret;
		arg = !ret;
		break;

	default:
		dev_err(tiopi->dev, "The parameter %d not supported\n", param);
		return -EINVAL;
	}

	*config = pinconf_to_config_packed(param, (u16)arg);
	return 0;
}

static int tegra_io_pad_pinconf_set(struct pinctrl_dev *pctldev,
				    unsigned int pin, unsigned long *configs,
				    unsigned int num_configs)
{
	struct tegra_io_pads_info *tiopi = pinctrl_dev_get_drvdata(pctldev);
	struct tegra_io_pads_cfg_info *pad_cfg = &tiopi->pads_cfg[pin];
	int io_rail_id = pad_cfg->io_rail_id;
	int param;
	u16 param_val;
	int ret;
	int i;

	for (i = 0; i < num_configs; i++) {
		param = pinconf_to_config_param(configs[i]);
		param_val = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case TEGRA_IO_RAIL_VOLTAGE:
			ret = tegra_io_rail_voltage_set(io_rail_id, param_val);
			if (ret < 0) {
				dev_err(tiopi->dev,
					"Failed to set voltage %d of pin %u: %d\n",
					param_val, pin, ret);
				return ret;
			}
			break;

		case TEGRA_IO_PAD_DEEP_POWER_DOWN:
			if (param_val)
				ret = tegra_io_rail_power_off(io_rail_id);
			else
				ret = tegra_io_rail_power_on(io_rail_id);
			if (ret < 0) {
				dev_err(tiopi->dev,
					"Failed to set DPD %d of pin %u: %d\n",
					param_val, pin, ret);
				return ret;
			}
			break;

		default:
			dev_err(tiopi->dev, "The parameter %d not supported\n",
				param);
			return -EINVAL;
		}
	}

	return 0;
}

static const struct pinconf_ops tegra_io_pad_pinconf_ops = {
	.pin_config_get = tegra_io_pad_pinconf_get,
	.pin_config_set = tegra_io_pad_pinconf_set,
};

static struct pinctrl_desc tegra_iop_pinctrl_desc = {
	.name = "pinctrl-tegra-io-pads",
	.pctlops = &tegra_iop_pinctrl_ops,
	.confops = &tegra_io_pad_pinconf_ops,
	.pins = tegra210_io_pads_pinctrl_desc,
	.npins = ARRAY_SIZE(tegra210_io_pads_pinctrl_desc),
	.custom_params = tegra_io_pads_cfg_params,
	.num_custom_params = ARRAY_SIZE(tegra_io_pads_cfg_params),
};

static int tegra_iop_pinctrl_probe(struct platform_device *pdev)
{
	struct tegra_io_pads_info *tiopi;
	struct device *dev = &pdev->dev;

	tiopi = devm_kzalloc(&pdev->dev, sizeof(*tiopi), GFP_KERNEL);
	if (!tiopi)
		return -ENOMEM;

	tiopi->dev = &pdev->dev;
	tiopi->dev->of_node = pdev->dev.of_node;
	tiopi->pads_cfg = tegra210_io_pads_cfg_info;
	tiopi->num_pads_cfg = ARRAY_SIZE(tegra210_io_pads_cfg_info);

	platform_set_drvdata(pdev, tiopi);

	tiopi->pctl = pinctrl_register(&tegra_iop_pinctrl_desc, dev, tiopi);
	if (IS_ERR(tiopi->pctl)) {
		dev_err(&pdev->dev, "Couldn't register pinctrl driver\n");
		return PTR_ERR(tiopi->pctl);
	}

	return 0;
}

static int tegra_iop_pinctrl_remove(struct platform_device *pdev)
{
	struct tegra_io_pads_info *tiopi = platform_get_drvdata(pdev);

	pinctrl_unregister(tiopi->pctl);

	return 0;
}

static const struct of_device_id tegra_io_pads_of_match[] = {
	{ .compatible = "nvidia,tegra210-io-pad", },
	{},
};
MODULE_DEVICE_TABLE(platform, tegra_iop_pinctrl_devtype);

static struct platform_driver tegra_iop_pinctrl_driver = {
	.driver = {
		.name = "pinctrl-tegra-io-pad",
		.of_match_table = tegra_io_pads_of_match,
	},
	.probe = tegra_iop_pinctrl_probe,
	.remove = tegra_iop_pinctrl_remove,
};

module_platform_driver(tegra_iop_pinctrl_driver);

MODULE_DESCRIPTION("NVIDIA TEGRA IO pad Control Driver");
MODULE_AUTHOR("Laxman Dewangan <ldewangan@nvidia.com>");
MODULE_ALIAS("platform:pinctrl-tegra-io-pads");
MODULE_LICENSE("GPL v2");
