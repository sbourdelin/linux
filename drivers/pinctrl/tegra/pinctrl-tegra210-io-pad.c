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
	TEGRA_IO_PAD_POWER_SOURCE_VOLTAGE = PIN_CONFIG_END + 1,
};

static const struct pinconf_generic_params tegra_io_pads_cfg_params[] = {
	{
		.property = "nvidia,power-source-voltage",
		.param = TEGRA_IO_PAD_POWER_SOURCE_VOLTAGE,
	},
};

struct tegra_io_pads_cfg_info {
	const char *name;
	const unsigned int pins[1];
	int pad_id;
	bool voltage_can_change;
	bool support_low_power_state;
};

#define TEGRA210_PAD_INFO_TABLE(_entry_)			\
	_entry_(0, "audio", AUDIO, true, false),		\
	_entry_(1, "audio-hv", AUDIO_HV, true, true),		\
	_entry_(2, "cam", CAM, true, false),			\
	_entry_(3, "csia", CSIA, true, false),			\
	_entry_(4, "csib", CSIB, true, false),			\
	_entry_(5, "csic", CSIC, true, false),			\
	_entry_(6, "csid", CSID, true, false),			\
	_entry_(7, "csie", CSIE, true, false),			\
	_entry_(8, "csif", CSIF, true, false),			\
	_entry_(9, "dbg", DBG, true, false),			\
	_entry_(10, "debug-nonao", DEBUG_NONAO, true, false),	\
	_entry_(11, "dmic", DMIC, true, false),			\
	_entry_(12, "dp", DP, true, false),			\
	_entry_(13, "dsi", DSI, true, false),			\
	_entry_(14, "dsib", DSIB, true, false),			\
	_entry_(15, "dsic", DSIC, true, false),			\
	_entry_(16, "dsid", DSID, true, false),			\
	_entry_(17, "emmc", SDMMC4, true, false),		\
	_entry_(18, "emmc2", EMMC2, true, false),		\
	_entry_(19, "gpio", GPIO, true, true),			\
	_entry_(20, "hdmi", HDMI, true, false),			\
	_entry_(21, "hsic", HSIC, true, false),			\
	_entry_(22, "lvds", LVDS, true, false),			\
	_entry_(23, "mipi-bias", MIPI_BIAS, true, false),	\
	_entry_(24, "pex-bias", PEX_BIAS, true, false),		\
	_entry_(25, "pex-clk1", PEX_CLK1, true, false),		\
	_entry_(26, "pex-clk2", PEX_CLK2, true, false),		\
	_entry_(27, "pex-ctrl", PEX_CNTRL, true, false),	\
	_entry_(28, "sdmmc1", SDMMC1, true, true),		\
	_entry_(29, "sdmmc3", SDMMC3, true, true),		\
	_entry_(30, "spi", SPI, true, false),			\
	_entry_(31, "spi-hv", SPI_HV, true, true),		\
	_entry_(32, "uart", UART, true, false),			\
	_entry_(33, "usb-bias", USB_BIAS, true, false),		\
	_entry_(34, "usb0", USB0, true, false),			\
	_entry_(35, "usb1", USB1, true, false),			\
	_entry_(36, "usb2", USB2, true, false),			\
	_entry_(37, "usb3", USB3, true, false)

#define TEGRA_IO_PAD_INFO(_id, _name, _pad_id, _vchange, _lpstate)	\
	{								\
		.name = _name,						\
		.pins = {(_id)},					\
		.pad_id = TEGRA_IO_PAD_##_pad_id,			\
		.voltage_can_change = (_vchange),			\
		.support_low_power_state = (_lpstate),			\
	}

static struct tegra_io_pads_cfg_info tegra210_io_pads_cfg_info[] = {
	TEGRA210_PAD_INFO_TABLE(TEGRA_IO_PAD_INFO),
};

#define TEGRA_IO_PAD_DESC(_id, _name, _pad_id,  _vchange, _lpstate)	\
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
	.dt_free_map = pinctrl_utils_free_map,
};

static int tegra_io_pad_pinconf_get(struct pinctrl_dev *pctldev,
				    unsigned int pin, unsigned long *config)
{
	struct tegra_io_pads_info *tiopi = pinctrl_dev_get_drvdata(pctldev);
	int param = pinconf_to_config_param(*config);
	struct tegra_io_pads_cfg_info *pad_cfg = &tiopi->pads_cfg[pin];
	int pad_id = pad_cfg->pad_id;
	int arg = 0;
	int ret;

	switch (param) {
	case TEGRA_IO_PAD_POWER_SOURCE_VOLTAGE:
		ret = tegra_io_pads_get_configured_voltage(pad_id);
		if (ret < 0)
			return ret;
		arg = (ret =  3300000) ? 1 : 0;
		break;

	case PIN_CONFIG_LOW_POWER_MODE:
		ret = tegra_io_pads_power_is_enabled(pad_id);
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
	u16 param_val;
	int pad_id = pad_cfg->pad_id;
	int volt;
	int param;
	int ret;
	int i;

	for (i = 0; i < num_configs; i++) {
		param = pinconf_to_config_param(configs[i]);
		param_val = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case TEGRA_IO_PAD_POWER_SOURCE_VOLTAGE:
			volt = (param_val) ? 3300000 : 1800000;
			ret = tegra_io_pads_configure_voltage(pad_id, volt);
			if (ret < 0) {
				dev_err(tiopi->dev,
					"Failed to configure pad %s for voltage %d: %d\n",
					pad_cfg->name, param_val, ret);
				return ret;
			}
			break;

		case PIN_CONFIG_LOW_POWER_MODE:
			if (param_val)
				ret = tegra_io_pads_power_disable(pad_id);
			else
				ret = tegra_io_pads_power_enable(pad_id);
			if (ret < 0) {
				dev_err(tiopi->dev,
					"Failed to set low power %s of pad %s: %d\n",
					param_val ? "enable" : "disable",
					pad_cfg->name, ret);
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
	struct device *dev = &pdev->dev;
	struct device_node *np_parent = pdev->dev.parent->of_node;
	struct tegra_io_pads_info *tiopi;

	if (!np_parent) {
		dev_err(&pdev->dev, "PMC should be register from DT\n");
		return -ENODEV;
	}

	pdev->dev.of_node = np_parent;

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

static struct platform_driver tegra_iop_pinctrl_driver = {
	.driver = {
		.name = "pinctrl-tegra210-io-pad",
	},
	.probe = tegra_iop_pinctrl_probe,
	.remove = tegra_iop_pinctrl_remove,
};

builtin_platform_driver(tegra_iop_pinctrl_driver);
