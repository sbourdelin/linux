/*
 * Copyright (C) STMicroelectronics SA 2017
 *
 * Authors: Philippe Cornu <philippe.cornu@st.com>
 *          Yannick Fertre <yannick.fertre@st.com>
 *
 * License terms:  GNU General Public License (GPL), version 2
 */
#include <drm/drmP.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <video/mipi_display.h>

#define DRV_NAME "orisetech_otm8009a"

#define OTM8009A_BACKLIGHT_DEFAULT	240
#define OTM8009A_BACKLIGHT_MAX		255

struct otm8009a {
	struct device *dev;
	struct drm_panel panel;
	struct backlight_device *bl_dev;
	struct gpio_desc *reset_gpio;
	bool prepared;
	bool enabled;
};

static const struct drm_display_mode default_mode = {
	.clock = 32729,
	.hdisplay = 480,
	.hsync_start = 480 + 120,
	.hsync_end = 480 + 120 + 63,
	.htotal = 480 + 120 + 63 + 120,
	.vdisplay = 800,
	.vsync_start = 800 + 12,
	.vsync_end = 800 + 12 + 12,
	.vtotal = 800 + 12 + 12 + 12,
	.vrefresh = 50,
	.flags = 0,
	.width_mm = 52,
	.height_mm = 86,
};

static inline struct otm8009a *panel_to_otm8009a(struct drm_panel *panel)
{
	return container_of(panel, struct otm8009a, panel);
}

static void otm8009a_dcs_write_buf(struct otm8009a *ctx, const void *data,
				   size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);

	if (mipi_dsi_dcs_write_buffer(dsi, data, len) < 0)
		DRM_WARN("mipi dsi dcs write buffer failed");
}

#define dcs_write_seq(seq...)					\
	do {							\
		static const u8 d[] = { seq };			\
		otm8009a_dcs_write_buf(ctx, d, ARRAY_SIZE(d));	\
	} while (0)

static int otm8009a_init_sequence(struct otm8009a *ctx)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	/*
	 * CMD2_ENA1: Enter in Command 2 mode, enable write function of
	 * Command 2 & enable parameter shift function.
	 * The 3 following sequences allow to enable ORISE command mode.
	 */
	dcs_write_seq(0xFF, 0x80, 0x09, 0x01);
	dcs_write_seq(0x00, 0x80);
	dcs_write_seq(0xFF, 0x80, 0x09);

	/*
	 * Starting from here, address shift needs to be set before sending
	 * a new command. You can find an example in the next sequence...
	 * SD_PCH_CTRL (0xC480) Source Driver Precharge Control (SD_PT=GND)
	 */
	dcs_write_seq(0x00, 0x80); /* address shift set to 0x80 */
	dcs_write_seq(0xC4, 0x30); /* 0xC480 parameter 1 is 0x30 */
	mdelay(10);

	/* Not documented (0xC48A) */
	dcs_write_seq(0x00, 0x8A);
	dcs_write_seq(0xC4, 0x40);
	mdelay(10);

	/* PWR_CTRL4 (0xC5B0) Power Control Setting 4 for DC Voltage */
	dcs_write_seq(0x00, 0xB1); /* 178th parameter */
	dcs_write_seq(0xC5, 0xA9);

	/* PWR_CTRL2 (0xC590) Power Control Setting 2 for Normal Mode */
	dcs_write_seq(0x00, 0x91); /* 146th parameter */
	dcs_write_seq(0xC5, 0x34);

	/* P_DRV_M (0xC0B4) Panel Driving Mode */
	dcs_write_seq(0x00, 0xB4);
	dcs_write_seq(0xC0, 0x50);

	/* VCOMDC (0xD900) VCOM Voltage Setting */
	dcs_write_seq(0x00, 0x00);
	dcs_write_seq(0xD9, 0x4E);

	/* OSC_ADJ (0xC181) Oscillator Adjustment for Idle/Normal mode */
	dcs_write_seq(0x00, 0x81);
	dcs_write_seq(0xC1, 0x66); /* 65Hz */

	/* RGB_VIDEO_SET (0xC1A1) RGB Video Mode Setting */
	dcs_write_seq(0x00, 0xA1);
	dcs_write_seq(0xC1, 0x08);

	/* PWR_CTRL2 (0xC590) Power Control Setting 2 for Normal Mode */
	dcs_write_seq(0x00, 0x92); /* 147th parameter */
	dcs_write_seq(0xC5, 0x01);
	dcs_write_seq(0x00, 0x95); /* 150th parameter */
	dcs_write_seq(0xC5, 0x34);
	dcs_write_seq(0x00, 0x94); /* 149th parameter */
	dcs_write_seq(0xC5, 0x33);

	/* GVDD/NGVDD (0xD800) */
	dcs_write_seq(0x00, 0x00);
	dcs_write_seq(0xD8, 0x79, 0x79);

	/* SD_CTRL (0xC0A2) Source Driver Timing Setting */
	dcs_write_seq(0x00, 0xA3); /* 164th parameter */
	dcs_write_seq(0xC0, 0x1B);

	/* PWR_CTRL1 (0xC580) Power Control Setting 1 */
	dcs_write_seq(0x00, 0x82); /* 131st parameter */
	dcs_write_seq(0xC5, 0x83);

	/* SD_PCH_CTRL (0xC480) Source Driver Precharge Control */
	dcs_write_seq(0x00, 0x81); /* 130th parameter */
	dcs_write_seq(0xC4, 0x83);

	/* RGB_VIDEO_SET (0xC1A1) RGB Video Mode Setting */
	dcs_write_seq(0x00, 0xA1);
	dcs_write_seq(0xC1, 0x0E); /* todo previously we wrote 0x08... */

	/* PANSET (0xB3A6) Panel Type Setting */
	dcs_write_seq(0x00, 0xA6);
	dcs_write_seq(0xB3, 0x00, 0x01);

	/* GOAVST (0xCE80) GOA VST Setting */
	dcs_write_seq(0x00, 0x80);
	dcs_write_seq(0xCE, 0x85, 0x01, 0x00, 0x84, 0x01, 0x00);

	/* GOACLKA1 (0xCEA0) GOA CLKA1 Setting */
	dcs_write_seq(0x00, 0xA0);
	dcs_write_seq(0xCE, 0x18, 0x04, 0x03, 0x39, 0x00, 0x00, 0x00, 0x18,
		      0x03, 0x03, 0x3A, 0x00, 0x00, 0x00);

	/* GOACLKA3 (0xCEB0) GOA CLKA3 Setting */
	dcs_write_seq(0x00, 0xB0);
	dcs_write_seq(0xCE, 0x18, 0x02, 0x03, 0x3B, 0x00, 0x00, 0x00, 0x18,
		      0x01, 0x03, 0x3C, 0x00, 0x00, 0x00);

	/* GOAECLK (0xCFC0) GOA ECLK Setting */
	dcs_write_seq(0x00, 0xC0);
	dcs_write_seq(0xCF, 0x01, 0x01, 0x20, 0x20, 0x00, 0x00, 0x01, 0x02,
		      0x00, 0x00);

	/* Not documented */
	dcs_write_seq(0x00, 0xD0);
	dcs_write_seq(0xCF, 0x00);

	/* PANCTRLSET1-8 (0xCB80-0xCBF0) Panel Control Setting 1-8 */
	dcs_write_seq(0x00, 0x80);
	dcs_write_seq(0xCB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x00);
	dcs_write_seq(0x00, 0x90);
	dcs_write_seq(0xCB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	dcs_write_seq(0x00, 0xA0);
	dcs_write_seq(0xCB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	dcs_write_seq(0x00, 0xB0);
	dcs_write_seq(0xCB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x00);
	dcs_write_seq(0x00, 0xC0);
	dcs_write_seq(0xCB, 0x00, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00,
		      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	dcs_write_seq(0x00, 0xD0);
	dcs_write_seq(0xCB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04,
		      0x04, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00);
	dcs_write_seq(0x00, 0xE0);
	dcs_write_seq(0xCB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x00);
	dcs_write_seq(0x00, 0xF0);
	dcs_write_seq(0xCB, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		      0xFF, 0xFF);

	/* PANU2D1-3 (0xCC80-0xCCA0) Panel U2D Setting 1-3 */
	dcs_write_seq(0x00, 0x80);
	dcs_write_seq(0xCC, 0x00, 0x26, 0x09, 0x0B, 0x01, 0x25, 0x00, 0x00,
		      0x00, 0x00);
	dcs_write_seq(0x00, 0x90);
	dcs_write_seq(0xCC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x00, 0x00, 0x26, 0x0A, 0x0C, 0x02);
	dcs_write_seq(0x00, 0xA0);
	dcs_write_seq(0xCC, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);

	/* PAND2U1-3 (0xCCB0-0xCCD0) Panel D2U Setting 1-3 */
	dcs_write_seq(0x00, 0xB0);
	dcs_write_seq(0xCC, 0x00, 0x25, 0x0C, 0x0A, 0x02, 0x26, 0x00, 0x00,
		      0x00, 0x00);
	dcs_write_seq(0x00, 0xC0);
	dcs_write_seq(0xCC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x00, 0x00, 0x25, 0x0B, 0x09, 0x01);
	dcs_write_seq(0x00, 0xD0);
	dcs_write_seq(0xCC, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);

	/* PWR_CTRL1 (0xC580) Power Control Setting 1 */
	dcs_write_seq(0x00, 0x81); /* 130th parameter */
	dcs_write_seq(0xC5, 0x66);

	/* Not documented */
	dcs_write_seq(0x00, 0xB6);
	dcs_write_seq(0xF5, 0x06);

	/* GMCT2.2P (0xE100) Gamma Correction 2.2+ Setting */
	dcs_write_seq(0x00, 0x00);
	dcs_write_seq(0xE1, 0x00, 0x09, 0x0F, 0x0E, 0x07, 0x10, 0x0B, 0x0A,
		      0x04, 0x07, 0x0B, 0x08, 0x0F, 0x10, 0x0A, 0x01);

	/* GMCT2.2N (0xE100) Gamma Correction 2.2- Setting */
	dcs_write_seq(0x00, 0x00);
	dcs_write_seq(0xE2, 0x00, 0x09, 0x0F, 0x0E, 0x07, 0x10, 0x0B, 0x0A,
		      0x04, 0x07, 0x0B, 0x08, 0x0F, 0x10, 0x0A, 0x01);

	/* Exit CMD2 mode */
	dcs_write_seq(0x00, 0x00);
	dcs_write_seq(0xFF, 0xFF, 0xFF, 0xFF);

	/* OTM8009a NOP */
	dcs_write_seq(0x00, 0x00);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret)
		return ret;

	/* Wait for sleep out exit */
	mdelay(120);

	/* Default portrait 480x800 rgb24 */
	dcs_write_seq(MIPI_DCS_SET_ADDRESS_MODE, 0x00);
	dcs_write_seq(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x01, 0xDF);
	dcs_write_seq(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x03, 0x1F);
	dcs_write_seq(MIPI_DCS_SET_PIXEL_FORMAT, 0x77);

	/* Disable CABC feature */
	dcs_write_seq(0x55, 0x00);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret)
		return ret;

	/* OTM8009a NOP */
	dcs_write_seq(0x00, 0x00);

	/* Send Command GRAM memory write (no parameters) */
	dcs_write_seq(MIPI_DCS_WRITE_MEMORY_START);

	return 0;
}

static int otm8009a_disable(struct drm_panel *panel)
{
	struct otm8009a *ctx = panel_to_otm8009a(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (!ctx->enabled)
		return 0; /* This is not an issue so we return 0 here */

	/* Power off the backlight. Note: end-user still controls brightness */
	ctx->bl_dev->props.power = FB_BLANK_POWERDOWN;
	ret = backlight_update_status(ctx->bl_dev);
	if (ret)
		return ret;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0)
		return ret;

	msleep(120);

	ctx->enabled = false;

	return 0;
}

static int otm8009a_unprepare(struct drm_panel *panel)
{
	struct otm8009a *ctx = panel_to_otm8009a(panel);

	if (!ctx->prepared)
		return 0;

	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		msleep(20);
	}

	ctx->prepared = false;

	return 0;
}

static int otm8009a_prepare(struct drm_panel *panel)
{
	struct otm8009a *ctx = panel_to_otm8009a(panel);
	int ret;

	if (ctx->prepared)
		return 0;

	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		msleep(20);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		msleep(20);
	}

	ret = otm8009a_init_sequence(ctx);
	if (ret)
		return ret;

	ctx->prepared = true;

	/* Power on the backlight. Note: end-user still controls brightness */
	ctx->bl_dev->props.power = FB_BLANK_UNBLANK;
	backlight_update_status(ctx->bl_dev);

	return 0;
}

static int otm8009a_enable(struct drm_panel *panel)
{
	struct otm8009a *ctx = panel_to_otm8009a(panel);

	ctx->enabled = true;

	return 0;
}

static int otm8009a_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		DRM_ERROR("failed to add mode %ux%ux@%u\n",
			  default_mode.hdisplay, default_mode.vdisplay,
			  default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(panel->connector, mode);

	panel->connector->display_info.width_mm = mode->width_mm;
	panel->connector->display_info.height_mm = mode->height_mm;

	return 1;
}

static const struct drm_panel_funcs otm8009a_drm_funcs = {
	.disable   = otm8009a_disable,
	.unprepare = otm8009a_unprepare,
	.prepare   = otm8009a_prepare,
	.enable    = otm8009a_enable,
	.get_modes = otm8009a_get_modes,
};

/*
 * DSI-BASED BACKLIGHT
 */

static int otm8009a_backlight_update_status(struct backlight_device *bd)
{
	struct otm8009a *ctx = bl_get_data(bd);
	u8 data[2];

	if (!ctx->prepared) {
		DRM_WARN("lcd not ready yet for setting its backlight!\n");
		return -ENXIO;
	}

	if (bd->props.power <= FB_BLANK_NORMAL) {
		/* Power on the backlight with the requested brightness */
		data[0] = MIPI_DCS_SET_DISPLAY_BRIGHTNESS;
		data[1] = bd->props.brightness;
		otm8009a_dcs_write_buf(ctx, data, ARRAY_SIZE(data));
		/* set Brightness Control & Backlight on */
		data[1] = 0x24;
	} else {
		/* Power off the backlight: set Brightness Control & Bl off */
		data[1] = 0;
	}

	/* Update Brightness Control & Backlight */
	data[0] = MIPI_DCS_WRITE_CONTROL_DISPLAY;
	otm8009a_dcs_write_buf(ctx, data, ARRAY_SIZE(data));

	return 0;
}

static const struct backlight_ops otm8009a_backlight_ops = {
	.update_status = otm8009a_backlight_update_status,
};

static int otm8009a_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct otm8009a *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpio\n");
		return PTR_ERR(ctx->reset_gpio);
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;

	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &otm8009a_drm_funcs;

	ctx->bl_dev = backlight_device_register(DRV_NAME "_backlight", dev, ctx,
						&otm8009a_backlight_ops, NULL);
	if (IS_ERR(ctx->bl_dev)) {
		dev_err(dev, "failed to register backlight device\n");
		return PTR_ERR(ctx->bl_dev);
	}

	ctx->bl_dev->props.max_brightness = OTM8009A_BACKLIGHT_MAX;
	ctx->bl_dev->props.brightness = OTM8009A_BACKLIGHT_DEFAULT;
	ctx->bl_dev->props.power = FB_BLANK_POWERDOWN;
	ctx->bl_dev->props.type = BACKLIGHT_RAW;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "mipi_dsi_attach failed. Is host ready?\n");
		drm_panel_remove(&ctx->panel);
		backlight_device_unregister(ctx->bl_dev);
		return ret;
	}

	DRM_INFO(DRV_NAME "_panel %ux%u@%u %ubpp dsi %udl - ready\n",
		 default_mode.hdisplay, default_mode.vdisplay,
		 default_mode.vrefresh,
		 mipi_dsi_pixel_format_to_bpp(dsi->format), dsi->lanes);

	return 0;
}

static int otm8009a_remove(struct mipi_dsi_device *dsi)
{
	struct otm8009a *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	backlight_device_unregister(ctx->bl_dev);

	return 0;
}

static const struct of_device_id orisetech_otm8009a_of_match[] = {
	{ .compatible = "orisetech,otm8009a" },
	{ }
};
MODULE_DEVICE_TABLE(of, orisetech_otm8009a_of_match);

static struct mipi_dsi_driver orisetech_otm8009a_driver = {
	.probe  = otm8009a_probe,
	.remove = otm8009a_remove,
	.driver = {
		.name = DRV_NAME "_panel",
		.of_match_table = orisetech_otm8009a_of_match,
	},
};
module_mipi_dsi_driver(orisetech_otm8009a_driver);

MODULE_AUTHOR("Philippe Cornu <philippe.cornu@st.com>");
MODULE_AUTHOR("Yannick Fertre <yannick.fertre@st.com>");
MODULE_DESCRIPTION("DRM driver for Orise Tech OTM8009A MIPI DSI panel");
MODULE_LICENSE("GPL v2");
