#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include <linux/gpio/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

struct lhr050h41 {
	struct drm_panel	panel;
	struct mipi_dsi_device	*dsi;

	struct backlight_device *backlight;
	struct gpio_desc	*power;
	struct gpio_desc	*reset;
};

static inline struct lhr050h41 *panel_to_lhr050h41(struct drm_panel *panel)
{
	return container_of(panel, struct lhr050h41, panel);
}

static int lhr050h41_switch_page(struct lhr050h41 *ctx, u8 page)
{
	u8 buf[4] = { 0xff, 0x98, 0x81, page };

	return mipi_dsi_dcs_write_buffer(ctx->dsi, buf, sizeof(buf));
}

static int lhr050h41_send_cmd_data(struct lhr050h41 *ctx, u8 cmd, u8 data)
{
	u8 buf[2] = { cmd, data };

	return mipi_dsi_dcs_write_buffer(ctx->dsi, buf, sizeof(buf));
}

static int lhr050h41_send_init_sequence(struct lhr050h41 *ctx)
{
	lhr050h41_switch_page(ctx, 3);

	lhr050h41_send_cmd_data(ctx, 0x01, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x02, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x03, 0x73);
	lhr050h41_send_cmd_data(ctx, 0x04, 0x03);
	lhr050h41_send_cmd_data(ctx, 0x05, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x06, 0x06);
	lhr050h41_send_cmd_data(ctx, 0x07, 0x06);
	lhr050h41_send_cmd_data(ctx, 0x08, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x09, 0x18);
	lhr050h41_send_cmd_data(ctx, 0x0a, 0x04);
	lhr050h41_send_cmd_data(ctx, 0x0b, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x0c, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x0d, 0x03);
	lhr050h41_send_cmd_data(ctx, 0x0e, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x0f, 0x25);
	lhr050h41_send_cmd_data(ctx, 0x10, 0x25);
	lhr050h41_send_cmd_data(ctx, 0x11, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x12, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x13, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x14, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x15, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x16, 0x0C);
	lhr050h41_send_cmd_data(ctx, 0x17, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x18, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x19, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x1a, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x1b, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x1c, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x1d, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x1e, 0xC0);
	lhr050h41_send_cmd_data(ctx, 0x1f, 0x80);
	lhr050h41_send_cmd_data(ctx, 0x20, 0x04);
	lhr050h41_send_cmd_data(ctx, 0x21, 0x01);
	lhr050h41_send_cmd_data(ctx, 0x22, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x23, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x24, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x25, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x26, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x27, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x28, 0x33);
	lhr050h41_send_cmd_data(ctx, 0x29, 0x03);
	lhr050h41_send_cmd_data(ctx, 0x2a, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x2b, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x2c, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x2d, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x2e, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x2f, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x30, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x31, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x32, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x33, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x34, 0x04);
	lhr050h41_send_cmd_data(ctx, 0x35, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x36, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x37, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x38, 0x3C);
	lhr050h41_send_cmd_data(ctx, 0x39, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x3a, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x3b, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x3c, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x3d, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x3e, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x3f, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x40, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x41, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x42, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x43, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x44, 0x00);

	lhr050h41_send_cmd_data(ctx, 0x50, 0x01);
	lhr050h41_send_cmd_data(ctx, 0x51, 0x23);
	lhr050h41_send_cmd_data(ctx, 0x52, 0x45);
	lhr050h41_send_cmd_data(ctx, 0x53, 0x67);
	lhr050h41_send_cmd_data(ctx, 0x54, 0x89);
	lhr050h41_send_cmd_data(ctx, 0x55, 0xab);
	lhr050h41_send_cmd_data(ctx, 0x56, 0x01);
	lhr050h41_send_cmd_data(ctx, 0x57, 0x23);
	lhr050h41_send_cmd_data(ctx, 0x58, 0x45);
	lhr050h41_send_cmd_data(ctx, 0x59, 0x67);
	lhr050h41_send_cmd_data(ctx, 0x5a, 0x89);
	lhr050h41_send_cmd_data(ctx, 0x5b, 0xab);
	lhr050h41_send_cmd_data(ctx, 0x5c, 0xcd);
	lhr050h41_send_cmd_data(ctx, 0x5d, 0xef);

	lhr050h41_send_cmd_data(ctx, 0x5e, 0x11);
	lhr050h41_send_cmd_data(ctx, 0x5f, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x60, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x61, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x62, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x63, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x64, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x65, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x66, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x67, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x68, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x69, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x6a, 0x0C);
	lhr050h41_send_cmd_data(ctx, 0x6b, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x6c, 0x0F);
	lhr050h41_send_cmd_data(ctx, 0x6d, 0x0E);
	lhr050h41_send_cmd_data(ctx, 0x6e, 0x0D);
	lhr050h41_send_cmd_data(ctx, 0x6f, 0x06);
	lhr050h41_send_cmd_data(ctx, 0x70, 0x07);
	lhr050h41_send_cmd_data(ctx, 0x71, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x72, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x73, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x74, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x75, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x76, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x77, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x78, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x79, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x7a, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x7b, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x7c, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x7d, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x7e, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x7f, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x80, 0x0C);
	lhr050h41_send_cmd_data(ctx, 0x81, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x82, 0x0F);
	lhr050h41_send_cmd_data(ctx, 0x83, 0x0E);
	lhr050h41_send_cmd_data(ctx, 0x84, 0x0D);
	lhr050h41_send_cmd_data(ctx, 0x85, 0x06);
	lhr050h41_send_cmd_data(ctx, 0x86, 0x07);
	lhr050h41_send_cmd_data(ctx, 0x87, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x88, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x89, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x8A, 0x02);

	lhr050h41_switch_page(ctx, 4);
	lhr050h41_send_cmd_data(ctx, 0x6C, 0x15);
	lhr050h41_send_cmd_data(ctx, 0x6E, 0x22);
	lhr050h41_send_cmd_data(ctx, 0x6F, 0x33);
	lhr050h41_send_cmd_data(ctx, 0x3A, 0xA4);
	lhr050h41_send_cmd_data(ctx, 0x8D, 0x0D);
	lhr050h41_send_cmd_data(ctx, 0x87, 0xBA);
	lhr050h41_send_cmd_data(ctx, 0x26, 0x76);
	lhr050h41_send_cmd_data(ctx, 0xB2, 0xD1);

	lhr050h41_switch_page(ctx, 1);
	lhr050h41_send_cmd_data(ctx, 0x22, 0x0A);
	lhr050h41_send_cmd_data(ctx, 0x53, 0xDC);
	lhr050h41_send_cmd_data(ctx, 0x55, 0xA7);
	lhr050h41_send_cmd_data(ctx, 0x50, 0x78);
	lhr050h41_send_cmd_data(ctx, 0x51, 0x78);
	lhr050h41_send_cmd_data(ctx, 0x31, 0x02);
	lhr050h41_send_cmd_data(ctx, 0x60, 0x14);
	lhr050h41_send_cmd_data(ctx, 0xA0, 0x2A);
	lhr050h41_send_cmd_data(ctx, 0xA1, 0x39);
	lhr050h41_send_cmd_data(ctx, 0xA2, 0x46);
	lhr050h41_send_cmd_data(ctx, 0xA3, 0x0e);
	lhr050h41_send_cmd_data(ctx, 0xA4, 0x12);
	lhr050h41_send_cmd_data(ctx, 0xA5, 0x25);
	lhr050h41_send_cmd_data(ctx, 0xA6, 0x19);
	lhr050h41_send_cmd_data(ctx, 0xA7, 0x1d);
	lhr050h41_send_cmd_data(ctx, 0xA8, 0xa6);
	lhr050h41_send_cmd_data(ctx, 0xA9, 0x1C);
	lhr050h41_send_cmd_data(ctx, 0xAA, 0x29);
	lhr050h41_send_cmd_data(ctx, 0xAB, 0x85);
	lhr050h41_send_cmd_data(ctx, 0xAC, 0x1C);
	lhr050h41_send_cmd_data(ctx, 0xAD, 0x1B);
	lhr050h41_send_cmd_data(ctx, 0xAE, 0x51);
	lhr050h41_send_cmd_data(ctx, 0xAF, 0x22);
	lhr050h41_send_cmd_data(ctx, 0xB0, 0x2d);
	lhr050h41_send_cmd_data(ctx, 0xB1, 0x4f);
	lhr050h41_send_cmd_data(ctx, 0xB2, 0x59);
	lhr050h41_send_cmd_data(ctx, 0xB3, 0x3F);
	lhr050h41_send_cmd_data(ctx, 0xC0, 0x2A);
	lhr050h41_send_cmd_data(ctx, 0xC1, 0x3a);
	lhr050h41_send_cmd_data(ctx, 0xC2, 0x45);
	lhr050h41_send_cmd_data(ctx, 0xC3, 0x0e);
	lhr050h41_send_cmd_data(ctx, 0xC4, 0x11);
	lhr050h41_send_cmd_data(ctx, 0xC5, 0x24);
	lhr050h41_send_cmd_data(ctx, 0xC6, 0x1a);
	lhr050h41_send_cmd_data(ctx, 0xC7, 0x1c);
	lhr050h41_send_cmd_data(ctx, 0xC8, 0xaa);
	lhr050h41_send_cmd_data(ctx, 0xC9, 0x1C);
	lhr050h41_send_cmd_data(ctx, 0xCA, 0x29);
	lhr050h41_send_cmd_data(ctx, 0xCB, 0x96);
	lhr050h41_send_cmd_data(ctx, 0xCC, 0x1C);
	lhr050h41_send_cmd_data(ctx, 0xCD, 0x1B);
	lhr050h41_send_cmd_data(ctx, 0xCE, 0x51);
	lhr050h41_send_cmd_data(ctx, 0xCF, 0x22);
	lhr050h41_send_cmd_data(ctx, 0xD0, 0x2b);
	lhr050h41_send_cmd_data(ctx, 0xD1, 0x4b);
	lhr050h41_send_cmd_data(ctx, 0xD2, 0x59);
	lhr050h41_send_cmd_data(ctx, 0xD3, 0x3F);

	lhr050h41_switch_page(ctx, 0);
	lhr050h41_send_cmd_data(ctx, 0x35, 0x00);
	lhr050h41_send_cmd_data(ctx, 0x11, 0x00);

	mdelay(120);

	lhr050h41_send_cmd_data(ctx, 0x29, 0x00);

	mdelay(20);

	return 0;
}

static int lhr050h41_prepare(struct drm_panel *panel)
{
	struct lhr050h41 *ctx = panel_to_lhr050h41(panel);

	/* Power the panel */
	gpiod_set_value(ctx->power, 1);
	mdelay(5);

	/* And reset it */
	gpiod_set_value(ctx->reset, 1);
	mdelay(20);

	gpiod_set_value(ctx->reset, 0);
	mdelay(20);

	lhr050h41_send_init_sequence(ctx);

	return 0;
}

static void lhr050h41_enable_bl(struct lhr050h41 *ctx, bool enable)
{
	if (!ctx->backlight)
		return;

	if (enable) {
		ctx->backlight->props.state &= ~BL_CORE_FBBLANK;
		ctx->backlight->props.power = FB_BLANK_UNBLANK;
	} else {
		ctx->backlight->props.power = FB_BLANK_POWERDOWN;
		ctx->backlight->props.state |= BL_CORE_FBBLANK;
	}

	backlight_update_status(ctx->backlight);
}

static int lhr050h41_enable(struct drm_panel *panel)
{
	struct lhr050h41 *ctx = panel_to_lhr050h41(panel);

	lhr050h41_enable_bl(ctx, true);

	return 0;
}

static int lhr050h41_disable(struct drm_panel *panel)
{
	struct lhr050h41 *ctx = panel_to_lhr050h41(panel);

	lhr050h41_enable_bl(ctx, false);

	return mipi_dsi_dcs_set_display_off(ctx->dsi);
}

static int lhr050h41_unprepare(struct drm_panel *panel)
{
	struct lhr050h41 *ctx = panel_to_lhr050h41(panel);

	mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
	gpiod_set_value(ctx->power, 0);
	gpiod_set_value(ctx->reset, 1);

	return 0;
}

static const struct drm_display_mode default_mode = {
	.clock		= 62000,
	.vrefresh	= 60,

	.hdisplay	= 720,
	.hsync_start	= 720 + 10,
	.hsync_end	= 720 + 10 + 20,
	.htotal		= 720 + 10 + 20 + 30,

	.vdisplay	= 1280,
	.vsync_start	= 1280 + 10,
	.vsync_end	= 1280 + 10 + 10,
	.vtotal		= 1280 + 10 + 10 + 20,
};

static int lhr050h41_get_modes(struct drm_panel *panel)
{
	struct drm_connector *connector = panel->connector;
	struct lhr050h41 *ctx = panel_to_lhr050h41(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		dev_err(&ctx->dsi->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	panel->connector->display_info.width_mm = 62;
	panel->connector->display_info.height_mm = 110;

	return 1;
}

static const struct drm_panel_funcs lhr050h41_funcs = {
	.prepare	= lhr050h41_prepare,
	.unprepare	= lhr050h41_unprepare,
	.enable		= lhr050h41_enable,
	.disable	= lhr050h41_disable,
	.get_modes	= lhr050h41_get_modes,
};

static int lhr050h41_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct device_node *np;
	struct lhr050h41 *ctx;
	int ret;

	ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dsi = dsi;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = &dsi->dev;
	ctx->panel.funcs = &lhr050h41_funcs;

	ctx->power = devm_gpiod_get(&dsi->dev, "power", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->power)) {
		dev_err(&dsi->dev, "Couldn't get our power GPIO\n");
		return PTR_ERR(ctx->power);
	}

	ctx->reset = devm_gpiod_get(&dsi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset)) {
		dev_err(&dsi->dev, "Couldn't get our reset GPIO\n");
		return PTR_ERR(ctx->reset);
	}

	np = of_parse_phandle(dsi->dev.of_node, "backlight", 0);
	if (np) {
		ctx->backlight = of_find_backlight_by_node(np);
		of_node_put(np);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}

	ret = drm_panel_add(&ctx->panel);
	if (ret < 0)
		return ret;

	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 4;

	return mipi_dsi_attach(dsi);
}

static int lhr050h41_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct lhr050h41 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	if (ctx->backlight)
		put_device(&ctx->backlight->dev);

	return 0;
}

static const struct of_device_id lhr050h41_of_match[] = {
	{ .compatible = "huarui,lhr050h41" },
	{ }
};
MODULE_DEVICE_TABLE(of, lhr050h41_of_match);

static struct mipi_dsi_driver lhr050h41_dsi_driver = {
	.probe		= lhr050h41_dsi_probe,
	.remove		= lhr050h41_dsi_remove,
	.driver = {
		.name		= "lhr050h41-dsi",
		.of_match_table	= lhr050h41_of_match,
	},
};
module_mipi_dsi_driver(lhr050h41_dsi_driver);

MODULE_AUTHOR("Maxime Ripard <maxime.ripard@free-electrons.com>");
MODULE_DESCRIPTION("Huarui LHR050H41 LCD Driver");
MODULE_LICENSE("GPL v2");
