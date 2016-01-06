/*
 * Copyright (C) 2014 Atmel
 *		      Bo Shen <voice.shen@atmel.com>
 *
 * Authors:	      Bo Shen <voice.shen@atmel.com>
 *		      Boris Brezillon <boris.brezillon@free-electrons.com>
 *		      Wu, Songjun <Songjun.Wu@atmel.com>
 *
 *
 * Copyright (C) 2010-2011 Freescale Semiconductor, Inc. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/component.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder_slave.h>

#define SIL902X_TPI_VIDEO_DATA			0x0

#define SIL902X_TPI_PIXEL_REPETITION		0x8
#define SIL902X_TPI_AVI_PIXEL_REP_BUS_24BIT     BIT(5)
#define SIL902X_TPI_AVI_PIXEL_REP_RISING_EDGE   BIT(4)
#define SIL902X_TPI_AVI_PIXEL_REP_4X		3
#define SIL902X_TPI_AVI_PIXEL_REP_2X		1
#define SIL902X_TPI_AVI_PIXEL_REP_NONE		0
#define SIL902X_TPI_CLK_RATIO_HALF		(0 << 6)
#define SIL902X_TPI_CLK_RATIO_1X		(1 << 6)
#define SIL902X_TPI_CLK_RATIO_2X		(2 << 6)
#define SIL902X_TPI_CLK_RATIO_4X		(3 << 6)

#define SIL902X_TPI_AVI_IN_FORMAT		0x9
#define SIL902X_TPI_AVI_INPUT_BITMODE_12BIT	BIT(7)
#define SIL902X_TPI_AVI_INPUT_DITHER		BIT(6)
#define SIL902X_TPI_AVI_INPUT_RANGE_LIMITED	(2 << 2)
#define SIL902X_TPI_AVI_INPUT_RANGE_FULL	(1 << 2)
#define SIL902X_TPI_AVI_INPUT_RANGE_AUTO	(0 << 2)
#define SIL902X_TPI_AVI_INPUT_COLORSPACE_BLACK	(3 << 0)
#define SIL902X_TPI_AVI_INPUT_COLORSPACE_YUV422	(2 << 0)
#define SIL902X_TPI_AVI_INPUT_COLORSPACE_YUV444	(1 << 0)
#define SIL902X_TPI_AVI_INPUT_COLORSPACE_RGB	(0 << 0)

#define SIL902X_TPI_AVI_INFOFRAME		0x0c

#define SIL902X_SYS_CTRL_DATA			0x1a
#define SIL902X_SYS_CTRL_PWR_DWN		BIT(4)
#define SIL902X_SYS_CTRL_AV_MUTE		BIT(3)
#define SIL902X_SYS_CTRL_DDC_BUS_REQ		BIT(2)
#define SIL902X_SYS_CTRL_DDC_BUS_GRTD		BIT(1)
#define SIL902X_SYS_CTRL_OUTPUT_MODE		BIT(0)
#define SIL902X_SYS_CTRL_OUTPUT_HDMI		1
#define SIL902X_SYS_CTRL_OUTPUT_DVI		0

#define SIL902X_REG_CHIPID(n)			(0x1b + (n))

#define SIL902X_PWR_STATE_CTRL			0x1e
#define SIL902X_AVI_POWER_STATE_MSK		GENMASK(1, 0)
#define SIL902X_AVI_POWER_STATE_D(l)		((l) & SIL902X_AVI_POWER_STATE_MSK)

#define SI902X_INT_ENABLE			0x3c
#define SI902X_INT_STATUS			0x3d
#define SI902X_HOTPLUG_EVENT			BIT(0)
#define SI902X_PLUGGED_STATUS			BIT(2)

#define SIL902X_REG_TPI_RQB			0xc7

struct sil902x {
	struct i2c_client *i2c;
	struct regmap *regmap;
	struct drm_bridge bridge;
	struct drm_connector connector;
	struct gpio_desc *reset_gpio;
	struct work_struct hotplug_work;
};

static inline struct sil902x *bridge_to_sil902x(struct drm_bridge *bridge)
{
	return container_of(bridge, struct sil902x, bridge);
}

static inline struct sil902x *connector_to_sil902x(struct drm_connector *con)
{
	return container_of(con, struct sil902x, connector);
}

static void sil902x_reset(struct sil902x *sil902x)
{
	gpiod_set_value(sil902x->reset_gpio, 1);

	msleep(100);

	gpiod_set_value(sil902x->reset_gpio, 0);
}

static void sil902x_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static enum drm_connector_status
sil902x_connector_detect(struct drm_connector *connector, bool force)
{
	struct sil902x *sil902x = connector_to_sil902x(connector);
	unsigned int status;

	regmap_read(sil902x->regmap, SI902X_INT_STATUS, &status);

	return (status & SI902X_PLUGGED_STATUS) ?
	       connector_status_connected : connector_status_disconnected;
}

static const struct drm_connector_funcs sil902x_atomic_connector_funcs = {
	.dpms = drm_atomic_helper_connector_dpms,
	.detect = sil902x_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = sil902x_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_funcs sil902x_connector_funcs = {
	.dpms = drm_atomic_helper_connector_dpms,
	.detect = sil902x_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = sil902x_connector_destroy,
};

static int sil902x_get_modes(struct drm_connector *connector)
{
	struct sil902x *sil902x = connector_to_sil902x(connector);
	struct regmap *regmap = sil902x->regmap;
	u32 bus_format = MEDIA_BUS_FMT_RGB888_1X24;
	unsigned int status;
	struct edid *edid;
	int num = 0;
	int ret;
	int i;

	ret = regmap_update_bits(regmap, SIL902X_PWR_STATE_CTRL,
				 SIL902X_AVI_POWER_STATE_MSK,
				 SIL902X_AVI_POWER_STATE_D(2));
	if (ret)
		return ret;

	ret = regmap_write(regmap, SIL902X_SYS_CTRL_DATA,
			   SIL902X_SYS_CTRL_OUTPUT_HDMI |
			   SIL902X_SYS_CTRL_PWR_DWN);
	if (ret)
		return ret;

	ret = regmap_update_bits(regmap, SIL902X_SYS_CTRL_DATA,
				 SIL902X_SYS_CTRL_DDC_BUS_REQ,
				 SIL902X_SYS_CTRL_DDC_BUS_REQ);
	if (ret)
		return ret;

	i = 0;
	do {
		ret = regmap_read(regmap, SIL902X_SYS_CTRL_DATA, &status);
		if (ret)
			return ret;
		i++;
	} while (!(status & SIL902X_SYS_CTRL_DDC_BUS_GRTD));

	ret = regmap_write(regmap, SIL902X_SYS_CTRL_DATA, status);
	if (ret)
		return ret;

	edid = drm_get_edid(connector, sil902x->i2c->adapter);
	drm_mode_connector_update_edid_property(connector, edid);
	if (edid) {
		num += drm_add_edid_modes(connector, edid);
		kfree(edid);
	}

	ret = drm_display_info_set_bus_formats(&connector->display_info,
					       &bus_format, 1);
	if (ret)
		return ret;

	regmap_read(regmap, SIL902X_SYS_CTRL_DATA, &status);
	if (ret)
		return ret;

	ret = regmap_update_bits(regmap, SIL902X_SYS_CTRL_DATA,
				 SIL902X_SYS_CTRL_DDC_BUS_REQ |
				 SIL902X_SYS_CTRL_DDC_BUS_GRTD, 0);
	if (ret)
		return ret;

	i = 0;
	do {
		ret = regmap_read(regmap, SIL902X_SYS_CTRL_DATA, &status);
		if (ret)
			return ret;
		i++;
	} while (status & (SIL902X_SYS_CTRL_DDC_BUS_REQ |
			   SIL902X_SYS_CTRL_DDC_BUS_GRTD));

	return num;
}

static enum drm_mode_status sil902x_mode_valid(struct drm_connector *connector,
					       struct drm_display_mode *mode)
{
	/* TODO: check mode */

	return MODE_OK;
}

static struct drm_encoder *sil902x_best_encoder(struct drm_connector *connector)
{
	struct sil902x *sil902x = connector_to_sil902x(connector);

	return sil902x->bridge.encoder;
}

static const struct drm_connector_helper_funcs sil902x_connector_helper_funcs = {
	.get_modes = sil902x_get_modes,
	.mode_valid = sil902x_mode_valid,
	.best_encoder = sil902x_best_encoder,
};

static void sil902x_bridge_disable(struct drm_bridge *bridge)
{
	struct sil902x *sil902x = bridge_to_sil902x(bridge);

	regmap_update_bits(sil902x->regmap, SIL902X_SYS_CTRL_DATA,
			   SIL902X_SYS_CTRL_PWR_DWN,
			   SIL902X_SYS_CTRL_PWR_DWN);
}

static void sil902x_bridge_enable(struct drm_bridge *bridge)
{
	struct sil902x *sil902x = bridge_to_sil902x(bridge);

	regmap_update_bits(sil902x->regmap, SIL902X_PWR_STATE_CTRL,
			   SIL902X_AVI_POWER_STATE_MSK,
			   SIL902X_AVI_POWER_STATE_D(0));
	regmap_update_bits(sil902x->regmap, SIL902X_SYS_CTRL_DATA,
			   SIL902X_SYS_CTRL_PWR_DWN, 0);
}

static void sil902x_bridge_mode_set(struct drm_bridge *bridge,
				    struct drm_display_mode *mode,
				    struct drm_display_mode *adj)
{
	u8 buf[HDMI_INFOFRAME_HEADER_SIZE + HDMI_AVI_INFOFRAME_SIZE];
	struct sil902x *sil902x = bridge_to_sil902x(bridge);
	struct regmap *regmap = sil902x->regmap;
	struct hdmi_avi_infoframe frame;
	int ret;

	buf[0] = adj->clock;
	buf[1] = adj->clock >> 8;
	buf[2] = adj->vrefresh;
	buf[3] = 0x00;
	buf[4] = adj->hdisplay;
	buf[5] = adj->hdisplay >> 8;
	buf[6] = adj->vdisplay;
	buf[7] = adj->vdisplay >> 8;
	buf[8] = SIL902X_TPI_CLK_RATIO_1X | SIL902X_TPI_AVI_PIXEL_REP_NONE |
		 SIL902X_TPI_AVI_PIXEL_REP_BUS_24BIT;
	buf[9] = SIL902X_TPI_AVI_INPUT_RANGE_AUTO |
		 SIL902X_TPI_AVI_INPUT_COLORSPACE_RGB;

	ret = regmap_bulk_write(regmap, SIL902X_TPI_VIDEO_DATA, buf, 10);
	if (ret)
		return;

	ret = drm_hdmi_avi_infoframe_from_display_mode(&frame, adj);
	if (ret < 0) {
		DRM_ERROR("couldn't fill AVI infoframe\n");
		return;
	}

	ret = hdmi_avi_infoframe_pack(&frame, buf, sizeof(buf));
	if (ret < 0) {
		DRM_ERROR("failed to pack AVI infoframe: %zd\n", ret);
		return;
	}

	/* Do not send the infoframe header, but keep the CRC field. */
	regmap_bulk_write(regmap, SIL902X_TPI_AVI_INFOFRAME,
			  buf + HDMI_INFOFRAME_HEADER_SIZE - 1,
			  HDMI_AVI_INFOFRAME_SIZE + 1);
}

static int sil902x_bridge_attach(struct drm_bridge *bridge)
{
	const struct drm_connector_funcs *funcs = &sil902x_connector_funcs;
	struct sil902x *sil902x = bridge_to_sil902x(bridge);
	struct drm_device *drm = bridge->dev;
	int ret;

	drm_connector_helper_add(&sil902x->connector,
				 &sil902x_connector_helper_funcs);

	if (drm_core_check_feature(drm, DRIVER_ATOMIC))
		funcs = &sil902x_atomic_connector_funcs;

	ret = drm_connector_init(drm, &sil902x->connector, funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ret;

	if (sil902x->i2c->irq > 0)
		sil902x->connector.polled = DRM_CONNECTOR_POLL_HPD;
	else
		sil902x->connector.polled = DRM_CONNECTOR_POLL_CONNECT;

	drm_mode_connector_attach_encoder(&sil902x->connector, bridge->encoder);

	return 0;
}

static void sil902x_bridge_nop(struct drm_bridge *bridge)
{
}

static const struct drm_bridge_funcs sil902x_bridge_funcs = {
	.attach = sil902x_bridge_attach,
	.mode_set = sil902x_bridge_mode_set,
	.disable = sil902x_bridge_disable,
	.post_disable = sil902x_bridge_nop,
	.pre_enable = sil902x_bridge_nop,
	.enable = sil902x_bridge_enable,
};

static const struct regmap_range sil902x_volatile_ranges[] = {
	{ .range_min = 0, .range_max = 0xff },
};

static const struct regmap_access_table sil902x_volatile_table = {
	.yes_ranges = sil902x_volatile_ranges,
	.n_yes_ranges = ARRAY_SIZE(sil902x_volatile_ranges),
};

static const struct regmap_config sil902x_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.volatile_table = &sil902x_volatile_table,
	.cache_type = REGCACHE_NONE,
};

static irqreturn_t sil902x_interrupt(int irq, void *data)
{
	struct sil902x *sil902x = data;
	unsigned int status = 0;

	regmap_read(sil902x->regmap, SI902X_INT_STATUS, &status);
	regmap_write(sil902x->regmap, SI902X_INT_STATUS, status);

	if ((status & SI902X_HOTPLUG_EVENT) && sil902x->bridge.dev)
		drm_helper_hpd_irq_event(sil902x->bridge.dev);

	return IRQ_HANDLED;
}

static int sil902x_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	unsigned int status = 0;
	struct sil902x *sil902x;
	u8 chipid[4];
	int ret;

	sil902x = devm_kzalloc(dev, sizeof(*sil902x), GFP_KERNEL);
	if (!sil902x)
		return -ENOMEM;

	sil902x->i2c = client;
	sil902x->regmap = devm_regmap_init_i2c(client, &sil902x_regmap_config);
	if (IS_ERR(sil902x->regmap))
		return PTR_ERR(sil902x->regmap);

	sil902x->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(sil902x->reset_gpio)) {
		dev_err(dev, "Failed to retrieve/request reset gpio: %d\n",
			PTR_ERR(sil902x->reset_gpio));
		return PTR_ERR(sil902x->reset_gpio);
	}

	sil902x_reset(sil902x);

	ret = regmap_write(sil902x->regmap, SIL902X_REG_TPI_RQB, 0x0);
	if (ret)
		return ret;

	ret = regmap_bulk_read(sil902x->regmap, SIL902X_REG_CHIPID(0),
			       &chipid, 4);
	if (ret) {
		dev_err(dev, "regmap_read failed %d\n", ret);
		return ret;
	}

	if (chipid[0] != 0xb0) {
		dev_err(dev, "Invalid chipid: %02x (expecting 0xb0)\n",
			chipid[0]);
		return -EINVAL;
	}

	/* Clear all pending interrupts */
	regmap_read(sil902x->regmap, SI902X_INT_STATUS, &status);
	regmap_write(sil902x->regmap, SI902X_INT_STATUS, status);

	if (client->irq > 0) {
		regmap_write(sil902x->regmap, SI902X_INT_ENABLE,
			     SI902X_HOTPLUG_EVENT);

		ret = devm_request_threaded_irq(dev, client->irq, NULL,
						sil902x_interrupt,
						IRQF_ONESHOT, dev_name(dev),
						sil902x);
		if (ret)
			return ret;
	}

	sil902x->bridge.funcs = &sil902x_bridge_funcs;
	sil902x->bridge.of_node = dev->of_node;
	ret = drm_bridge_add(&sil902x->bridge);
	if (ret) {
		dev_err(dev, "Failed to add drm_bridge\n");
		return ret;
	}

	i2c_set_clientdata(client, sil902x);

	return 0;
}

static int sil902x_remove(struct i2c_client *client)

{
	struct sil902x *sil902x = i2c_get_clientdata(client);

	drm_bridge_remove(&sil902x->bridge);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id sil902x_dt_ids[] = {
	{ .compatible = "sil,sil9022", },
	{ }
};
MODULE_DEVICE_TABLE(of, sil902x_dt_ids);
#endif

static const struct i2c_device_id sil902x_i2c_ids[] = {
	{ "sil9022", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, sil9022_id);

static struct i2c_driver sil902x_driver = {
	.probe = sil902x_probe,
	.remove = sil902x_remove,
	.driver = {
		.name = "sil902x",
		.of_match_table = of_match_ptr(sil902x_dt_ids),
	},
	.id_table = sil902x_i2c_ids,
};
module_i2c_driver(sil902x_driver);

MODULE_AUTHOR("Boris Brezillon <boris.brezillon@free-electrons.com>");
MODULE_DESCRIPTION("SIL902x RGB -> HDMI bridges");
MODULE_LICENSE("GPL");
