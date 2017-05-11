/*
 * Copyright © 2016-2017 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Portions of this file (derived from panel-simple.c) are:
 *
 * Copyright (C) 2013, NVIDIA Corporation.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * Raspberry Pi 7" touchscreen panel bridge driver.
 *
 * The 7" touchscreen consists of a DPI LCD panel, a Toshiba
 * TC358762XBG DSI-DPI bridge, and an I2C-connected Atmel ATTINY88-MUR
 * controlling power management, the LCD PWM, and initial register
 * setup of the Tohsiba.
 *
 * This driver controls the TC358762 and ATTINY88, bridging between
 * the DSI host and the LCD panel.  The panel-simple driver has the
 * actual panel.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/pm.h>

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>

/* I2C registers of the Atmel microcontroller. */
enum REG_ADDR {
	REG_ID = 0x80,
	REG_PORTA, /* BIT(2) for horizontal flip, BIT(3) for vertical flip */
	REG_PORTB,
	REG_PORTC,
	REG_PORTD,
	REG_POWERON,
	REG_PWM,
	REG_DDRA,
	REG_DDRB,
	REG_DDRC,
	REG_DDRD,
	REG_TEST,
	REG_WR_ADDRL,
	REG_WR_ADDRH,
	REG_READH,
	REG_READL,
	REG_WRITEH,
	REG_WRITEL,
	REG_ID2,
};

/* We only turn the PWM on or off, without varying values. */
#define RPI_TOUCHSCREEN_MAX_BRIGHTNESS 1

/* DSI D-PHY Layer Registers */
#define D0W_DPHYCONTTX		0x0004
#define CLW_DPHYCONTRX		0x0020
#define D0W_DPHYCONTRX		0x0024
#define D1W_DPHYCONTRX		0x0028
#define COM_DPHYCONTRX		0x0038
#define CLW_CNTRL		0x0040
#define D0W_CNTRL		0x0044
#define D1W_CNTRL		0x0048
#define DFTMODE_CNTRL		0x0054

/* DSI PPI Layer Registers */
#define PPI_STARTPPI		0x0104
#define PPI_BUSYPPI		0x0108
#define PPI_LINEINITCNT		0x0110
#define PPI_LPTXTIMECNT		0x0114
#define PPI_LANEENABLE		0x0134
#define PPI_TX_RX_TA		0x013C
#define PPI_CLS_ATMR		0x0140
#define PPI_D0S_ATMR		0x0144
#define PPI_D1S_ATMR		0x0148
#define PPI_D0S_CLRSIPOCOUNT	0x0164
#define PPI_D1S_CLRSIPOCOUNT	0x0168
#define CLS_PRE			0x0180
#define D0S_PRE			0x0184
#define D1S_PRE			0x0188
#define CLS_PREP		0x01A0
#define D0S_PREP		0x01A4
#define D1S_PREP		0x01A8
#define CLS_ZERO		0x01C0
#define D0S_ZERO		0x01C4
#define D1S_ZERO		0x01C8
#define PPI_CLRFLG		0x01E0
#define PPI_CLRSIPO		0x01E4
#define HSTIMEOUT		0x01F0
#define HSTIMEOUTENABLE		0x01F4

/* DSI Protocol Layer Registers */
#define DSI_STARTDSI		0x0204
#define DSI_BUSYDSI		0x0208
#define DSI_LANEENABLE		0x0210
# define DSI_LANEENABLE_CLOCK		BIT(0)
# define DSI_LANEENABLE_D0		BIT(1)
# define DSI_LANEENABLE_D1		BIT(2)

#define DSI_LANESTATUS0		0x0214
#define DSI_LANESTATUS1		0x0218
#define DSI_INTSTATUS		0x0220
#define DSI_INTMASK		0x0224
#define DSI_INTCLR		0x0228
#define DSI_LPTXTO		0x0230
#define DSI_MODE		0x0260
#define DSI_PAYLOAD0		0x0268
#define DSI_PAYLOAD1		0x026C
#define DSI_SHORTPKTDAT		0x0270
#define DSI_SHORTPKTREQ		0x0274
#define DSI_BTASTA		0x0278
#define DSI_BTACLR		0x027C

/* DSI General Registers */
#define DSIERRCNT		0x0300
#define DSISIGMOD		0x0304

/* DSI Application Layer Registers */
#define APLCTRL			0x0400
#define APLSTAT			0x0404
#define APLERR			0x0408
#define PWRMOD			0x040C
#define RDPKTLN			0x0410
#define PXLFMT			0x0414
#define MEMWRCMD		0x0418

/* LCDC/DPI Host Registers */
#define LCDCTRL			0x0420
#define HSR			0x0424
#define HDISPR			0x0428
#define VSR			0x042C
#define VDISPR			0x0430
#define VFUEN			0x0434

/* DBI-B Host Registers */
#define DBIBCTRL		0x0440

/* SPI Master Registers */
#define SPICMR			0x0450
#define SPITCR			0x0454

/* System Controller Registers */
#define SYSSTAT			0x0460
#define SYSCTRL			0x0464
#define SYSPLL1			0x0468
#define SYSPLL2			0x046C
#define SYSPLL3			0x0470
#define SYSPMCTRL		0x047C

/* GPIO Registers */
#define GPIOC			0x0480
#define GPIOO			0x0484
#define GPIOI			0x0488

/* I2C Registers */
#define I2CCLKCTRL		0x0490

/* Chip/Rev Registers */
#define IDREG			0x04A0

/* Debug Registers */
#define WCMDQUEUE		0x0500
#define RCMDQUEUE		0x0504

struct rpi_touchscreen {
	struct drm_bridge base;
	struct drm_bridge *panel_bridge;
	struct mipi_dsi_device *dsi;
	struct i2c_client *bridge_i2c;
};

static struct rpi_touchscreen *bridge_to_ts(struct drm_bridge *bridge)
{
	return container_of(bridge, struct rpi_touchscreen, base);
}

static u8 rpi_touchscreen_i2c_read(struct rpi_touchscreen *ts, u8 reg)
{
	return i2c_smbus_read_byte_data(ts->bridge_i2c, reg);
}

static void rpi_touchscreen_i2c_write(struct rpi_touchscreen *ts,
				      u8 reg, u8 val)
{
	int ret;

	ret = i2c_smbus_write_byte_data(ts->bridge_i2c, reg, val);
	if (ret)
		dev_err(&ts->dsi->dev, "I2C write failed: %d\n", ret);
}

static int rpi_touchscreen_write(struct rpi_touchscreen *ts, u16 reg, u32 val)
{
#if 0
	/* The firmware uses LP DSI transactions like this to bring up
	 * the hardware, which should be faster than using I2C to then
	 * pass to the Toshiba.  However, I was unable to get it to
	 * work.
	 */
	u8 msg[] = {
		reg,
		reg >> 8,
		val,
		val >> 8,
		val >> 16,
		val >> 24,
	};

	mipi_dsi_dcs_write_buffer(ts->dsi, msg, sizeof(msg));
#else
	rpi_touchscreen_i2c_write(ts, REG_WR_ADDRH, reg >> 8);
	rpi_touchscreen_i2c_write(ts, REG_WR_ADDRL, reg);
	rpi_touchscreen_i2c_write(ts, REG_WRITEH, val >> 8);
	rpi_touchscreen_i2c_write(ts, REG_WRITEL, val);
#endif

	return 0;
}

static void rpi_bridge_disable(struct drm_bridge *bridge)
{
	struct rpi_touchscreen *ts = bridge_to_ts(bridge);

	rpi_touchscreen_i2c_write(ts, REG_PWM, 0);

	rpi_touchscreen_i2c_write(ts, REG_POWERON, 0);
	udelay(1);
}

static void rpi_bridge_enable(struct drm_bridge *bridge)
{
	struct rpi_touchscreen *ts = bridge_to_ts(bridge);
	int i;

	rpi_touchscreen_i2c_write(ts, REG_POWERON, 1);
	/* Wait for nPWRDWN to go low to indicate poweron is done. */
	for (i = 0; i < 100; i++) {
		if (rpi_touchscreen_i2c_read(ts, REG_PORTB) & 1)
			break;
	}

	rpi_touchscreen_write(ts, DSI_LANEENABLE,
			      DSI_LANEENABLE_CLOCK |
			      DSI_LANEENABLE_D0);
	rpi_touchscreen_write(ts, PPI_D0S_CLRSIPOCOUNT, 0x05);
	rpi_touchscreen_write(ts, PPI_D1S_CLRSIPOCOUNT, 0x05);
	rpi_touchscreen_write(ts, PPI_D0S_ATMR, 0x00);
	rpi_touchscreen_write(ts, PPI_D1S_ATMR, 0x00);
	rpi_touchscreen_write(ts, PPI_LPTXTIMECNT, 0x03);

	rpi_touchscreen_write(ts, SPICMR, 0x00);
	rpi_touchscreen_write(ts, LCDCTRL, 0x00100150);
	rpi_touchscreen_write(ts, SYSCTRL, 0x040f);
	msleep(100);

	rpi_touchscreen_write(ts, PPI_STARTPPI, 0x01);
	rpi_touchscreen_write(ts, DSI_STARTDSI, 0x01);
	msleep(100);

	/* Turn on the backlight. */
	rpi_touchscreen_i2c_write(ts, REG_PWM, 255);

	/* Default to the same orientation as the closed source
	 * firmware used for the panel.  Runtime rotation
	 * configuration will be supported using VC4's plane
	 * orientation bits.
	 */
	rpi_touchscreen_i2c_write(ts, REG_PORTA, BIT(2));
}

static int rpi_bridge_attach(struct drm_bridge *bridge)
{
	struct rpi_touchscreen *ts = bridge_to_ts(bridge);

	return drm_bridge_attach(bridge->encoder, ts->panel_bridge, bridge);
}

static struct drm_bridge_funcs rpi_bridge_funcs = {
	.enable = rpi_bridge_enable,
	.disable = rpi_bridge_disable,
	.attach = rpi_bridge_attach,
};

static struct i2c_client *rpi_touchscreen_get_i2c(struct device *dev,
						  const char *name)
{
	struct device_node *node;
	struct i2c_client *client;

	node = of_parse_phandle(dev->of_node, name, 0);
	if (!node)
		return ERR_PTR(-ENODEV);

	client = of_find_i2c_device_by_node(node);
	of_node_put(node);
	if (!client)
		return ERR_PTR(-ENODEV);

	return client;
}

static int rpi_touchscreen_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct drm_panel *panel;
	struct rpi_touchscreen *ts;
	int ret, ver;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	dev_set_drvdata(dev, ts);

	ts->dsi = dsi;
	dsi->mode_flags = (MIPI_DSI_MODE_VIDEO |
			   MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			   MIPI_DSI_MODE_LPM);
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 1;

	ts->bridge_i2c =
		rpi_touchscreen_get_i2c(dev, "raspberrypi,touchscreen-bridge");
	if (IS_ERR(ts->bridge_i2c)) {
		ret = -EPROBE_DEFER;
		return ret;
	}

	ver = rpi_touchscreen_i2c_read(ts, REG_ID);
	if (ver < 0) {
		dev_err(dev, "Atmel I2C read failed: %d\n", ver);
		return -ENODEV;
	}

	switch (ver) {
	case 0xde: /* ver 1 */
	case 0xc3: /* ver 2 */
		break;
	default:
		dev_err(dev, "Unknown Atmel firmware revision: 0x%02x\n", ver);
		ret = -ENODEV;
		goto err_release_i2c;
	}

	drm_of_find_panel_or_bridge(dev->of_node, 0, 0, &panel, NULL);
	if (!panel)
		return -EPROBE_DEFER;

	ts->panel_bridge = drm_panel_bridge_add(panel, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ts->panel_bridge)) {
		dev_err(dev, "Failed to create panel bridge\n");
		ret = PTR_ERR(ts->panel_bridge);
		goto err_release_i2c;
	}

	/* Turn off at boot, so we can cleanly sequence powering on. */
	rpi_touchscreen_i2c_write(ts, REG_POWERON, 0);

	ts->base.funcs = &rpi_bridge_funcs;
	ts->base.of_node = dev->of_node;

	ret = drm_bridge_add(&ts->base);
	if (ret) {
		dev_err(dev, "Failed to add bridge\n");
		goto err_remove_bridge;
	}

	return mipi_dsi_attach(dsi);

err_remove_bridge:
	drm_panel_bridge_remove(ts->panel_bridge);
err_release_i2c:
	put_device(&ts->bridge_i2c->dev);
	return ret;
}

static int rpi_touchscreen_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct rpi_touchscreen *ts = dev_get_drvdata(dev);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0) {
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);
		return ret;
	}

	drm_panel_bridge_remove(ts->panel_bridge);
	drm_bridge_remove(&ts->base);

	put_device(&ts->bridge_i2c->dev);

	return 0;
}

static void rpi_touchscreen_dsi_shutdown(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct rpi_touchscreen *ts = dev_get_drvdata(dev);

	rpi_touchscreen_i2c_write(ts, REG_POWERON, 0);
}

static const struct of_device_id rpi_touchscreen_of_match[] = {
	{ .compatible = "raspberrypi,7inch-touchscreen-bridge" },
	{ } /* sentinel */
};
MODULE_DEVICE_TABLE(of, rpi_touchscreen_of_match);

static struct mipi_dsi_driver rpi_touchscreen_driver = {
	.driver = {
		.name = "raspberrypi-touchscreen-bridge",
		.of_match_table = rpi_touchscreen_of_match,
	},
	.probe = rpi_touchscreen_dsi_probe,
	.remove = rpi_touchscreen_dsi_remove,
	.shutdown = rpi_touchscreen_dsi_shutdown,
};
module_mipi_dsi_driver(rpi_touchscreen_driver);

MODULE_AUTHOR("Eric Anholt <eric@anholt.net>");
MODULE_DESCRIPTION("Raspberry Pi 7-inch touchscreen driver");
MODULE_LICENSE("GPL v2");
