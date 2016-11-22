/*
 * pinctrl-tegra-io-pad: IO PAD driver for configuration of IO rail and deep
 *			 Power Down mode via pinctrl framework.
 *
 * Copyright (C) 2016 NVIDIA CORPORATION. All rights reserved.
 *
 * Author: Laxman Dewangan <ldewangan@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <soc/tegra/pmc.h>

#include "../core.h"
#include "../pinconf.h"
#include "../pinctrl-utils.h"

#define TEGRA_IO_RAIL_1800000UV 1800000
#define TEGRA_IO_RAIL_3300000UV 3300000

/* Covert IO voltage to IO pad voltage enum */
#define tegra_io_uv_to_io_pads_uv(io_uv)				\
		(((io_uv) == TEGRA_IO_RAIL_1800000UV) ?			\
		  TEGRA_IO_PAD_1800000UV : TEGRA_IO_PAD_3300000UV)

#define tegra_io_voltage_is_valid(io_uv)			\
	({ typeof(io_uv) io_uv_ = (io_uv);			\
	    ((io_uv_ == TEGRA_IO_RAIL_1800000UV) ||		\
	     (io_uv_ == TEGRA_IO_RAIL_3300000UV)); })

struct tegra_io_pads_cfg {
	const char *name;
	const unsigned int pins[1];
	const char *vsupply;
	enum tegra_io_pad id;
	bool supports_low_power;
};

struct tegra_io_pads_soc_data {
	const struct tegra_io_pads_cfg *cfg;
	int num_cfg;
	const struct pinctrl_pin_desc *desc;
	int num_desc;
};

struct tegra_io_pads_info {
	struct device *dev;
	struct pinctrl_dev *pctl;
	const struct tegra_io_pads_soc_data *soc_data;
};

struct tegra_io_pads_regulator_info {
	struct tegra_io_pads_info *tiopi;
	const struct tegra_io_pads_cfg *cfg;
	struct regulator *regulator;
	struct notifier_block regulator_nb;
};

static int tegra_io_pads_pinctrl_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct tegra_io_pads_info *tiopi = pinctrl_dev_get_drvdata(pctldev);

	return tiopi->soc_data->num_cfg;
}

static const char *tegra_io_pads_pinctrl_get_group_name(
		struct pinctrl_dev *pctldev, unsigned int group)
{
	struct tegra_io_pads_info *tiopi = pinctrl_dev_get_drvdata(pctldev);

	return tiopi->soc_data->cfg[group].name;
}

static int tegra_io_pads_pinctrl_get_group_pins(struct pinctrl_dev *pctldev,
						unsigned int group,
						const unsigned int **pins,
						unsigned int *num_pins)
{
	struct tegra_io_pads_info *tiopi = pinctrl_dev_get_drvdata(pctldev);

	*pins = tiopi->soc_data->cfg[group].pins;
	*num_pins = 1;

	return 0;
}

static const struct pinctrl_ops tegra_io_pads_pinctrl_ops = {
	.get_groups_count	= tegra_io_pads_pinctrl_get_groups_count,
	.get_group_name		= tegra_io_pads_pinctrl_get_group_name,
	.get_group_pins		= tegra_io_pads_pinctrl_get_group_pins,
	.dt_node_to_map		= pinconf_generic_dt_node_to_map_pin,
	.dt_free_map		= pinctrl_utils_free_map,
};

static int tegra_io_pads_pinconf_get(struct pinctrl_dev *pctldev,
				     unsigned int pin, unsigned long *config)
{
	struct tegra_io_pads_info *tiopi = pinctrl_dev_get_drvdata(pctldev);
	int param = pinconf_to_config_param(*config);
	const struct tegra_io_pads_cfg *cfg = &tiopi->soc_data->cfg[pin];
	int arg = 0;
	int ret;

	switch (param) {
	case PIN_CONFIG_LOW_POWER_MODE:
		if (!cfg->supports_low_power) {
			dev_err(tiopi->dev,
				"IO pad %s does not support low power\n",
				cfg->name);
			return -EINVAL;
		}

		ret = tegra_io_pad_power_get_status(cfg->id);
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

static int tegra_io_pads_pinconf_set(struct pinctrl_dev *pctldev,
				     unsigned int pin, unsigned long *configs,
				    unsigned int num_configs)
{
	struct tegra_io_pads_info *tiopi = pinctrl_dev_get_drvdata(pctldev);
	const struct tegra_io_pads_cfg *cfg = &tiopi->soc_data->cfg[pin];
	int i;

	for (i = 0; i < num_configs; i++) {
		int ret;
		int param = pinconf_to_config_param(configs[i]);
		u16 param_val = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case PIN_CONFIG_LOW_POWER_MODE:
			if (!cfg->supports_low_power) {
				dev_err(tiopi->dev,
					"IO pad %s does not support low power\n",
					cfg->name);
				return -EINVAL;
			}
			if (param_val)
				ret = tegra_io_pad_power_disable(cfg->id);
			else
				ret = tegra_io_pad_power_enable(cfg->id);
			if (ret < 0) {
				dev_err(tiopi->dev,
					"Failed to set DPD %d of io-pad %s: %d\n",
					param_val, cfg->name, ret);
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

static const struct pinconf_ops tegra_io_pads_pinconf_ops = {
	.pin_config_get = tegra_io_pads_pinconf_get,
	.pin_config_set = tegra_io_pads_pinconf_set,
};

static struct pinctrl_desc tegra_io_pads_pinctrl_desc = {
	.name = "pinctrl-tegra-io-pads",
	.pctlops = &tegra_io_pads_pinctrl_ops,
	.confops = &tegra_io_pads_pinconf_ops,
};

static int tegra_io_pads_rail_change_notify_cb(struct notifier_block *nb,
					       unsigned long event, void *data)
{
	struct tegra_io_pads_regulator_info *rinfo;
	struct pre_voltage_change_data *vdata;
	unsigned long int io_volt_uv;
	enum tegra_io_pad_voltage pad_volt;
	int ret;

	rinfo = container_of(nb, struct tegra_io_pads_regulator_info,
			     regulator_nb);

	switch (event) {
	case REGULATOR_EVENT_PRE_VOLTAGE_CHANGE:
		vdata = data;

		if (!tegra_io_voltage_is_valid(vdata->old_uV) ||
		    !tegra_io_voltage_is_valid(vdata->min_uV)) {
			dev_err(rinfo->tiopi->dev,
				"IO rail %s voltage is not 1.8/3.3V: %lu:%lu\n",
				rinfo->cfg->name, vdata->old_uV, vdata->min_uV);
			return -EINVAL;
		}

		/**
		 * Change IO pad voltage before changing IO voltage when it
		 * changes from 1.8V to 3.3V
		 */
		if (vdata->min_uV == TEGRA_IO_RAIL_1800000UV)
			break;

		ret = tegra_io_pad_set_voltage(rinfo->cfg->id,
					       TEGRA_IO_PAD_3300000UV);
		if (ret < 0) {
			dev_err(rinfo->tiopi->dev,
				"Failed to set voltage %lu of pad %s: %d\n",
				vdata->min_uV, rinfo->cfg->name, ret);
			return ret;
		}
		break;

	case REGULATOR_EVENT_VOLTAGE_CHANGE:
		io_volt_uv = (unsigned long)data;
		ret = tegra_io_pad_get_voltage(rinfo->cfg->id);
		if (ret < 0) {
			dev_err(rinfo->tiopi->dev,
				"Failed to get IO pad voltage: %d\n", ret);
			return ret;
		}

		if (!tegra_io_voltage_is_valid(io_volt_uv)) {
			dev_err(rinfo->tiopi->dev,
				"IO rail %s voltage is not 1.8/3.3V: %lu\n",
				rinfo->cfg->name, io_volt_uv);
			return -EINVAL;
		}

		/*
		 * If IO pad configuration matching with IO rail voltage then
		 * do nothing.
		 */
		if (((io_volt_uv == TEGRA_IO_RAIL_1800000UV) &&
		     (ret == TEGRA_IO_PAD_1800000UV)) ||
		     ((io_volt_uv == TEGRA_IO_RAIL_3300000UV) &&
		      (ret == TEGRA_IO_PAD_3300000UV)))
			break;

		ret = tegra_io_pad_set_voltage(rinfo->cfg->id,
					       TEGRA_IO_PAD_1800000UV);
		if (ret < 0) {
			dev_err(rinfo->tiopi->dev,
				"Failed to set voltage %lu of pad %s: %d\n",
				vdata->min_uV, rinfo->cfg->name, ret);
			return ret;
		}
		break;

	case REGULATOR_EVENT_ABORT_VOLTAGE_CHANGE:
		io_volt_uv = (unsigned long)data;

		if (!tegra_io_voltage_is_valid(io_volt_uv)) {
			dev_err(rinfo->tiopi->dev,
				"IO rail %s voltage is not 1.8/3.3V: %lu\n",
				rinfo->cfg->name, io_volt_uv);
			return -EINVAL;
		}

		pad_volt = tegra_io_uv_to_io_pads_uv(io_volt_uv);
		ret = tegra_io_pad_set_voltage(rinfo->cfg->id, pad_volt);
		if (ret < 0) {
			dev_err(rinfo->tiopi->dev,
				"Failed to set voltage %lu of pad %s: %d\n",
				io_volt_uv, rinfo->cfg->name, ret);
			return ret;
		}
		break;

	default:
		break;
	}

	return NOTIFY_OK;
}

static int tegra_io_pads_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct platform_device_id *id = platform_get_device_id(pdev);
	const struct tegra_io_pads_soc_data *soc_data =
			(const struct tegra_io_pads_soc_data *)id->driver_data;
	struct tegra_io_pads_info *tiopi;
	int ret, i;

	if (!pdev->dev.parent->of_node) {
		dev_err(dev, "PMC should be register from DT\n");
		return -ENODEV;
	}

	tiopi = devm_kzalloc(dev, sizeof(*tiopi), GFP_KERNEL);
	if (!tiopi)
		return -ENOMEM;

	tiopi->dev = &pdev->dev;
	pdev->dev.of_node = pdev->dev.parent->of_node;
	tiopi->soc_data = soc_data;

	for (i = 0; i < soc_data->num_cfg; ++i) {
		struct tegra_io_pads_regulator_info *rinfo;
		enum tegra_io_pad_voltage pad_volt;
		int io_volt_uv;

		if (!soc_data->cfg[i].vsupply)
			continue;

		rinfo = devm_kzalloc(dev, sizeof(*rinfo), GFP_KERNEL);
		if (!rinfo)
			return -ENOMEM;

		rinfo->tiopi = tiopi;
		rinfo->cfg = &soc_data->cfg[i];

		rinfo->regulator = devm_regulator_get_optional(dev,
						soc_data->cfg[i].vsupply);
		if (IS_ERR(rinfo->regulator)) {
			ret = PTR_ERR(rinfo->regulator);
			if (ret == -EPROBE_DEFER)
				return ret;
			continue;
		}

		io_volt_uv = regulator_get_voltage(rinfo->regulator);
		if (io_volt_uv < 0) {
			dev_err(dev, "Failed to get voltage for rail %s: %d\n",
				soc_data->cfg[i].vsupply, io_volt_uv);
			return ret;
		}

		if (!tegra_io_voltage_is_valid(io_volt_uv)) {
			dev_err(dev, "IO rail %s voltage is not 1.8/3.3V: %d\n",
				soc_data->cfg[i].vsupply, io_volt_uv);
			continue;
		}

		pad_volt = tegra_io_uv_to_io_pads_uv(io_volt_uv);
		ret = tegra_io_pad_set_voltage(soc_data->cfg[i].id, pad_volt);
		if (ret < 0) {
			dev_err(dev, "Failed to set voltage %d of pad %s: %d\n",
				io_volt_uv, soc_data->cfg[i].name, ret);
			return ret;
		}

		rinfo->regulator_nb.notifier_call =
					tegra_io_pads_rail_change_notify_cb;
		ret = devm_regulator_register_notifier(rinfo->regulator,
						       &rinfo->regulator_nb);
		if (ret < 0) {
			dev_err(dev, "Failed to register regulator %s notifier: %d\n",
				soc_data->cfg[i].name, ret);
			return ret;
		}
	}

	tegra_io_pads_pinctrl_desc.pins = tiopi->soc_data->desc;
	tegra_io_pads_pinctrl_desc.npins = tiopi->soc_data->num_desc;
	platform_set_drvdata(pdev, tiopi);

	tiopi->pctl = devm_pinctrl_register(dev, &tegra_io_pads_pinctrl_desc,
					    tiopi);
	if (IS_ERR(tiopi->pctl)) {
		ret = PTR_ERR(tiopi->pctl);
		dev_err(dev, "Failed to register io-pad pinctrl driver: %d\n",
			ret);
		return ret;
	}

	return 0;
}

#define TEGRA124_PAD_INFO_TABLE(_entry_)			\
	_entry_(0, "audio", AUDIO, true, NULL),			\
	_entry_(1, "bb", BB, true, NULL),			\
	_entry_(2, "cam", CAM, true, NULL),			\
	_entry_(3, "comp", COMP, true, NULL),			\
	_entry_(4, "csia", CSIA, true, NULL),			\
	_entry_(5, "csib", CSIB, true, NULL),			\
	_entry_(6, "csie", CSIE, true, NULL),			\
	_entry_(7, "dsi", DSI, true, NULL),			\
	_entry_(8, "dsib", DSIB, true, NULL),			\
	_entry_(9, "dsic", DSIC, true, NULL),			\
	_entry_(10, "dsid", DSID, true, NULL),			\
	_entry_(11, "hdmi", HDMI, true, NULL),			\
	_entry_(12, "hsic", HSIC, true, NULL),			\
	_entry_(13, "hv", HV, true, NULL),			\
	_entry_(14, "lvds", LVDS, true, NULL),			\
	_entry_(15, "mipi-bias", MIPI_BIAS, true, NULL),	\
	_entry_(16, "nand", NAND, true, NULL),			\
	_entry_(17, "pex-bias", PEX_BIAS, true, NULL),		\
	_entry_(18, "pex-clk1", PEX_CLK1, true, NULL),		\
	_entry_(19, "pex-clk2", PEX_CLK2, true, NULL),		\
	_entry_(20, "pex-ctrl", PEX_CNTRL, true, NULL),		\
	_entry_(21, "sdmmc1", SDMMC1, true, NULL),		\
	_entry_(22, "sdmmc3", SDMMC3, true, NULL),		\
	_entry_(23, "sdmmc4", SDMMC4, true, NULL),		\
	_entry_(24, "sys-ddc", SYS_DDC, true, NULL),		\
	_entry_(25, "uart", UART, true, NULL),			\
	_entry_(26, "usb0", USB0, true, NULL),			\
	_entry_(27, "usb1", USB1, true, NULL),			\
	_entry_(28, "usb2", USB2, true, NULL),			\
	_entry_(29, "usb-bias", USB_BIAS, true, NULL)

#define TEGRA210_PAD_INFO_TABLE(_entry_)			\
	_entry_(0, "audio", AUDIO, true, "vddio-audio"),	\
	_entry_(1, "audio-hv", AUDIO_HV, true, "vddio-audio-hv"), \
	_entry_(2, "cam", CAM, true, "vddio-cam"),		\
	_entry_(3, "csia", CSIA, true, NULL),			\
	_entry_(4, "csib", CSIB, true, NULL),			\
	_entry_(5, "csic", CSIC, true, NULL),			\
	_entry_(6, "csid", CSID, true, NULL),			\
	_entry_(7, "csie", CSIE, true, NULL),			\
	_entry_(8, "csif", CSIF, true, NULL),			\
	_entry_(9, "dbg", DBG, true, "vddio-dbg"),		\
	_entry_(10, "debug-nonao", DEBUG_NONAO, true, NULL),	\
	_entry_(11, "dmic", DMIC, true, "vddio-dmic"),		\
	_entry_(12, "dp", DP, true, NULL),			\
	_entry_(13, "dsi", DSI, true, NULL),			\
	_entry_(14, "dsib", DSIB, true, NULL),			\
	_entry_(15, "dsic", DSIC, true, NULL),			\
	_entry_(16, "dsid", DSID, true, NULL),			\
	_entry_(17, "emmc", SDMMC4, true, NULL),		\
	_entry_(18, "emmc2", EMMC2, true, NULL),		\
	_entry_(19, "gpio", GPIO, true, "vddio-gpio"),		\
	_entry_(20, "hdmi", HDMI, true, NULL),			\
	_entry_(21, "hsic", HSIC, true, NULL),			\
	_entry_(22, "lvds", LVDS, true, NULL),			\
	_entry_(23, "mipi-bias", MIPI_BIAS, true, NULL),	\
	_entry_(24, "pex-bias", PEX_BIAS, true, NULL),		\
	_entry_(25, "pex-clk1", PEX_CLK1, true, NULL),		\
	_entry_(26, "pex-clk2", PEX_CLK2, true, NULL),		\
	_entry_(27, "pex-ctrl", PEX_CNTRL, false, "vddio-pex-ctrl"), \
	_entry_(28, "sdmmc1", SDMMC1, true, "vddio-sdmmc1"),	\
	_entry_(29, "sdmmc3", SDMMC3, true, "vddio-sdmmc3"),	\
	_entry_(30, "spi", SPI, true, "vddio-spi"),		\
	_entry_(31, "spi-hv", SPI_HV, true, "vddio-spi-hv"),	\
	_entry_(32, "uart", UART, true, "vddio-uart"),		\
	_entry_(33, "usb0", USB0, true, NULL),			\
	_entry_(34, "usb1", USB1, true, NULL),			\
	_entry_(35, "usb2", USB2, true, NULL),			\
	_entry_(36, "usb3", USB3, true, NULL),			\
	_entry_(37, "usb-bias", USB_BIAS, true, NULL)

#define TEGRA_IO_PAD_INFO(_pin, _name, _id, _lpstate, _vsupply)	\
	{							\
		.name = _name,					\
		.pins = {(_pin)},				\
		.id = TEGRA_IO_PAD_##_id,			\
		.vsupply = (_vsupply),				\
		.supports_low_power = (_lpstate),		\
	}

static const struct tegra_io_pads_cfg tegra124_io_pads_cfg_info[] = {
	TEGRA124_PAD_INFO_TABLE(TEGRA_IO_PAD_INFO),
};

static const struct tegra_io_pads_cfg tegra210_io_pads_cfg_info[] = {
	TEGRA210_PAD_INFO_TABLE(TEGRA_IO_PAD_INFO),
};

#define TEGRA_IO_PAD_DESC(_pin, _name, _id, _lpstate, _vsupply)	\
	PINCTRL_PIN(_pin, _name)

static const struct pinctrl_pin_desc tegra124_io_pads_pinctrl_desc[] = {
	TEGRA124_PAD_INFO_TABLE(TEGRA_IO_PAD_DESC),
};

static const struct pinctrl_pin_desc tegra210_io_pads_pinctrl_desc[] = {
	TEGRA210_PAD_INFO_TABLE(TEGRA_IO_PAD_DESC),
};

static const struct tegra_io_pads_soc_data tegra124_io_pad_soc_data = {
	.desc		= tegra124_io_pads_pinctrl_desc,
	.num_desc	= ARRAY_SIZE(tegra124_io_pads_pinctrl_desc),
	.cfg		= tegra124_io_pads_cfg_info,
	.num_cfg	= ARRAY_SIZE(tegra124_io_pads_cfg_info),
};

static const struct tegra_io_pads_soc_data tegra210_io_pad_soc_data = {
	.desc		= tegra210_io_pads_pinctrl_desc,
	.num_desc	= ARRAY_SIZE(tegra210_io_pads_pinctrl_desc),
	.cfg		= tegra210_io_pads_cfg_info,
	.num_cfg	= ARRAY_SIZE(tegra210_io_pads_cfg_info),
};

static const struct platform_device_id tegra_io_pads_dev_id[] = {
	{
		.name = "pinctrl-t124-io-pad",
		.driver_data = (kernel_ulong_t)&tegra124_io_pad_soc_data,
	}, {
		.name = "pinctrl-t210-io-pad",
		.driver_data = (kernel_ulong_t)&tegra210_io_pad_soc_data,
	}, {
	},
};
MODULE_DEVICE_TABLE(platform, tegra_io_pads_dev_id);

static struct platform_driver tegra_io_pads_pinctrl_driver = {
	.driver		= {
		.name	= "pinctrl-tegra-io-pad",
	},
	.probe		= tegra_io_pads_pinctrl_probe,
	.id_table	= tegra_io_pads_dev_id,
};

module_platform_driver(tegra_io_pads_pinctrl_driver);

MODULE_DESCRIPTION("NVIDIA TEGRA IO pad Control Driver");
MODULE_AUTHOR("Laxman Dewangan <ldewangan@nvidia.com>");
MODULE_LICENSE("GPL v2");
