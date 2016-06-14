/*
 * Video Camera Capture driver for Freescale i.MX5/6 SOC
 *
 * Open Firmware parsing.
 *
 * Copyright (c) 2016 Mentor Graphics Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <linux/of_platform.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-of.h>
#include <media/v4l2-ctrls.h>
#include <video/imx-ipu-v3.h>
#include "imx-camif.h"

/* parse inputs property from a sensor's upstream sink endpoint node */
static void of_parse_sensor_inputs(struct imxcam_dev *dev,
				   struct device_node *sink_ep,
				   struct imxcam_sensor *sensor)
{
	struct imxcam_sensor_input *sinput = &sensor->input;
	int next_input = dev->num_sensor_inputs;
	int ret, i;

	for (i = 0; i < IMXCAM_MAX_INPUTS; i++) {
		const char *input_name;
		u32 val;

		ret = of_property_read_u32_index(sink_ep, "inputs", i, &val);
		if (ret)
			break;

		sinput->value[i] = val;

		ret = of_property_read_string_index(sink_ep, "input-names", i,
						    &input_name);
		/*
		 * if input-names not provided, they will be set using
		 * the subdev name once the subdev is known during
		 * async bind
		 */
		if (!ret)
			strncpy(sinput->name[i], input_name,
				sizeof(sinput->name[i]));

		val = 0;
		ret = of_property_read_u32_index(sink_ep, "input-caps",
						 i, &val);
		sinput->caps[i] = val;
	}

	sinput->num = i;

	/* if no inputs provided just assume a single input */
	if (sinput->num == 0) {
		sinput->num = 1;
		sinput->caps[0] = 0;
	}

	sinput->first = next_input;
	sinput->last = next_input + sinput->num - 1;

	dev->num_sensor_inputs = sinput->last + 1;
}

static int of_parse_sensor(struct imxcam_dev *dev,
			   struct imxcam_sensor *sensor,
			   struct device_node *sink_ep,
			   struct device_node *csi_port,
			   struct device_node *sensor_node)
{
	struct device_node *sensor_ep, *csi_ep;

	sensor_ep = of_graph_get_next_endpoint(sensor_node, NULL);
	if (!sensor_ep)
		return -EINVAL;
	csi_ep = of_get_next_child(csi_port, NULL);
	if (!csi_ep) {
		of_node_put(sensor_ep);
		return -EINVAL;
	}

	sensor->csi_np = csi_port;

	v4l2_of_parse_endpoint(sensor_ep, &sensor->ep);
	v4l2_of_parse_endpoint(csi_ep, &sensor->csi_ep);

	of_parse_sensor_inputs(dev, sink_ep, sensor);

	of_node_put(sensor_ep);
	of_node_put(csi_ep);
	return 0;
}

static struct v4l2_async_subdev *add_async_subdev(struct imxcam_dev *dev,
						  struct device_node *np)
{
	struct v4l2_async_subdev *asd;
	int asd_idx;

	asd_idx = dev->subdev_notifier.num_subdevs;
	if (asd_idx >= IMXCAM_MAX_SUBDEVS)
		return ERR_PTR(-ENOSPC);

	asd = &dev->async_desc[asd_idx];
	dev->async_ptrs[asd_idx] = asd;

	asd->match_type = V4L2_ASYNC_MATCH_OF;
	asd->match.of.node = np;
	dev->subdev_notifier.num_subdevs++;

	dev_dbg(dev->dev, "%s: added %s, num %d, node %p\n",
		__func__, np->name, dev->subdev_notifier.num_subdevs, np);

	return asd;
}

/* Discover all the subdevices we need downstream from a sink endpoint */
static int of_discover_subdevs(struct imxcam_dev *dev,
			       struct device_node *csi_port,
			       struct device_node *sink_ep,
			       int *vidmux_input)
{
	struct device_node *rpp, *epnode = NULL;
	struct v4l2_async_subdev *asd;
	struct imxcam_sensor *sensor;
	int sensor_idx, num_sink_ports;
	int i, vidmux_idx = -1, ret = 0;

	rpp = of_graph_get_remote_port_parent(sink_ep);
	if (!rpp)
		return 0;
	if (!of_device_is_available(rpp))
		goto out;

	asd = add_async_subdev(dev, rpp);
	if (IS_ERR(asd)) {
		ret = PTR_ERR(asd);
		goto out;
	}

	if (of_device_is_compatible(rpp, "fsl,imx-mipi-csi2")) {
		/*
		 * there is only one internal mipi receiver, so exit
		 * with 0 if we've already passed through here
		 */
		if (dev->csi2_asd) {
			dev->subdev_notifier.num_subdevs--;
			ret = 0;
			goto out;
		}

		/* the mipi csi2 receiver has only one sink port */
		num_sink_ports = 1;
		dev->csi2_asd = asd;
		dev_dbg(dev->dev, "found mipi-csi2 %s\n", rpp->name);
	} else if (of_device_is_compatible(rpp, "imx-video-mux")) {
		/* for the video mux, all but the last port are sinks */
		num_sink_ports = of_get_child_count(rpp) - 1;

		vidmux_idx = dev->num_vidmux;
		if (vidmux_idx >= IMXCAM_MAX_VIDEOMUX) {
			ret = -ENOSPC;
			goto out;
		}

		dev->vidmux_asd[vidmux_idx] = asd;
		dev->num_vidmux++;
		dev_dbg(dev->dev, "found video mux %s\n", rpp->name);
	} else {
		/* this rpp must be a sensor, it has no sink ports */
		num_sink_ports = 0;

		sensor_idx = dev->num_sensors;
		if (sensor_idx >= IMXCAM_MAX_SENSORS)
			return -ENOSPC;

		sensor = &dev->sensor_list[sensor_idx];

		ret = of_parse_sensor(dev, sensor, sink_ep, csi_port, rpp);
		if (ret)
			goto out;

		/*
		 * save the input indeces of all video-muxes recorded in
		 * this pipeline path required to receive data from this
		 * sensor.
		 */
		memcpy(sensor->vidmux_input, vidmux_input,
		       sizeof(sensor->vidmux_input));

		sensor->asd = asd;
		dev->num_sensors++;
		dev_dbg(dev->dev, "found sensor %s\n", rpp->name);
	}

	/* continue discovery downstream */
	dev_dbg(dev->dev, "scanning %d sink ports on %s\n",
		num_sink_ports, rpp->name);

	for (i = 0; i < num_sink_ports; i++) {
		epnode = of_graph_get_next_endpoint(rpp, epnode);
		if (!epnode) {
			v4l2_err(&dev->v4l2_dev,
				 "no endpoint at port %d on %s\n",
				 i, rpp->name);
			ret = -EINVAL;
			break;
		}

		if (vidmux_idx >= 0)
			vidmux_input[vidmux_idx] = i;

		ret = of_discover_subdevs(dev, csi_port, epnode, vidmux_input);
		of_node_put(epnode);
		if (ret)
			break;
	}

out:
	of_node_put(rpp);
	return ret;
}

static int of_parse_ports(struct imxcam_dev *dev, struct device_node *np)
{
	struct device_node *port, *epnode;
	struct v4l2_async_subdev *asd;
	int vidmux_inputs[IMXCAM_MAX_VIDEOMUX];
	int i, j, csi_idx, ret = 0;

	for (i = 0; ; i++) {
		port = of_parse_phandle(np, "ports", i);
		if (!port) {
			ret = 0;
			break;
		}

		csi_idx = dev->num_csi;
		if (csi_idx >= IMXCAM_MAX_CSI) {
			ret = -ENOSPC;
			break;
		}
		/* register the CSI subdev */
		asd = add_async_subdev(dev, port);
		if (IS_ERR(asd)) {
			ret = PTR_ERR(asd);
			break;
		}
		dev->csi_asd[csi_idx] = asd;
		dev->num_csi++;

		/*
		 * discover and register all async subdevs downstream
		 * from this CSI port.
		 */
		for_each_child_of_node(port, epnode) {
			for (j = 0; j < IMXCAM_MAX_VIDEOMUX; j++)
				vidmux_inputs[j] = -1;

			ret = of_discover_subdevs(dev, port, epnode,
						  vidmux_inputs);
			of_node_put(epnode);
			if (ret)
				break;
		}

		of_node_put(port);
		if (ret)
			break;
	}

	if (ret)
		return ret;

	if (!dev->num_sensors) {
		v4l2_err(&dev->v4l2_dev, "no sensors found!\n");
		return -EINVAL;
	}

	return 0;
}

static int of_parse_fim(struct imxcam_dev *dev, struct device_node *np)
{
	struct imxcam_fim *fim = &dev->fim;
	struct device_node *fim_np;
	u32 val, tol[2], icap[2];
	int ret;

	fim_np = of_get_child_by_name(np, "fim");
	if (!fim_np) {
		/* set to the default defaults */
		fim->of_defaults[FIM_CL_ENABLE] = FIM_CL_ENABLE_DEF;
		fim->of_defaults[FIM_CL_NUM] = FIM_CL_NUM_DEF;
		fim->of_defaults[FIM_CL_NUM_SKIP] = FIM_CL_NUM_SKIP_DEF;
		fim->of_defaults[FIM_CL_TOLERANCE_MIN] =
			FIM_CL_TOLERANCE_MIN_DEF;
		fim->of_defaults[FIM_CL_TOLERANCE_MAX] =
			FIM_CL_TOLERANCE_MAX_DEF;
		fim->icap_channel = -1;
		return 0;
	}

	ret = of_property_read_u32(fim_np, "enable", &val);
	if (ret)
		val = FIM_CL_ENABLE_DEF;
	fim->of_defaults[FIM_CL_ENABLE] = val;

	ret = of_property_read_u32(fim_np, "num-avg", &val);
	if (ret)
		val = FIM_CL_NUM_DEF;
	fim->of_defaults[FIM_CL_NUM] = val;

	ret = of_property_read_u32(fim_np, "num-skip", &val);
	if (ret)
		val = FIM_CL_NUM_SKIP_DEF;
	fim->of_defaults[FIM_CL_NUM_SKIP] = val;

	ret = of_property_read_u32_array(fim_np, "tolerance-range", tol, 2);
	if (ret) {
		tol[0] = FIM_CL_TOLERANCE_MIN_DEF;
		tol[1] = FIM_CL_TOLERANCE_MAX_DEF;
	}
	fim->of_defaults[FIM_CL_TOLERANCE_MIN] = tol[0];
	fim->of_defaults[FIM_CL_TOLERANCE_MAX] = tol[1];

	ret = of_property_read_u32_array(fim_np, "input-capture-channel",
					 icap, 2);
	if (!ret) {
		fim->icap_channel = icap[0];
		fim->icap_flags = icap[1];
	} else {
		fim->icap_channel = -1;
	}

	of_node_put(fim_np);
	return 0;
}

int imxcam_of_parse(struct imxcam_dev *dev, struct device_node *np)
{
	int ret = of_parse_fim(dev, np);
	if (ret)
		return ret;

	return of_parse_ports(dev, np);
}
