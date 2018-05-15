// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 */

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/of_gpio.h>
#include <linux/of_graph.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#define SN_BRIDGE_REVISION_ID 0x2

/* Link Training specific registers */
#define SN_DEVICE_REV_REG			0x08
#define SN_HPD_DISABLE_REG			0x5C
#define SN_REFCLK_FREQ_REG			0x0A
#define SN_DSI_LANES_REG			0x10
#define SN_DSIA_CLK_FREQ_REG			0x12
#define SN_ENH_FRAME_REG			0x5A
#define SN_SSC_CONFIG_REG			0x93
#define SN_DATARATE_CONFIG_REG			0x94
#define SN_PLL_ENABLE_REG			0x0D
#define SN_SCRAMBLE_CONFIG_REG			0x95
#define SN_AUX_WDATA0_REG			0x64
#define SN_AUX_ADDR_19_16_REG			0x74
#define SN_AUX_ADDR_15_8_REG			0x75
#define SN_AUX_ADDR_7_0_REG			0x76
#define SN_AUX_LENGTH_REG			0x77
#define SN_AUX_CMD_REG				0x78
#define SN_ML_TX_MODE_REG			0x96
/* video config specific registers */
#define SN_CHA_ACTIVE_LINE_LENGTH_LOW_REG	0x20
#define SN_CHA_ACTIVE_LINE_LENGTH_HIGH_REG	0x21
#define SN_CHA_VERTICAL_DISPLAY_SIZE_LOW_REG	0x24
#define SN_CHA_VERTICAL_DISPLAY_SIZE_HIGH_REG	0x25
#define SN_CHA_HSYNC_PULSE_WIDTH_LOW_REG	0x2C
#define SN_CHA_HSYNC_PULSE_WIDTH_HIGH_REG	0x2D
#define SN_CHA_VSYNC_PULSE_WIDTH_LOW_REG	0x30
#define SN_CHA_VSYNC_PULSE_WIDTH_HIGH_REG	0x31
#define SN_CHA_HORIZONTAL_BACK_PORCH_REG	0x34
#define SN_CHA_VERTICAL_BACK_PORCH_REG		0x36
#define SN_CHA_HORIZONTAL_FRONT_PORCH_REG	0x38
#define SN_CHA_VERTICAL_FRONT_PORCH_REG		0x3A
#define SN_DATA_FORMAT_REG			0x5B

#define MIN_DSI_CLK_FREQ_MHZ	40

/* fudge factor required to account for 8b/10b encoding */
#define DP_CLK_FUDGE_NUM	10
#define DP_CLK_FUDGE_DEN	8

#define DPPLL_CLK_SRC_REFCLK	0
#define DPPLL_CLK_SRC_DSICLK	1

#define SN_DSIA_REFCLK_OFFSET	1
#define SN_DSIA_LANE_OFFSET	3
#define SN_DP_LANE_OFFSET	4
#define SN_DP_DATA_RATE_OFFSET	5
#define SN_TIMING_HIGH_OFFSET	8

#define SN_ENABLE_VID_STREAM_BIT	BIT(3)
#define SN_DSIA_NUM_LANES_BITS		(BIT(4) | BIT(3))
#define SN_DP_NUM_LANES_BITS		(BIT(5) | BIT(4))
#define SN_DP_DATA_RATE_BITS		(BIT(7) | BIT(6) | BIT(5))
#define SN_HPD_DISABLE_BIT		BIT(0)

struct ti_sn_bridge {
	struct device			*dev;
	struct regmap			*regmap;
	struct drm_bridge		bridge;
	struct drm_connector		connector;
	struct device_node		*host_node;
	struct mipi_dsi_device		*dsi;
	struct clk			*refclk;
	struct drm_panel		*panel;
	struct gpio_desc		*enable_gpio;
	unsigned int			num_supplies;
	struct regulator_bulk_data	*supplies;
	struct i2c_adapter		*ddc;
	struct drm_display_mode		curr_mode;
};

static const struct regmap_range ti_sn_bridge_volatile_ranges[] = {
	{ .range_min = 0, .range_max = 0xff },
};

static const struct regmap_access_table ti_sn_bridge_volatile_table = {
	.yes_ranges = ti_sn_bridge_volatile_ranges,
	.n_yes_ranges = ARRAY_SIZE(ti_sn_bridge_volatile_ranges),
};

static const struct regmap_config ti_sn_bridge_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.volatile_table = &ti_sn_bridge_volatile_table,
	.cache_type = REGCACHE_NONE,
};

#ifdef CONFIG_PM
static int ti_sn_bridge_resume(struct device *dev)
{
	struct ti_sn_bridge *pdata = dev_get_drvdata(dev);
	int ret = 0;

	ret = regulator_bulk_enable(pdata->num_supplies, pdata->supplies);
	if (ret) {
		DRM_ERROR("failed to enable supplies %d\n", ret);
		return ret;
	}

	gpiod_set_value(pdata->enable_gpio, 1);

	return ret;
}

static int ti_sn_bridge_suspend(struct device *dev)
{
	struct ti_sn_bridge *pdata = dev_get_drvdata(dev);
	int ret = 0;

	gpiod_set_value(pdata->enable_gpio, 0);

	ret = regulator_bulk_disable(pdata->num_supplies, pdata->supplies);
	if (ret)
		DRM_ERROR("failed to disable supplies %d\n", ret);

	return ret;
}
#endif

static const struct dev_pm_ops ti_sn_bridge_pm_ops = {
	SET_RUNTIME_PM_OPS(ti_sn_bridge_suspend, ti_sn_bridge_resume, NULL)
};

/* Connector funcs */
static struct ti_sn_bridge *
connector_to_ti_sn_bridge(struct drm_connector *connector)
{
	return container_of(connector, struct ti_sn_bridge, connector);
}

static int ti_sn_bridge_connector_get_modes(struct drm_connector *connector)
{
	struct ti_sn_bridge *pdata = connector_to_ti_sn_bridge(connector);
	struct drm_panel *panel = pdata->panel;
	struct edid *edid;
	u32 num_modes;

	if (panel) {
		DRM_DEBUG_KMS("get mode from connected drm_panel\n");
		return drm_panel_get_modes(panel);
	}

	if (!pdata->ddc)
		return 0;

	pm_runtime_get_sync(pdata->dev);
	edid = drm_get_edid(connector, pdata->ddc);
	pm_runtime_put_sync(pdata->dev);
	if (!edid)
		return 0;

	drm_mode_connector_update_edid_property(connector, edid);
	num_modes = drm_add_edid_modes(connector, edid);
	kfree(edid);

	return num_modes;
}

static enum drm_mode_status
ti_sn_bridge_connector_mode_valid(struct drm_connector *connector,
			     struct drm_display_mode *mode)
{
	/* maximum supported resolution is 4K at 60 fps */
	if (mode->clock > 594000)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static struct drm_connector_helper_funcs ti_sn_bridge_connector_helper_funcs = {
	.get_modes = ti_sn_bridge_connector_get_modes,
	.mode_valid = ti_sn_bridge_connector_mode_valid,
};

static enum drm_connector_status
ti_sn_bridge_connector_detect(struct drm_connector *connector, bool force)
{
	struct ti_sn_bridge *pdata = connector_to_ti_sn_bridge(connector);

	/**
	 * TODO: Currently if drm_panel is present, then always
	 * return the status as connected. Need to add support to detect
	 * device state for no panel(hot pluggable) scenarios.
	 */
	if (pdata->panel)
		return connector_status_connected;
	else
		return connector_status_unknown;
}

static const struct drm_connector_funcs ti_sn_bridge_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.detect = ti_sn_bridge_connector_detect,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static struct ti_sn_bridge *bridge_to_ti_sn_bridge(struct drm_bridge *bridge)
{
	return container_of(bridge, struct ti_sn_bridge, bridge);
}

static int ti_sn_bridge_read_device_rev(struct ti_sn_bridge *pdata)
{
	unsigned int rev = 0;
	int ret = 0;

	ret = regmap_read(pdata->regmap, SN_DEVICE_REV_REG, &rev);
	if (ret) {
		DRM_ERROR("Revision read failed %d\n", ret);
		return ret;
	}

	if (rev != SN_BRIDGE_REVISION_ID) {
		DRM_ERROR("ti_sn_bridge revision id: 0x%x mismatch\n", rev);
		ret = -EINVAL;
	}

	return ret;
}

static const char * const ti_sn_bridge_supply_names[] = {
	"vcca",
	"vcc",
	"vccio",
	"vpll",
};

static int ti_sn_bridge_parse_regulators(struct ti_sn_bridge *pdata)
{
	unsigned int i;

	pdata->num_supplies = ARRAY_SIZE(ti_sn_bridge_supply_names);

	pdata->supplies = devm_kcalloc(pdata->dev, pdata->num_supplies,
				       sizeof(*pdata->supplies), GFP_KERNEL);
	if (!pdata->supplies)
		return -ENOMEM;

	for (i = 0; i < pdata->num_supplies; i++)
		pdata->supplies[i].supply = ti_sn_bridge_supply_names[i];

	return devm_regulator_bulk_get(pdata->dev,
				       pdata->num_supplies, pdata->supplies);
}

static int ti_sn_bridge_attach_panel(struct ti_sn_bridge *pdata)
{
	struct device_node *panel_node, *port, *endpoint;

	pdata->panel = NULL;
	port = of_graph_get_port_by_id(pdata->dev->of_node, 1);
	if (!port)
		return 0;

	endpoint = of_get_child_by_name(port, "endpoint");
	of_node_put(port);
	if (!endpoint) {
		DRM_ERROR("no output endpoint found\n");
		return -EINVAL;
	}

	panel_node = of_graph_get_remote_port_parent(endpoint);
	of_node_put(endpoint);
	if (!panel_node) {
		DRM_ERROR("no output node found\n");
		return -EINVAL;
	}

	pdata->panel = of_drm_find_panel(panel_node);
	of_node_put(panel_node);
	if (!pdata->panel) {
		DRM_ERROR("no panel node found\n");
		return -EINVAL;
	}
	drm_panel_attach(pdata->panel, &pdata->connector);
	DRM_DEBUG_KMS("drm panel attached to ti_sn_bridge\n");

	return 0;
}

static int ti_sn_bridge_attach(struct drm_bridge *bridge)
{
	struct mipi_dsi_host *host;
	struct mipi_dsi_device *dsi;
	struct ti_sn_bridge *pdata = bridge_to_ti_sn_bridge(bridge);
	int ret;
	const struct mipi_dsi_device_info info = { .type = "ti_sn_bridge",
						   .channel = 0,
						   .node = NULL,
						 };

	if (!bridge->encoder) {
		DRM_ERROR("Parent encoder object not found\n");
		return -ENODEV;
	}

	/* HPD not supported */
	pdata->connector.polled = 0;

	ret = drm_connector_init(bridge->dev, &pdata->connector,
				 &ti_sn_bridge_connector_funcs,
				 DRM_MODE_CONNECTOR_eDP);
	if (ret) {
		DRM_ERROR("Failed to initialize connector with drm\n");
		return ret;
	}

	drm_connector_helper_add(&pdata->connector,
				 &ti_sn_bridge_connector_helper_funcs);
	drm_mode_connector_attach_encoder(&pdata->connector, bridge->encoder);

	host = of_find_mipi_dsi_host_by_node(pdata->host_node);
	if (!host) {
		DRM_ERROR("failed to find dsi host\n");
		return -ENODEV;
	}

	dsi = mipi_dsi_device_register_full(host, &info);
	if (IS_ERR(dsi)) {
		DRM_ERROR("failed to create dsi device\n");
		ret = PTR_ERR(dsi);
		return ret;
	}

	/* TODO: setting to 4 lanes always for now */
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_EOT_PACKET | MIPI_DSI_MODE_VIDEO_HSE;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		DRM_ERROR("failed to attach dsi to host\n");
		mipi_dsi_device_unregister(dsi);
		return ret;
	}

	pdata->dsi = dsi;

	DRM_DEBUG_KMS("ti_sn_bridge attached to dsi\n");
	/* attach panel to bridge */
	ti_sn_bridge_attach_panel(pdata);

	return 0;
}

static void ti_sn_bridge_mode_set(struct drm_bridge *bridge,
				    struct drm_display_mode *mode,
				    struct drm_display_mode *adj_mode)
{
	struct ti_sn_bridge *pdata = bridge_to_ti_sn_bridge(bridge);

	DRM_DEBUG("mode_set: hdisplay=%d, vdisplay=%d, vrefresh=%d, clock=%d\n",
		adj_mode->hdisplay, adj_mode->vdisplay,
		adj_mode->vrefresh, adj_mode->clock);

	drm_mode_copy(&pdata->curr_mode, adj_mode);
}

static void ti_sn_bridge_disable(struct drm_bridge *bridge)
{
	struct ti_sn_bridge *pdata = bridge_to_ti_sn_bridge(bridge);
	struct drm_panel *panel = pdata->panel;

	if (panel) {
		drm_panel_disable(panel);
		drm_panel_unprepare(panel);
	}

	/* disable video stream */
	regmap_update_bits(pdata->regmap, SN_ENH_FRAME_REG,
			   SN_ENABLE_VID_STREAM_BIT, 0);
	/* semi auto link training mode OFF */
	regmap_write(pdata->regmap, SN_ML_TX_MODE_REG, 0);
	/* disable DP PLL */
	regmap_write(pdata->regmap, SN_PLL_ENABLE_REG, 0);
}

static u32 ti_sn_bridge_get_dsi_freq(struct ti_sn_bridge *pdata)
{
	u32 bit_rate_khz, clk_freq_khz;
	struct drm_display_mode *mode = &pdata->curr_mode;

	bit_rate_khz = mode->clock *
			mipi_dsi_pixel_format_to_bpp(pdata->dsi->format);
	clk_freq_khz = bit_rate_khz / (pdata->dsi->lanes * 2);

	return clk_freq_khz;
}

#define REFCLK_LUT_SIZE	5

/* clk frequencies supported by bridge in Hz in case derived from REFCLK pin */
static const u32 ti_sn_bridge_refclk_lut[] = {
	12000000,
	19200000,
	26000000,
	27000000,
	38400000,
};

/* clk frequencies supported by bridge in Hz in case derived from DACP/N pin */
static const u32 ti_sn_bridge_dsiclk_lut[] = {
	468000000,
	384000000,
	416000000,
	486000000,
	460800000,
};

static void ti_sn_bridge_set_refclk(struct ti_sn_bridge *pdata)
{
	int i = 0;
	u8 refclk_src;
	u32 refclk_rate;
	const u32 *refclk_lut;

	if (pdata->refclk) {
		refclk_src = DPPLL_CLK_SRC_REFCLK;
		refclk_rate = clk_get_rate(pdata->refclk);
		refclk_lut = ti_sn_bridge_refclk_lut;
		clk_prepare_enable(pdata->refclk);
	} else {
		refclk_src = DPPLL_CLK_SRC_DSICLK;
		refclk_rate = ti_sn_bridge_get_dsi_freq(pdata) * 1000;
		refclk_lut = ti_sn_bridge_dsiclk_lut;
	}

	/* for i equals to REFCLK_LUT_SIZE means default frequency */
	for (i = 0; i < REFCLK_LUT_SIZE; i++)
		if (refclk_lut[i] == refclk_rate)
			break;

	regmap_write(pdata->regmap, SN_REFCLK_FREQ_REG,
		     (refclk_src | (i << SN_DSIA_REFCLK_OFFSET)));
}

/**
 * LUT index corresponds to register value and
 * LUT values corresponds to dp data rate supported
 * by the bridge in Mbps unit.
 */
static const unsigned int ti_sn_bridge_dp_rate_lut[] = {
	0, 1620, 2160, 2430, 2700, 3240, 4320, 5400
};

static void ti_sn_bridge_set_dsi_dp_rate(struct ti_sn_bridge *pdata)
{
	unsigned int bit_rate_mhz, clk_freq_mhz, dp_rate_mhz;
	unsigned int val = 0, i = 0;
	struct drm_display_mode *mode = &pdata->curr_mode;

	/* set DSIA clk frequency */
	bit_rate_mhz = (mode->clock / 1000) *
			mipi_dsi_pixel_format_to_bpp(pdata->dsi->format);
	clk_freq_mhz = bit_rate_mhz / (pdata->dsi->lanes * 2);

	/* for each increment in val, frequency increases by 5MHz */
	val = (MIN_DSI_CLK_FREQ_MHZ / 5) +
		(((clk_freq_mhz - MIN_DSI_CLK_FREQ_MHZ) / 5) & 0xFF);
	regmap_write(pdata->regmap, SN_DSIA_CLK_FREQ_REG, val);

	/* set DP data rate */
	dp_rate_mhz = ((bit_rate_mhz / pdata->dsi->lanes) * DP_CLK_FUDGE_NUM) /
							DP_CLK_FUDGE_DEN;
	for (i = 0; i < ARRAY_SIZE(ti_sn_bridge_dp_rate_lut); i++)
		if (ti_sn_bridge_dp_rate_lut[i] > dp_rate_mhz)
			break;
	if (i == ARRAY_SIZE(ti_sn_bridge_dp_rate_lut))
		i--; /* set to maximum possible */

	regmap_update_bits(pdata->regmap, SN_DATARATE_CONFIG_REG,
			   SN_DP_DATA_RATE_BITS, i << SN_DP_DATA_RATE_OFFSET);
}

static void ti_sn_bridge_set_video_timings(struct ti_sn_bridge *pdata)
{
	struct drm_display_mode *mode = &pdata->curr_mode;

	regmap_write(pdata->regmap, SN_CHA_ACTIVE_LINE_LENGTH_LOW_REG,
		     mode->hdisplay & 0xFF);
	regmap_write(pdata->regmap, SN_CHA_ACTIVE_LINE_LENGTH_HIGH_REG,
		     (mode->hdisplay >> SN_TIMING_HIGH_OFFSET) & 0xFF);
	regmap_write(pdata->regmap, SN_CHA_VERTICAL_DISPLAY_SIZE_LOW_REG,
		     mode->vdisplay & 0xFF);
	regmap_write(pdata->regmap, SN_CHA_VERTICAL_DISPLAY_SIZE_HIGH_REG,
		     (mode->vdisplay >> SN_TIMING_HIGH_OFFSET) & 0xFF);
	regmap_write(pdata->regmap, SN_CHA_HSYNC_PULSE_WIDTH_LOW_REG,
		     (mode->hsync_end - mode->hsync_start) & 0xFF);
	regmap_write(pdata->regmap, SN_CHA_HSYNC_PULSE_WIDTH_HIGH_REG,
		     ((mode->hsync_end - mode->hsync_start) >>
		      SN_TIMING_HIGH_OFFSET) & 0xFF);
	regmap_write(pdata->regmap, SN_CHA_VSYNC_PULSE_WIDTH_LOW_REG,
		     (mode->vsync_end - mode->vsync_start) & 0xFF);
	regmap_write(pdata->regmap, SN_CHA_VSYNC_PULSE_WIDTH_HIGH_REG,
		     ((mode->vsync_end - mode->vsync_start) >>
		      SN_TIMING_HIGH_OFFSET) & 0xFF);
	regmap_write(pdata->regmap, SN_CHA_HORIZONTAL_BACK_PORCH_REG,
		     (mode->htotal - mode->hsync_end) & 0xFF);
	regmap_write(pdata->regmap, SN_CHA_VERTICAL_BACK_PORCH_REG,
		     (mode->vtotal - mode->vsync_end) & 0xFF);
	regmap_write(pdata->regmap, SN_CHA_HORIZONTAL_FRONT_PORCH_REG,
		     (mode->hsync_start - mode->hdisplay) & 0xFF);
	regmap_write(pdata->regmap, SN_CHA_VERTICAL_FRONT_PORCH_REG,
		     (mode->vsync_start - mode->vdisplay) & 0xFF);
	usleep_range(10000, 10500); /* 10ms delay recommended by spec */
}

static void ti_sn_bridge_enable(struct drm_bridge *bridge)
{
	struct ti_sn_bridge *pdata = bridge_to_ti_sn_bridge(bridge);
	struct drm_panel *panel = pdata->panel;
	unsigned int val = 0;

	if (panel) {
		drm_panel_prepare(panel);
		/* in case drm_panel is connected then HPD is not supported */
		regmap_update_bits(pdata->regmap, SN_HPD_DISABLE_REG,
				   SN_HPD_DISABLE_BIT, SN_HPD_DISABLE_BIT);
	}

	/* DSI_A lane config */
	val = (4 - pdata->dsi->lanes) << SN_DSIA_LANE_OFFSET;
	regmap_update_bits(pdata->regmap, SN_DSI_LANES_REG,
			   SN_DSIA_NUM_LANES_BITS, val);

	/* DP lane config */
	val = (pdata->dsi->lanes - 1) << SN_DP_LANE_OFFSET;
	regmap_update_bits(pdata->regmap, SN_SSC_CONFIG_REG,
			   SN_DP_NUM_LANES_BITS, val);

	/* set dsi/dp clk frequency value */
	ti_sn_bridge_set_dsi_dp_rate(pdata);

	/* enable DP PLL */
	regmap_write(pdata->regmap, SN_PLL_ENABLE_REG, 1);
	usleep_range(10000, 10500); /* 10ms delay recommended by spec */

	/**
	 * The SN65DSI86 only supports ASSR Display Authentication method and
	 * this method is enabled by default. An eDP panel must support this
	 * authentication method. We need to enable this method in the eDP panel
	 * at DisplayPort address 0x0010A prior to link training.
	 */
	regmap_write(pdata->regmap, SN_AUX_WDATA0_REG, 0x01);
	regmap_write(pdata->regmap, SN_AUX_ADDR_19_16_REG, 0x00);
	regmap_write(pdata->regmap, SN_AUX_ADDR_15_8_REG, 0x01);
	regmap_write(pdata->regmap, SN_AUX_ADDR_7_0_REG, 0x0A);
	regmap_write(pdata->regmap, SN_AUX_LENGTH_REG, 0x01);
	regmap_write(pdata->regmap, SN_AUX_CMD_REG, 0x81);
	usleep_range(10000, 10500); /* 10ms delay recommended by spec */

	/* Semi auto link training mode */
	regmap_write(pdata->regmap, SN_ML_TX_MODE_REG, 0x0A);
	msleep(20); /* 20ms delay recommended by spec */

	/* config video parameters */
	ti_sn_bridge_set_video_timings(pdata);

	/* enable video stream */
	regmap_update_bits(pdata->regmap, SN_ENH_FRAME_REG,
			   SN_ENABLE_VID_STREAM_BIT, SN_ENABLE_VID_STREAM_BIT);

	if (panel)
		drm_panel_enable(panel);
}

static void ti_sn_bridge_pre_enable(struct drm_bridge *bridge)
{
	struct ti_sn_bridge *pdata = bridge_to_ti_sn_bridge(bridge);

	pm_runtime_get_sync(pdata->dev);

	/* configure bridge CLK_SRC and ref_clk */
	ti_sn_bridge_set_refclk(pdata);
}

static void ti_sn_bridge_post_disable(struct drm_bridge *bridge)
{
	struct ti_sn_bridge *pdata = bridge_to_ti_sn_bridge(bridge);

	if (pdata->refclk)
		clk_disable_unprepare(pdata->refclk);

	pm_runtime_put_sync(pdata->dev);
}

static const struct drm_bridge_funcs ti_sn_bridge_funcs = {
	.attach = ti_sn_bridge_attach,
	.pre_enable = ti_sn_bridge_pre_enable,
	.enable = ti_sn_bridge_enable,
	.disable = ti_sn_bridge_disable,
	.post_disable = ti_sn_bridge_post_disable,
	.mode_set = ti_sn_bridge_mode_set,
};

static int ti_sn_bridge_parse_dsi_host(struct ti_sn_bridge *pdata)
{
	struct device_node *np = pdata->dev->of_node;
	struct device_node *end_node;

	end_node = of_graph_get_endpoint_by_regs(np, 0, 0);
	if (!end_node) {
		DRM_ERROR("remote endpoint not found\n");
		return -ENODEV;
	}

	pdata->host_node = of_graph_get_remote_port_parent(end_node);
	of_node_put(end_node);
	if (!pdata->host_node) {
		DRM_ERROR("remote node not found\n");
		return -ENODEV;
	}
	of_node_put(pdata->host_node);

	return 0;
}

static int ti_sn_bridge_probe(struct i2c_client *client,
	 const struct i2c_device_id *id)
{
	struct ti_sn_bridge *pdata;
	struct device_node *ddc_node;
	int ret = 0;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		DRM_ERROR("device doesn't support I2C\n");
		return -ENODEV;
	}

	pdata = devm_kzalloc(&client->dev, sizeof(struct ti_sn_bridge),
			     GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->dev = &client->dev;
	dev_set_drvdata(&client->dev, pdata);

	pdata->regmap = devm_regmap_init_i2c(client,
					     &ti_sn_bridge_regmap_config);
	if (IS_ERR(pdata->regmap)) {
		DRM_ERROR("regmap i2c init failed\n");
		return PTR_ERR(pdata->regmap);
	}

	pdata->enable_gpio = devm_gpiod_get(pdata->dev,
					    "enable", GPIOD_OUT_LOW);
	if (IS_ERR(pdata->enable_gpio)) {
		DRM_ERROR("failed to get enable gpio from DT\n");
		ret = PTR_ERR(pdata->enable_gpio);
		return ret;
	}

	ret = ti_sn_bridge_parse_regulators(pdata);
	if (ret) {
		DRM_ERROR("failed to parse regulators\n");
		return ret;
	}

	ret = ti_sn_bridge_parse_dsi_host(pdata);
	if (ret)
		return ret;

	pm_runtime_enable(pdata->dev);
	pm_runtime_get_sync(pdata->dev);
	ret = ti_sn_bridge_read_device_rev(pdata);
	pm_runtime_put_sync(pdata->dev);
	if (ret)
		goto err_rev_read;

	pdata->refclk = devm_clk_get(pdata->dev, "refclk");

	ddc_node = of_parse_phandle(pdata->dev->of_node, "ddc-i2c-bus", 0);
	if (ddc_node) {
		pdata->ddc = of_find_i2c_adapter_by_node(ddc_node);
		of_node_put(ddc_node);
		if (!pdata->ddc) {
			DRM_DEBUG_KMS("failed to read ddc node\n");
			ret = -EPROBE_DEFER;
			goto err_rev_read;
		}
	} else {
		DRM_DEBUG_KMS("no ddc property found\n");
	}

	i2c_set_clientdata(client, pdata);

	pdata->bridge.funcs = &ti_sn_bridge_funcs;
	pdata->bridge.of_node = client->dev.of_node;

	drm_bridge_add(&pdata->bridge);

	return 0;

err_rev_read:
	pm_runtime_disable(pdata->dev);
	return ret;
}

static int ti_sn_bridge_remove(struct i2c_client *client)
{
	struct ti_sn_bridge *pdata = i2c_get_clientdata(client);

	if (!pdata)
		return -EINVAL;

	mipi_dsi_detach(pdata->dsi);
	mipi_dsi_device_unregister(pdata->dsi);

	drm_bridge_remove(&pdata->bridge);
	pm_runtime_disable(pdata->dev);
	i2c_put_adapter(pdata->ddc);

	return 0;
}

static struct i2c_device_id ti_sn_bridge_id[] = {
	{ "ti,sn65dsi86", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, ti_sn_bridge_id);

static const struct of_device_id ti_sn_bridge_match_table[] = {
	{.compatible = "ti,sn65dsi86"},
	{},
};
MODULE_DEVICE_TABLE(of, ti_sn_bridge_match_table);

static struct i2c_driver ti_sn_bridge_driver = {
	.driver = {
		.name = "ti_sn65dsi86",
		.of_match_table = ti_sn_bridge_match_table,
		.pm = &ti_sn_bridge_pm_ops,
	},
	.probe = ti_sn_bridge_probe,
	.remove = ti_sn_bridge_remove,
	.id_table = ti_sn_bridge_id,
};

module_i2c_driver(ti_sn_bridge_driver);
MODULE_DESCRIPTION("sn65dsi86 DSI to eDP bridge driver");
MODULE_LICENSE("GPL v2");
