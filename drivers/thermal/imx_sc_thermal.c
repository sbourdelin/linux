// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2018 NXP.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/err.h>
#include <linux/firmware/imx/sci.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>

#include "thermal_core.h"

#define IMX_SC_MISC_FUNC_GET_TEMP	13
#define IMX_SC_C_TEMP			0

struct imx_sc_ipc *thermal_ipc_handle;

struct imx_sc_sensor {
	struct thermal_zone_device *tzd;
	unsigned int resource_id;
};

struct imx_sc_thermal_data {
	struct imx_sc_sensor *sensor;
};

struct imx_sc_msg_req_misc_get_temp {
	struct imx_sc_rpc_msg hdr;
	u16 resource_id;
	u8 type;
} __packed;

struct imx_sc_msg_resp_misc_get_temp {
	struct imx_sc_rpc_msg hdr;
	u16 celsius;
	u8 tenths;
} __packed;

static int imx_sc_thermal_get_temp(void *data, int *temp)
{
	struct imx_sc_msg_resp_misc_get_temp *resp;
	struct imx_sc_msg_req_misc_get_temp msg;
	struct imx_sc_rpc_msg *hdr = &msg.hdr;
	struct imx_sc_sensor *sensor = data;
	int ret;

	msg.resource_id = sensor->resource_id;
	msg.type = IMX_SC_C_TEMP;

	hdr->ver = IMX_SC_RPC_VERSION;
	hdr->svc = IMX_SC_RPC_SVC_MISC;
	hdr->func = IMX_SC_MISC_FUNC_GET_TEMP;
	hdr->size = 2;

	ret = imx_scu_call_rpc(thermal_ipc_handle, &msg, true);
	if (ret) {
		pr_err("read temp sensor %d failed, ret %d\n",
			sensor->resource_id, ret);
		return ret;
	}

	resp = (struct imx_sc_msg_resp_misc_get_temp *)&msg;
	*temp = resp->celsius * 1000 + resp->tenths * 100;

	return 0;
}

static const struct thermal_zone_of_device_ops imx_sc_thermal_ops = {
	.get_temp = imx_sc_thermal_get_temp,
};

static int imx_sc_thermal_register_sensor(struct platform_device *pdev,
					  struct imx_sc_sensor *sensor)
{
	struct thermal_zone_device *tzd;

	tzd = devm_thermal_zone_of_sensor_register(&pdev->dev,
						   sensor->resource_id,
						   sensor,
						   &imx_sc_thermal_ops);
	if (IS_ERR(tzd)) {
		dev_err(&pdev->dev, "failed to register sensor: %d\n",
			sensor->resource_id);
		return -EINVAL;
	}

	sensor->tzd = tzd;

	return 0;
}

static int imx_sc_thermal_get_sensor_id(struct device_node *sensor_np)
{
	struct of_phandle_args sensor_specs;
	int ret;

	ret = of_parse_phandle_with_args(sensor_np, "thermal-sensors",
			"#thermal-sensor-cells",
			0, &sensor_specs);
	if (ret)
		return ret;

	if (sensor_specs.args_count >= 1) {
		ret = sensor_specs.args[0];
		WARN(sensor_specs.args_count > 1,
				"%pOFn: too many cells in sensor specifier %d\n",
				sensor_specs.np, sensor_specs.args_count);
	} else {
		ret = 0;
	}

	return ret;
}

static int imx_sc_thermal_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *sensor_np = NULL;
	struct imx_sc_thermal_data *data;
	struct imx_sc_sensor *sensors;
	u32 sensor_num;
	int ret, i;

	ret = imx_scu_get_handle(&thermal_ipc_handle);
	if (ret) {
		if (ret == -EPROBE_DEFER)
			return ret;

		dev_err(&pdev->dev, "failed to get ipc handle: %d!\n", ret);
		return ret;
	}

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	ret = of_property_read_u32(np, "tsens-num", &sensor_num);
	if (ret || !sensor_num) {
		dev_err(&pdev->dev, "failed to get valid temp sensor number!\n");
		ret = -EINVAL;
		goto free_data;
	}

	sensors = devm_kzalloc(&pdev->dev, sizeof(*data->sensor) * sensor_num,
			       GFP_KERNEL);
	if (!sensors) {
		ret = -ENOMEM;
		goto free_data;
	}

	data->sensor = sensors;

	np = of_find_node_by_name(NULL, "thermal-zones");
	if (!np) {
		ret = -ENODEV;
		goto free_sensors;
	}

	for (i = 0; i < sensor_num; i++) {
		struct imx_sc_sensor *sensor = &data->sensor[i];

		sensor_np = of_get_next_child(np, sensor_np);
		sensor->resource_id = imx_sc_thermal_get_sensor_id(sensor_np);
		if (sensor->resource_id < 0) {
			dev_err(&pdev->dev, "invalid sensor resource id: %d\n",
				sensor->resource_id);
			ret = sensor->resource_id;
			goto put_node;
		}

		ret = imx_sc_thermal_register_sensor(pdev, sensor);
		if (ret) {
			dev_err(&pdev->dev, "failed to register thermal sensor: %d\n",
				ret);
			goto put_node;
		}
	}

	of_node_put(sensor_np);
	of_node_put(np);

	return 0;

put_node:
	of_node_put(np);
	of_node_put(sensor_np);
free_sensors:
	kfree(sensors);
free_data:
	kfree(data);

	return ret;
}

static const struct of_device_id imx_sc_thermal_table[] = {
	{ .compatible = "nxp,imx8qxp-sc-thermal", },
	{},
};
MODULE_DEVICE_TABLE(of, imx_sc_thermal_table);

static struct platform_driver imx_sc_thermal_driver = {
		.probe = imx_sc_thermal_probe,
		.driver = {
			.name = "imx-sc-thermal",
			.of_match_table = imx_sc_thermal_table,
		},
};
module_platform_driver(imx_sc_thermal_driver);

MODULE_AUTHOR("Anson Huang <Anson.Huang@nxp.com>");
MODULE_DESCRIPTION("Thermal driver for NXP i.MX SoCs with system controller");
MODULE_LICENSE("GPL v2");
