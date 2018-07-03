/*
 * Copyright (C) Fuzhou Rockchip Electronics Co.Ltd
 * Author: Chris Zhong <zyw@rock-chips.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_dp_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_of.h>
#include <drm/drm_bridge.h>

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/extcon.h>
#include <linux/firmware.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/mfd/syscon.h>
#include <linux/phy/phy.h>
#include <linux/iopoll.h>

#include <sound/hdmi-codec.h>

#include "cdn-dp-core.h"
#include "cdn-dp-reg.h"
#include "rockchip_drm_vop.h"

#define connector_to_dp(c) \
		container_of(c, struct cdn_dp_device, connector)

#define encoder_to_dp(c) \
		container_of(c, struct cdn_dp_device, encoder)

#define GRF_SOC_CON9		0x6224
#define DP_SEL_VOP_LIT		BIT(12)
#define GRF_SOC_CON26		0x6268
#define DPTX_HPD_SEL		(3 << 12)
#define DPTX_HPD_DEL		(2 << 12)
#define DPTX_HPD_SEL_MASK	(3 << 28)

#define CDN_FW_TIMEOUT_MS	(64 * 1000)
#define CDN_DPCD_TIMEOUT_MS	5000
#define RK_DP_FIRMWARE		"rockchip/dptx.bin"
#define CDN_DP_FIRMWARE		"cadence/dptx.bin"

#define FW_ALIVE_TIMEOUT_US		1000000
#define HPD_EVENT_TIMEOUT		40000

struct cdn_dp_data {
	u8 max_phy;
};

struct cdn_dp_data rk3399_cdn_dp = {
	.max_phy = 2,
};

static const struct of_device_id cdn_dp_dt_ids[] = {
	{ .compatible = "rockchip,rk3399-cdn-dp",
		.data = (void *)&rk3399_cdn_dp },
	{ .compatible = "cdns,mhdp", .data = NULL },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, cdn_dp_dt_ids);

static int cdn_dp_grf_write(struct cdn_dp_device *dp,
			    unsigned int reg, unsigned int val)
{
	int ret;

	ret = clk_prepare_enable(dp->grf_clk);
	if (ret) {
		DRM_DEV_ERROR(dp->dev, "Failed to prepare_enable grf clock\n");
		return ret;
	}

	ret = regmap_write(dp->grf, reg, val);
	if (ret) {
		DRM_DEV_ERROR(dp->dev, "Could not write to GRF: %d\n", ret);
		return ret;
	}

	clk_disable_unprepare(dp->grf_clk);

	return 0;
}

static int cdn_dp_clk_enable(struct cdn_dp_device *dp)
{
	int ret;
	unsigned long rate;

	ret = clk_prepare_enable(dp->pclk);
	if (ret < 0) {
		DRM_DEV_ERROR(dp->dev, "cannot enable dp pclk %d\n", ret);
		goto err_pclk;
	}

	ret = clk_prepare_enable(dp->core_clk);
	if (ret < 0) {
		DRM_DEV_ERROR(dp->dev, "cannot enable core_clk %d\n", ret);
		goto err_core_clk;
	}

	ret = pm_runtime_get_sync(dp->dev);
	if (ret < 0) {
		DRM_DEV_ERROR(dp->dev, "cannot get pm runtime %d\n", ret);
		goto err_pm_runtime_get;
	}

	reset_control_assert(dp->core_rst);
	reset_control_assert(dp->dptx_rst);
	reset_control_assert(dp->apb_rst);
	reset_control_deassert(dp->core_rst);
	reset_control_deassert(dp->dptx_rst);
	reset_control_deassert(dp->apb_rst);

	rate = clk_get_rate(dp->core_clk);
	if (!rate) {
		DRM_DEV_ERROR(dp->dev, "get clk rate failed\n");
		ret = -EINVAL;
		goto err_set_rate;
	}

	cdn_dp_set_fw_clk(dp, rate);
	cdn_dp_clock_reset(dp);

	return 0;

err_set_rate:
	pm_runtime_put(dp->dev);
err_pm_runtime_get:
	clk_disable_unprepare(dp->core_clk);
err_core_clk:
	clk_disable_unprepare(dp->pclk);
err_pclk:
	return ret;
}

static void cdn_dp_clk_disable(struct cdn_dp_device *dp)
{
	pm_runtime_put_sync(dp->dev);
	clk_disable_unprepare(dp->pclk);
	clk_disable_unprepare(dp->core_clk);
}

static int cdn_dp_get_port_lanes(struct cdn_dp_port *port)
{
	struct extcon_dev *edev = port->extcon;
	union extcon_property_value property;
	int dptx;
	u8 lanes;

	dptx = extcon_get_state(edev, EXTCON_DISP_DP);
	if (dptx > 0) {
		extcon_get_property(edev, EXTCON_DISP_DP,
				    EXTCON_PROP_USB_SS, &property);
		if (property.intval)
			lanes = 2;
		else
			lanes = 4;
	} else {
		lanes = 0;
	}

	return lanes;
}

static int cdn_dp_get_sink_count(struct cdn_dp_device *dp, u8 *sink_count)
{
	int ret;
	u8 value;

	*sink_count = 0;
	ret = cdn_dp_dpcd_read(dp, DP_SINK_COUNT, &value, 1);
	if (ret)
		return ret;

	*sink_count = DP_GET_SINK_COUNT(value);
	return 0;
}

static struct cdn_dp_port *cdn_dp_connected_port(struct cdn_dp_device *dp)
{
	struct cdn_dp_port *port;
	int i, lanes;

	for (i = 0; i < dp->ports; i++) {
		port = dp->port[i];
		lanes = cdn_dp_get_port_lanes(port);
		if (lanes)
			return port;
	}
	return NULL;
}

static bool cdn_dp_check_sink_connection(struct cdn_dp_device *dp)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(CDN_DPCD_TIMEOUT_MS);
	struct cdn_dp_port *port;
	u8 sink_count = 0;

	if (dp->active_port < 0 || dp->active_port >= dp->ports) {
		DRM_DEV_ERROR(dp->dev, "active_port is wrong!\n");
		return false;
	}

	port = dp->port[dp->active_port];

	/*
	 * Attempt to read sink count, retry in case the sink may not be ready.
	 *
	 * Sinks are *supposed* to come up within 1ms from an off state, but
	 * some docks need more time to power up.
	 */
	while (time_before(jiffies, timeout)) {
		if (!extcon_get_state(port->extcon, EXTCON_DISP_DP))
			return false;

		if (!cdn_dp_get_sink_count(dp, &sink_count))
			return sink_count ? true : false;

		usleep_range(5000, 10000);
	}

	DRM_DEV_ERROR(dp->dev, "Get sink capability timed out\n");
	return false;
}

static enum drm_connector_status
cdn_dp_connector_detect(struct drm_connector *connector, bool force)
{
	struct cdn_dp_device *dp = connector_to_dp(connector);
	enum drm_connector_status status = connector_status_disconnected;

	if (dp->mhdp_ip) {
		int ret = cdn_dp_get_hpd_status(dp);

		if (ret > 0)
			status = connector_status_connected;
	} else {
		mutex_lock(&dp->lock);
		if (dp->connected)
			status = connector_status_connected;
		mutex_unlock(&dp->lock);
	}

	return status;
}

static void cdn_dp_connector_destroy(struct drm_connector *connector)
{
	struct cdn_dp_device *dp = connector_to_dp(connector);

	if (!dp->mhdp_ip)
		drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static const struct drm_connector_funcs cdn_dp_atomic_connector_funcs = {
	.detect = cdn_dp_connector_detect,
	.destroy = cdn_dp_connector_destroy,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.dpms = drm_helper_connector_dpms,
};

static int cdn_dp_connector_get_modes(struct drm_connector *connector)
{
	struct cdn_dp_device *dp = connector_to_dp(connector);
	struct edid *edid;
	int ret = 0;

	mutex_lock(&dp->lock);

	if (dp->mhdp_ip)
		edid = drm_do_get_edid(connector, cdn_dp_get_edid_block, dp);
	else
		edid = dp->edid;

	if (edid) {
		DRM_DEV_DEBUG_KMS(dp->dev, "got edid: width[%d] x height[%d]\n",
				  edid->width_cm, edid->height_cm);

		dp->sink_has_audio = drm_detect_monitor_audio(edid);
		ret = drm_add_edid_modes(connector, edid);
		if (ret)
			drm_mode_connector_update_edid_property(connector,
								edid);
	}

	mutex_unlock(&dp->lock);

	return ret;
}

static int cdn_dp_connector_mode_valid(struct drm_connector *connector,
				       struct drm_display_mode *mode)
{
	struct cdn_dp_device *dp = connector_to_dp(connector);
	struct drm_display_info *display_info = &dp->connector.display_info;
	u32 requested, actual, rate, sink_max, source_max = 0;
	u8 lanes, bpc;

	/* If DP is disconnected, every mode is invalid */
	if (!dp->connected)
		return MODE_BAD;

	switch (display_info->bpc) {
	case 10:
		bpc = 10;
		break;
	case 6:
		bpc = 6;
		break;
	default:
		bpc = 8;
		break;
	}

	requested = mode->clock * bpc * 3 / 1000;

	source_max = dp->lanes;
	sink_max = drm_dp_max_lane_count(dp->dpcd);
	lanes = min(source_max, sink_max);

	source_max = drm_dp_bw_code_to_link_rate(CDN_DP_MAX_LINK_RATE);
	sink_max = drm_dp_max_link_rate(dp->dpcd);
	rate = min(source_max, sink_max);

	actual = rate * lanes / 100;

	/* efficiency is about 0.8 */
	actual = actual * 8 / 10;

	if (requested > actual) {
		DRM_DEV_DEBUG_KMS(dp->dev,
				  "requested=%d, actual=%d, clock=%d\n",
				  requested, actual, mode->clock);
		return MODE_CLOCK_HIGH;
	}

	return MODE_OK;
}

static struct drm_connector_helper_funcs cdn_dp_connector_helper_funcs = {
	.get_modes = cdn_dp_connector_get_modes,
	.mode_valid = cdn_dp_connector_mode_valid,
};

static int cdn_dp_firmware_init(struct cdn_dp_device *dp)
{
	int ret;
	const u32 *iram_data, *dram_data;
	const struct firmware *fw = dp->fw;
	const struct cdn_firmware_header *hdr;

	hdr = (struct cdn_firmware_header *)fw->data;
	if (fw->size != le32_to_cpu(hdr->size_bytes)) {
		DRM_DEV_ERROR(dp->dev, "firmware is invalid\n");
		return -EINVAL;
	}

	iram_data = (const u32 *)(fw->data + hdr->header_size);
	dram_data = (const u32 *)(fw->data + hdr->header_size + hdr->iram_size);

	ret = cdn_dp_load_firmware(dp, iram_data, hdr->iram_size,
				   dram_data, hdr->dram_size);
	if (ret)
		return ret;

	ret = cdn_dp_set_firmware_active(dp, true);
	if (ret) {
		DRM_DEV_ERROR(dp->dev, "active fw failed: %d\n", ret);
		return ret;
	}

	return cdn_dp_event_config(dp);
}

static int cdn_dp_get_sink_capability(struct cdn_dp_device *dp)
{
	int ret;

	if (!cdn_dp_check_sink_connection(dp))
		return -ENODEV;

	ret = cdn_dp_dpcd_read(dp, DP_DPCD_REV, dp->dpcd,
			       DP_RECEIVER_CAP_SIZE);
	if (ret) {
		DRM_DEV_ERROR(dp->dev, "Failed to get caps %d\n", ret);
		return ret;
	}

	kfree(dp->edid);
	dp->edid = drm_do_get_edid(&dp->connector,
				   cdn_dp_get_edid_block, dp);
	return 0;
}

static int cdn_dp_enable_phy(struct cdn_dp_device *dp, struct cdn_dp_port *port)
{
	union extcon_property_value property;
	int ret;

	if (!port->phy_enabled) {
		ret = phy_power_on(port->phy);
		if (ret) {
			DRM_DEV_ERROR(dp->dev, "phy power on failed: %d\n",
				      ret);
			goto err_phy;
		}
		port->phy_enabled = true;
	}

	ret = cdn_dp_grf_write(dp, GRF_SOC_CON26,
			       DPTX_HPD_SEL_MASK | DPTX_HPD_SEL);
	if (ret) {
		DRM_DEV_ERROR(dp->dev, "Failed to write HPD_SEL %d\n", ret);
		goto err_power_on;
	}

	ret = cdn_dp_get_hpd_status(dp);
	if (ret <= 0) {
		if (!ret)
			DRM_DEV_ERROR(dp->dev, "hpd does not exist\n");
		goto err_power_on;
	}

	ret = extcon_get_property(port->extcon, EXTCON_DISP_DP,
				  EXTCON_PROP_USB_TYPEC_POLARITY, &property);
	if (ret) {
		DRM_DEV_ERROR(dp->dev, "get property failed\n");
		goto err_power_on;
	}

	port->lanes = cdn_dp_get_port_lanes(port);
	ret = cdn_dp_set_host_cap(dp, port->lanes, property.intval);
	if (ret) {
		DRM_DEV_ERROR(dp->dev, "set host capabilities failed: %d\n",
			      ret);
		goto err_power_on;
	}

	dp->active_port = port->id;
	return 0;

err_power_on:
	if (phy_power_off(port->phy))
		DRM_DEV_ERROR(dp->dev, "phy power off failed: %d", ret);
	else
		port->phy_enabled = false;

err_phy:
	cdn_dp_grf_write(dp, GRF_SOC_CON26,
			 DPTX_HPD_SEL_MASK | DPTX_HPD_DEL);
	return ret;
}

static int cdn_dp_disable_phy(struct cdn_dp_device *dp,
			      struct cdn_dp_port *port)
{
	int ret;

	if (port->phy_enabled) {
		ret = phy_power_off(port->phy);
		if (ret) {
			DRM_DEV_ERROR(dp->dev, "phy power off failed: %d", ret);
			return ret;
		}
	}

	port->phy_enabled = false;
	port->lanes = 0;
	dp->active_port = -1;
	return 0;
}

static int cdn_dp_disable(struct cdn_dp_device *dp)
{
	int ret, i;

	if (!dp->active)
		return 0;

	for (i = 0; i < dp->ports; i++)
		cdn_dp_disable_phy(dp, dp->port[i]);

	ret = cdn_dp_grf_write(dp, GRF_SOC_CON26,
			       DPTX_HPD_SEL_MASK | DPTX_HPD_DEL);
	if (ret) {
		DRM_DEV_ERROR(dp->dev, "Failed to clear hpd sel %d\n",
			      ret);
		return ret;
	}

	cdn_dp_set_firmware_active(dp, false);
	cdn_dp_clk_disable(dp);
	dp->active = false;
	dp->link.rate = 0;
	dp->link.num_lanes = 0;
	if (!dp->connected) {
		kfree(dp->edid);
		dp->edid = NULL;
	}

	return 0;
}

static int cdn_dp_enable(struct cdn_dp_device *dp)
{
	int ret, i, lanes;
	struct cdn_dp_port *port;

	port = cdn_dp_connected_port(dp);
	if (!port) {
		DRM_DEV_ERROR(dp->dev,
			      "Can't enable without connection\n");
		return -ENODEV;
	}

	if (dp->active)
		return 0;

	ret = cdn_dp_clk_enable(dp);
	if (ret)
		return ret;

	ret = cdn_dp_firmware_init(dp);
	if (ret) {
		DRM_DEV_ERROR(dp->dev, "firmware init failed: %d", ret);
		goto err_clk_disable;
	}

	/* only enable the port that connected with downstream device */
	for (i = port->id; i < dp->ports; i++) {
		port = dp->port[i];
		lanes = cdn_dp_get_port_lanes(port);
		if (lanes) {
			ret = cdn_dp_enable_phy(dp, port);
			if (ret)
				continue;

			ret = cdn_dp_get_sink_capability(dp);
			if (ret) {
				cdn_dp_disable_phy(dp, port);
			} else {
				dp->active = true;
				dp->lanes = port->lanes;
				return 0;
			}
		}
	}

err_clk_disable:
	cdn_dp_clk_disable(dp);
	return ret;
}

static void cdn_dp_encoder_mode_set(struct drm_encoder *encoder,
				    struct drm_display_mode *mode,
				    struct drm_display_mode *adjusted)
{
	struct cdn_dp_device *dp = encoder_to_dp(encoder);
	struct drm_display_info *display_info = &dp->connector.display_info;
	struct video_info *video = &dp->video_info;

	switch (display_info->bpc) {
	case 10:
		video->color_depth = 10;
		break;
	case 6:
		video->color_depth = 6;
		break;
	default:
		video->color_depth = 8;
		break;
	}

	video->color_fmt = PXL_RGB;
	video->v_sync_polarity = !!(mode->flags & DRM_MODE_FLAG_NVSYNC);
	video->h_sync_polarity = !!(mode->flags & DRM_MODE_FLAG_NHSYNC);

	memcpy(&dp->mode, adjusted, sizeof(*mode));
}

static bool cdn_dp_check_link_status(struct cdn_dp_device *dp)
{
	u8 link_status[DP_LINK_STATUS_SIZE];
	struct cdn_dp_port *port = cdn_dp_connected_port(dp);
	u8 sink_lanes = drm_dp_max_lane_count(dp->dpcd);

	if (!port || !dp->link.rate || !dp->link.num_lanes)
		return false;

	if (cdn_dp_dpcd_read(dp, DP_LANE0_1_STATUS, link_status,
			     DP_LINK_STATUS_SIZE)) {
		DRM_ERROR("Failed to get link status\n");
		return false;
	}

	/* if link training is requested we should perform it always */
	return drm_dp_channel_eq_ok(link_status, min(port->lanes, sink_lanes));
}

static void cdn_dp_encoder_enable(struct drm_encoder *encoder)
{
	struct cdn_dp_device *dp = encoder_to_dp(encoder);
	int ret, val;

	ret = drm_of_encoder_active_endpoint_id(dp->dev->of_node, encoder);
	if (ret < 0) {
		DRM_DEV_ERROR(dp->dev, "Could not get vop id, %d", ret);
		return;
	}

	DRM_DEV_DEBUG_KMS(dp->dev, "vop %s output to cdn-dp\n",
			  (ret) ? "LIT" : "BIG");
	if (ret)
		val = DP_SEL_VOP_LIT | (DP_SEL_VOP_LIT << 16);
	else
		val = DP_SEL_VOP_LIT << 16;

	ret = cdn_dp_grf_write(dp, GRF_SOC_CON9, val);
	if (ret)
		return;

	mutex_lock(&dp->lock);

	ret = cdn_dp_enable(dp);
	if (ret) {
		DRM_DEV_ERROR(dp->dev, "Failed to enable encoder %d\n",
			      ret);
		goto out;
	}
	if (!cdn_dp_check_link_status(dp)) {
		ret = cdn_dp_train_link(dp);
		if (ret) {
			DRM_DEV_ERROR(dp->dev, "Failed link train %d\n", ret);
			goto out;
		}
	}

	ret = cdn_dp_set_video_status(dp, CONTROL_VIDEO_IDLE);
	if (ret) {
		DRM_DEV_ERROR(dp->dev, "Failed to idle video %d\n", ret);
		goto out;
	}

	ret = cdn_dp_config_video(dp);
	if (ret) {
		DRM_DEV_ERROR(dp->dev, "Failed to config video %d\n", ret);
		goto out;
	}

	ret = cdn_dp_set_video_status(dp, CONTROL_VIDEO_VALID);
	if (ret) {
		DRM_DEV_ERROR(dp->dev, "Failed to valid video %d\n", ret);
		goto out;
	}
out:
	mutex_unlock(&dp->lock);
}

static void cdn_dp_encoder_disable(struct drm_encoder *encoder)
{
	struct cdn_dp_device *dp = encoder_to_dp(encoder);
	int ret;

	mutex_lock(&dp->lock);
	if (dp->active) {
		ret = cdn_dp_disable(dp);
		if (ret) {
			DRM_DEV_ERROR(dp->dev, "Failed to disable encoder %d\n",
				      ret);
		}
	}
	mutex_unlock(&dp->lock);

	/*
	 * In the following 2 cases, we need to run the event_work to re-enable
	 * the DP:
	 * 1. If there is not just one port device is connected, and remove one
	 *    device from a port, the DP will be disabled here, at this case,
	 *    run the event_work to re-open DP for the other port.
	 * 2. If re-training or re-config failed, the DP will be disabled here.
	 *    run the event_work to re-connect it.
	 */
	if (!dp->connected && cdn_dp_connected_port(dp))
		schedule_work(&dp->event_work);
}

static int cdn_dp_encoder_atomic_check(struct drm_encoder *encoder,
				       struct drm_crtc_state *crtc_state,
				       struct drm_connector_state *conn_state)
{
	struct rockchip_crtc_state *s = to_rockchip_crtc_state(crtc_state);

	s->output_mode = ROCKCHIP_OUT_MODE_AAAA;
	s->output_type = DRM_MODE_CONNECTOR_DisplayPort;

	return 0;
}

static const struct drm_encoder_helper_funcs cdn_dp_encoder_helper_funcs = {
	.mode_set = cdn_dp_encoder_mode_set,
	.enable = cdn_dp_encoder_enable,
	.disable = cdn_dp_encoder_disable,
	.atomic_check = cdn_dp_encoder_atomic_check,
};

static const struct drm_encoder_funcs cdn_dp_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int cdn_dp_parse_dt(struct cdn_dp_device *dp)
{
	struct device *dev = dp->dev;
	struct device_node *np = dev->of_node;

	dp->grf = syscon_regmap_lookup_by_phandle(np, "rockchip,grf");
	if (IS_ERR(dp->grf)) {
		DRM_DEV_ERROR(dev, "cdn-dp needs rockchip,grf property\n");
		return PTR_ERR(dp->grf);
	}

	dp->core_clk = devm_clk_get(dev, "core-clk");
	if (IS_ERR(dp->core_clk)) {
		DRM_DEV_ERROR(dev, "cannot get core_clk_dp\n");
		return PTR_ERR(dp->core_clk);
	}

	dp->pclk = devm_clk_get(dev, "pclk");
	if (IS_ERR(dp->pclk)) {
		DRM_DEV_ERROR(dev, "cannot get pclk\n");
		return PTR_ERR(dp->pclk);
	}

	dp->spdif_clk = devm_clk_get(dev, "spdif");
	if (IS_ERR(dp->spdif_clk)) {
		DRM_DEV_ERROR(dev, "cannot get spdif_clk\n");
		return PTR_ERR(dp->spdif_clk);
	}

	dp->grf_clk = devm_clk_get(dev, "grf");
	if (IS_ERR(dp->grf_clk)) {
		DRM_DEV_ERROR(dev, "cannot get grf clk\n");
		return PTR_ERR(dp->grf_clk);
	}

	dp->spdif_rst = devm_reset_control_get(dev, "spdif");
	if (IS_ERR(dp->spdif_rst)) {
		DRM_DEV_ERROR(dev, "no spdif reset control found\n");
		return PTR_ERR(dp->spdif_rst);
	}

	dp->dptx_rst = devm_reset_control_get(dev, "dptx");
	if (IS_ERR(dp->dptx_rst)) {
		DRM_DEV_ERROR(dev, "no uphy reset control found\n");
		return PTR_ERR(dp->dptx_rst);
	}

	dp->core_rst = devm_reset_control_get(dev, "core");
	if (IS_ERR(dp->core_rst)) {
		DRM_DEV_ERROR(dev, "no core reset control found\n");
		return PTR_ERR(dp->core_rst);
	}

	dp->apb_rst = devm_reset_control_get(dev, "apb");
	if (IS_ERR(dp->apb_rst)) {
		DRM_DEV_ERROR(dev, "no apb reset control found\n");
		return PTR_ERR(dp->apb_rst);
	}

	return 0;
}

static int cdn_dp_audio_hw_params(struct device *dev,  void *data,
				  struct hdmi_codec_daifmt *daifmt,
				  struct hdmi_codec_params *params)
{
	struct cdn_dp_device *dp = dev_get_drvdata(dev);
	struct audio_info audio = {
		.sample_width = params->sample_width,
		.sample_rate = params->sample_rate,
		.channels = params->channels,
	};
	int ret;

	mutex_lock(&dp->lock);
	if (!dp->active) {
		ret = -ENODEV;
		goto out;
	}

	switch (daifmt->fmt) {
	case HDMI_I2S:
		audio.format = AFMT_I2S;
		break;
	case HDMI_SPDIF:
		audio.format = AFMT_SPDIF;
		break;
	default:
		DRM_DEV_ERROR(dev, "Invalid format %d\n", daifmt->fmt);
		ret = -EINVAL;
		goto out;
	}

	ret = cdn_dp_audio_config(dp, &audio);
	if (!ret)
		dp->audio_info = audio;

out:
	mutex_unlock(&dp->lock);
	return ret;
}

static void cdn_dp_audio_shutdown(struct device *dev, void *data)
{
	struct cdn_dp_device *dp = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&dp->lock);
	if (!dp->active)
		goto out;

	ret = cdn_dp_audio_stop(dp, &dp->audio_info);
	if (!ret)
		dp->audio_info.format = AFMT_UNUSED;
out:
	mutex_unlock(&dp->lock);
}

static int cdn_dp_audio_digital_mute(struct device *dev, void *data,
				     bool enable)
{
	struct cdn_dp_device *dp = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&dp->lock);
	if (!dp->active) {
		ret = -ENODEV;
		goto out;
	}

	ret = cdn_dp_audio_mute(dp, enable);

out:
	mutex_unlock(&dp->lock);
	return ret;
}

static int cdn_dp_audio_get_eld(struct device *dev, void *data,
				u8 *buf, size_t len)
{
	struct cdn_dp_device *dp = dev_get_drvdata(dev);

	memcpy(buf, dp->connector.eld, min(sizeof(dp->connector.eld), len));

	return 0;
}

static const struct hdmi_codec_ops audio_codec_ops = {
	.hw_params = cdn_dp_audio_hw_params,
	.audio_shutdown = cdn_dp_audio_shutdown,
	.digital_mute = cdn_dp_audio_digital_mute,
	.get_eld = cdn_dp_audio_get_eld,
};

static int cdn_dp_audio_codec_init(struct cdn_dp_device *dp,
				   struct device *dev)
{
	struct hdmi_codec_pdata codec_data = {
		.i2s = 1,
		.spdif = 1,
		.ops = &audio_codec_ops,
		.max_i2s_channels = 8,
	};

	dp->audio_pdev = platform_device_register_data(
			 dev, HDMI_CODEC_DRV_NAME, PLATFORM_DEVID_AUTO,
			 &codec_data, sizeof(codec_data));

	return PTR_ERR_OR_ZERO(dp->audio_pdev);
}

static int cdn_dp_request_firmware(struct cdn_dp_device *dp)
{
	int ret;
	unsigned long timeout = jiffies + msecs_to_jiffies(CDN_FW_TIMEOUT_MS);
	unsigned long sleep = 1000;

	WARN_ON(!mutex_is_locked(&dp->lock));

	if (dp->fw_loaded)
		return 0;

	/* Drop the lock before getting the firmware to avoid blocking boot */
	mutex_unlock(&dp->lock);

	while (time_before(jiffies, timeout)) {
		ret = request_firmware(&dp->fw, RK_DP_FIRMWARE, dp->dev);
		if (ret == -ENOENT) {
			msleep(sleep);
			sleep *= 2;
			continue;
		} else if (ret) {
			DRM_DEV_ERROR(dp->dev,
				      "failed to request firmware: %d\n", ret);
			goto out;
		}

		dp->fw_loaded = true;
		ret = 0;
		goto out;
	}

	DRM_DEV_ERROR(dp->dev, "Timed out trying to load firmware\n");
	ret = -ETIMEDOUT;
out:
	mutex_lock(&dp->lock);
	return ret;
}

static void cdn_dp_pd_event_work(struct work_struct *work)
{
	struct cdn_dp_device *dp = container_of(work, struct cdn_dp_device,
						event_work);
	struct drm_connector *connector = &dp->connector;
	enum drm_connector_status old_status;

	int ret;

	mutex_lock(&dp->lock);

	if (dp->suspended)
		goto out;

	ret = cdn_dp_request_firmware(dp);
	if (ret)
		goto out;

	dp->connected = true;

	/* Not connected, notify userspace to disable the block */
	if (!cdn_dp_connected_port(dp)) {
		DRM_DEV_INFO(dp->dev, "Not connected. Disabling cdn\n");
		dp->connected = false;

	/* Connected but not enabled, enable the block */
	} else if (!dp->active) {
		DRM_DEV_INFO(dp->dev, "Connected, not enabled. Enabling cdn\n");
		ret = cdn_dp_enable(dp);
		if (ret) {
			DRM_DEV_ERROR(dp->dev, "Enable dp failed %d\n", ret);
			dp->connected = false;
		}

	/* Enabled and connected to a dongle without a sink, notify userspace */
	} else if (!cdn_dp_check_sink_connection(dp)) {
		DRM_DEV_INFO(dp->dev, "Connected without sink. Assert hpd\n");
		dp->connected = false;

	/* Enabled and connected with a sink, re-train if requested */
	} else if (!cdn_dp_check_link_status(dp)) {
		unsigned int rate = dp->link.rate;
		unsigned int lanes = dp->link.num_lanes;
		struct drm_display_mode *mode = &dp->mode;

		DRM_DEV_INFO(dp->dev, "Connected with sink. Re-train link\n");
		ret = cdn_dp_train_link(dp);
		if (ret) {
			dp->connected = false;
			DRM_DEV_ERROR(dp->dev, "Train link failed %d\n", ret);
			goto out;
		}

		/* If training result is changed, update the video config */
		if ((rate != dp->link.rate || lanes != dp->link.num_lanes) &&
				mode->clock) {
			ret = cdn_dp_config_video(dp);
			if (ret) {
				dp->connected = false;
				DRM_DEV_ERROR(dp->dev,
					      "Failed to config video %d\n",
					      ret);
			}
		}
	}

out:
	mutex_unlock(&dp->lock);

	old_status = connector->status;
	connector->status = connector->funcs->detect(connector, false);
	if (old_status != connector->status)
		drm_kms_helper_hotplug_event(dp->drm_dev);
}

static int cdn_dp_pd_event(struct notifier_block *nb,
			   unsigned long event, void *priv)
{
	struct cdn_dp_port *port = container_of(nb, struct cdn_dp_port,
						event_nb);
	struct cdn_dp_device *dp = port->dp;

	/*
	 * It would be nice to be able to just do the work inline right here.
	 * However, we need to make a bunch of calls that might sleep in order
	 * to turn on the block/phy, so use a worker instead.
	 */
	schedule_work(&dp->event_work);

	return NOTIFY_DONE;
}

static int cdn_dp_bind(struct device *dev, struct device *master, void *data)
{
	struct cdn_dp_device *dp = dev_get_drvdata(dev);
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	struct cdn_dp_port *port;
	struct drm_device *drm_dev = data;
	int ret, i;

	ret = cdn_dp_parse_dt(dp);
	if (ret < 0)
		return ret;

	dp->drm_dev = drm_dev;
	dp->connected = false;
	dp->active = false;
	dp->active_port = -1;
	dp->fw_loaded = false;

	INIT_WORK(&dp->event_work, cdn_dp_pd_event_work);

	encoder = &dp->encoder;

	encoder->possible_crtcs = drm_of_find_possible_crtcs(drm_dev,
							     dev->of_node);
	DRM_DEBUG_KMS("possible_crtcs = 0x%x\n", encoder->possible_crtcs);

	ret = drm_encoder_init(drm_dev, encoder, &cdn_dp_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, NULL);
	if (ret) {
		DRM_ERROR("failed to initialize encoder with drm\n");
		return ret;
	}

	drm_encoder_helper_add(encoder, &cdn_dp_encoder_helper_funcs);

	connector = &dp->connector;
	connector->polled = DRM_CONNECTOR_POLL_HPD;
	connector->dpms = DRM_MODE_DPMS_OFF;

	ret = drm_connector_init(drm_dev, connector,
				 &cdn_dp_atomic_connector_funcs,
				 DRM_MODE_CONNECTOR_DisplayPort);
	if (ret) {
		DRM_ERROR("failed to initialize connector with drm\n");
		goto err_free_encoder;
	}

	drm_connector_helper_add(connector, &cdn_dp_connector_helper_funcs);

	ret = drm_mode_connector_attach_encoder(connector, encoder);
	if (ret) {
		DRM_ERROR("failed to attach connector and encoder\n");
		goto err_free_connector;
	}

	for (i = 0; i < dp->ports; i++) {
		port = dp->port[i];

		port->event_nb.notifier_call = cdn_dp_pd_event;
		ret = devm_extcon_register_notifier(dp->dev, port->extcon,
						    EXTCON_DISP_DP,
						    &port->event_nb);
		if (ret) {
			DRM_DEV_ERROR(dev,
				      "register EXTCON_DISP_DP notifier err\n");
			goto err_free_connector;
		}
	}

	pm_runtime_enable(dev);

	schedule_work(&dp->event_work);

	return 0;

err_free_connector:
	drm_connector_cleanup(connector);
err_free_encoder:
	drm_encoder_cleanup(encoder);
	return ret;
}

static void cdn_dp_unbind(struct device *dev, struct device *master, void *data)
{
	struct cdn_dp_device *dp = dev_get_drvdata(dev);
	struct drm_encoder *encoder = &dp->encoder;
	struct drm_connector *connector = &dp->connector;

	cancel_work_sync(&dp->event_work);
	cdn_dp_encoder_disable(encoder);
	encoder->funcs->destroy(encoder);
	connector->funcs->destroy(connector);

	pm_runtime_disable(dev);
	if (dp->fw_loaded)
		release_firmware(dp->fw);
	kfree(dp->edid);
	dp->edid = NULL;
}

static const struct component_ops cdn_dp_component_ops = {
	.bind = cdn_dp_bind,
	.unbind = cdn_dp_unbind,
};

int cdn_dp_suspend(struct device *dev)
{
	struct cdn_dp_device *dp = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&dp->lock);
	if (dp->active)
		ret = cdn_dp_disable(dp);
	dp->suspended = true;
	mutex_unlock(&dp->lock);

	return ret;
}

int cdn_dp_resume(struct device *dev)
{
	struct cdn_dp_device *dp = dev_get_drvdata(dev);

	mutex_lock(&dp->lock);
	dp->suspended = false;
	if (dp->fw_loaded)
		schedule_work(&dp->event_work);
	mutex_unlock(&dp->lock);

	return 0;
}

static inline struct cdn_dp_device *bridge_to_dp(struct drm_bridge *bridge)
{
	return container_of(bridge, struct cdn_dp_device, bridge);
}

static unsigned int max_link_rate(struct cdn_mhdp_host host,
				  struct cdn_mhdp_sink sink)
{
	return min(host.link_rate, sink.link_rate);
}

static void cdn_mhdp_link_training_init(struct cdn_dp_device *dp)
{
	u32 reg32;

	drm_dp_dpcd_writeb(&dp->aux, DP_TRAINING_PATTERN_SET,
			   DP_TRAINING_PATTERN_DISABLE);

	/* Reset PHY configuration */
	reg32 = CDN_PHY_COMMON_CONFIG | CDN_PHY_TRAINING_TYPE(1);
	if (!(dp->host.lanes_cnt & SCRAMBLER_EN))
		reg32 |= CDN_PHY_SCRAMBLER_BYPASS;

	cdn_dp_register_write(dp, CDN_DPTX_PHY_CONFIG, reg32);

	cdn_dp_register_write(dp, CDN_DP_ENHNCD,
			      dp->sink.enhanced & dp->host.enhanced);

	cdn_dp_register_write(dp, CDN_DP_LANE_EN,
			      CDN_DP_LANE_EN_LANES(dp->link.num_lanes));

	drm_dp_link_configure(&dp->aux, &dp->link);

	cdn_dp_register_write(dp, CDN_DPTX_PHY_CONFIG, CDN_PHY_COMMON_CONFIG |
			      CDN_PHY_TRAINING_EN | CDN_PHY_TRAINING_TYPE(1) |
			      CDN_PHY_SCRAMBLER_BYPASS);

	drm_dp_dpcd_writeb(&dp->aux, DP_TRAINING_PATTERN_SET,
			   DP_TRAINING_PATTERN_1 | DP_LINK_SCRAMBLING_DISABLE);
}

static void cdn_mhdp_get_adjust_train(struct cdn_dp_device *dp,
				      u8 link_status[DP_LINK_STATUS_SIZE],
				      u8 lanes_data[DP_MAX_NUM_LANES])
{
	unsigned int i;
	u8 adjust, max_pre_emphasis, max_volt_swing;

	max_pre_emphasis = CDN_PRE_EMPHASIS(dp->host.pre_emphasis)
		<< DP_TRAIN_PRE_EMPHASIS_SHIFT;
	max_volt_swing = CDN_VOLT_SWING(dp->host.volt_swing);

	for (i = 0; i < dp->link.num_lanes; i++) {
		adjust = drm_dp_get_adjust_request_voltage(link_status, i);
		lanes_data[i] = min_t(u8, adjust, max_volt_swing);
		if (lanes_data[i] != adjust)
			lanes_data[i] |= DP_TRAIN_MAX_SWING_REACHED;

		adjust = drm_dp_get_adjust_request_pre_emphasis(link_status, i);
		lanes_data[i] |= min_t(u8, adjust, max_pre_emphasis);
		if ((lanes_data[i] >> DP_TRAIN_PRE_EMPHASIS_SHIFT) != adjust)
			lanes_data[i] |= DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;
	}
}

static void cdn_mhdp_adjust_requested_eq(struct cdn_dp_device *dp,
					 u8 link_status[DP_LINK_STATUS_SIZE])
{
	unsigned int i;
	u8 pre, volt, max_pre = CDN_VOLT_SWING(dp->host.volt_swing),
	   max_volt = CDN_PRE_EMPHASIS(dp->host.pre_emphasis);

	for (i = 0; i < dp->link.num_lanes; i++) {
		volt = drm_dp_get_adjust_request_voltage(link_status, i);
		pre = drm_dp_get_adjust_request_pre_emphasis(link_status, i);
		if (volt + pre > 3)
			drm_dp_set_adjust_request_voltage(link_status, i,
							  3 - pre);
		if (dp->host.volt_swing & CDN_FORCE_VOLT_SWING)
			drm_dp_set_adjust_request_voltage(link_status, i,
							  max_volt);
		if (dp->host.pre_emphasis & CDN_FORCE_PRE_EMPHASIS)
			drm_dp_set_adjust_request_pre_emphasis(link_status, i,
							       max_pre);
	}
}

static bool cdn_mhdp_link_training_channel_eq(struct cdn_dp_device *dp,
					      u8 eq_tps,
					      unsigned int training_interval)
{
	u8 lanes_data[DP_MAX_NUM_LANES], fail_counter_short = 0;
	u8 *dpcd;
	u32 reg32;

	dpcd = kzalloc(sizeof(u8) * DP_LINK_STATUS_SIZE, GFP_KERNEL);

	dev_dbg(dp->dev, "Link training - Starting EQ phase\n");

	/* Enable link training TPS[eq_tps] in PHY */
	reg32 = CDN_PHY_COMMON_CONFIG | CDN_PHY_TRAINING_EN |
		CDN_PHY_TRAINING_TYPE(eq_tps);
	if (eq_tps != 4)
		reg32 |= CDN_PHY_SCRAMBLER_BYPASS;
	cdn_dp_register_write(dp, CDN_DPTX_PHY_CONFIG, reg32);

	drm_dp_dpcd_writeb(&dp->aux, DP_TRAINING_PATTERN_SET,
			   (eq_tps != 4) ? eq_tps | DP_LINK_SCRAMBLING_DISABLE :
			   CDN_DP_TRAINING_PATTERN_4);

	drm_dp_dpcd_read_link_status(&dp->aux, dpcd);

	do {
		cdn_mhdp_get_adjust_train(dp, dpcd, lanes_data);

		cdn_dp_adjust_lt(dp, dp->link.num_lanes,
				 training_interval, lanes_data, dpcd);

		if (!drm_dp_clock_recovery_ok(dpcd, dp->link.num_lanes))
			goto err;

		if (drm_dp_channel_eq_ok(dpcd, dp->link.num_lanes)) {
			dev_dbg(dp->dev,
				"Link training: EQ phase succeeded\n");
			kfree(dpcd);
			return true;
		}

		fail_counter_short++;

		cdn_mhdp_adjust_requested_eq(dp, dpcd);
	} while (fail_counter_short < 5);

err:
	dev_dbg(dp->dev,
		"Link training - EQ phase failed for %d lanes and %d rate\n",
		dp->link.num_lanes, dp->link.rate);

	kfree(dpcd);
	return false;
}

static void cdn_mhdp_adjust_requested_cr(struct cdn_dp_device *dp,
					 u8 link_status[DP_LINK_STATUS_SIZE],
					 u8 *req_volt, u8 *req_pre)
{
	unsigned int i, max_volt = CDN_VOLT_SWING(dp->host.volt_swing),
		     max_pre = CDN_PRE_EMPHASIS(dp->host.pre_emphasis);

	for (i = 0; i < dp->link.num_lanes; i++) {
		if (dp->host.volt_swing & CDN_FORCE_VOLT_SWING)
			drm_dp_set_adjust_request_voltage(link_status, i,
							  max_volt);
		else
			drm_dp_set_adjust_request_voltage(link_status, i,
							  req_volt[i]);

		if (dp->host.pre_emphasis & CDN_FORCE_PRE_EMPHASIS)
			drm_dp_set_adjust_request_pre_emphasis(link_status, i,
							       max_pre);
		else
			drm_dp_set_adjust_request_pre_emphasis(link_status, i,
							       req_pre[i]);
	}
}

static void cdn_mhdp_validate_cr(struct cdn_dp_device *dp, bool *cr_done,
				 bool *same_before_adjust,
				 bool *max_swing_reached,
				 u8 before_cr[DP_LINK_STATUS_SIZE],
				 u8 after_cr[DP_LINK_STATUS_SIZE],
				 u8 *req_volt, u8 *req_pre)
{
	unsigned int i;
	u8 tmp, max_volt = CDN_VOLT_SWING(dp->host.volt_swing),
	   max_pre = CDN_PRE_EMPHASIS(dp->host.pre_emphasis), lane_status;
	bool same_pre, same_volt;

	*same_before_adjust = false;
	*max_swing_reached = false;
	*cr_done = true;

	for (i = 0; i < dp->link.num_lanes; i++) {
		tmp = drm_dp_get_adjust_request_voltage(after_cr, i);
		req_volt[i] = min_t(u8, tmp, max_volt);

		tmp = drm_dp_get_adjust_request_pre_emphasis(after_cr, i) >>
			DP_TRAIN_PRE_EMPHASIS_SHIFT;
		req_pre[i] = min_t(u8, tmp, max_pre);

		same_pre = (before_cr[i] & DP_TRAIN_PRE_EMPHASIS_MASK) ==
			(req_pre[i] << DP_TRAIN_PRE_EMPHASIS_SHIFT);
		same_volt = (before_cr[i] & DP_TRAIN_VOLTAGE_SWING_MASK) ==
			req_volt[i];
		if (same_pre && same_volt)
			*same_before_adjust = true;

		lane_status = drm_dp_get_lane_status(after_cr, i);
		if (!(lane_status & DP_LANE_CR_DONE)) {
			*cr_done = false;
			/* 3.1.5.2 in DP Standard v1.4. Table 3-1 */
			if (req_volt[i] + req_pre[i] >= 3) {
				*max_swing_reached = true;
				return;
			}
		}
	}
}

static bool cdn_mhdp_link_training_clock_recovery(struct cdn_dp_device *dp)
{
	u8 lanes_data[DP_MAX_NUM_LANES], fail_counter_short = 0,
	   fail_counter_cr_long = 0;
	u8 *dpcd;
	bool cr_done;

	dpcd = kzalloc(sizeof(u8) * DP_LINK_STATUS_SIZE, GFP_KERNEL);

	dev_dbg(dp->dev, "Link training starting CR phase\n");

	cdn_mhdp_link_training_init(dp);

	drm_dp_dpcd_read_link_status(&dp->aux, dpcd);

	do {
		u8 requested_adjust_volt_swing[DP_MAX_NUM_LANES] = {},
		   requested_adjust_pre_emphasis[DP_MAX_NUM_LANES] = {};
		bool same_before_adjust, max_swing_reached;

		cdn_mhdp_get_adjust_train(dp, dpcd, lanes_data);

		cdn_dp_adjust_lt(dp, dp->link.num_lanes, 100,
				 lanes_data, dpcd);

		cdn_mhdp_validate_cr(dp, &cr_done, &same_before_adjust,
				     &max_swing_reached, lanes_data, dpcd,
				     requested_adjust_volt_swing,
				     requested_adjust_pre_emphasis);

		if (max_swing_reached)
			goto err;

		if (cr_done) {
			dev_dbg(dp->dev,
				"Link training: CR phase succeeded\n");
			kfree(dpcd);
			return true;
		}

		/* Not all CR_DONE bits set */
		fail_counter_cr_long++;

		if (same_before_adjust) {
			fail_counter_short++;
			continue;
		}

		fail_counter_short = 0;
		/*
		 * Voltage swing/pre-emphasis adjust requested during CR phase
		 */
		cdn_mhdp_adjust_requested_cr(dp, dpcd,
					     requested_adjust_volt_swing,
					     requested_adjust_pre_emphasis);
	} while (fail_counter_short < 5 && fail_counter_cr_long < 10);

err:
	dev_dbg(dp->dev,
		"Link training: CR phase failed for %d lanes and %d rate\n",
		dp->link.num_lanes, dp->link.rate);

	kfree(dpcd);

	return false;
}

static void lower_link_rate(struct drm_dp_link *link)
{
	switch (drm_dp_link_rate_to_bw_code(link->rate)) {
	case DP_LINK_BW_2_7:
		link->rate = drm_dp_bw_code_to_link_rate(DP_LINK_BW_1_62);
		break;
	case DP_LINK_BW_5_4:
		link->rate = drm_dp_bw_code_to_link_rate(DP_LINK_BW_2_7);
		break;
	case DP_LINK_BW_8_1:
		link->rate = drm_dp_bw_code_to_link_rate(DP_LINK_BW_5_4);
		break;
	}
}

static u8 eq_training_pattern_supported(struct cdn_mhdp_host *host,
					struct cdn_mhdp_sink *sink)
{
	return fls(host->pattern_supp & sink->pattern_supp);
}

static int cdn_mhdp_link_training(struct cdn_dp_device *dp,
				  unsigned int video_mode,
				  unsigned int training_interval)
{
	u32 reg32;
	u8 eq_tps = eq_training_pattern_supported(&dp->host, &dp->sink);

	while (1) {
		if (!cdn_mhdp_link_training_clock_recovery(dp)) {
			if (drm_dp_link_rate_to_bw_code(dp->link.rate) !=
					DP_LINK_BW_1_62) {
				dev_dbg(dp->dev,
					"Reducing link rate during CR phase\n");
				lower_link_rate(&dp->link);
				drm_dp_link_configure(&dp->aux, &dp->link);

				continue;
			} else if (dp->link.num_lanes > 1) {
				dev_dbg(dp->dev,
					"Reducing lanes number during CR phase\n");
				dp->link.num_lanes >>= 1;
				dp->link.rate = max_link_rate(dp->host,
							      dp->sink);
				drm_dp_link_configure(&dp->aux, &dp->link);

				continue;
			}

			dev_dbg(dp->dev,
				"Link training failed during CR phase\n");
			goto err;
		}

		if (cdn_mhdp_link_training_channel_eq(dp, eq_tps,
						      training_interval))
			break;

		if (dp->link.num_lanes > 1) {
			dev_dbg(dp->dev,
				"Reducing lanes number during EQ phase\n");
			dp->link.num_lanes >>= 1;
			drm_dp_link_configure(&dp->aux, &dp->link);

			continue;
		} else if (drm_dp_link_rate_to_bw_code(dp->link.rate) !=
			   DP_LINK_BW_1_62) {
			dev_dbg(dp->dev,
				"Reducing link rate during EQ phase\n");
			lower_link_rate(&dp->link);
			drm_dp_link_configure(&dp->aux, &dp->link);

			continue;
		}

		dev_dbg(dp->dev, "Link training failed during EQ phase\n");
		goto err;
	}

	dev_dbg(dp->dev, "Link training successful\n");

	drm_dp_dpcd_writeb(&dp->aux, DP_TRAINING_PATTERN_SET,
			   (dp->host.lanes_cnt & SCRAMBLER_EN) ? 0 :
			   DP_LINK_SCRAMBLING_DISABLE);

	/* SW reset DPTX framer */
	cdn_dp_register_write(dp, CDN_DP_SW_RESET, 1);
	cdn_dp_register_write(dp, CDN_DP_SW_RESET, 0);

	/* Enable framer */
	/* FIXME: update when MST supported, BIT(2) */
	cdn_dp_register_write(dp, CDN_DP_FRAMER_GLOBAL_CONFIG,
			      CDN_DP_FRAMER_EN |
			      CDN_DP_NUM_LANES(dp->link.num_lanes) |
			      CDN_DP_DISABLE_PHY_RST |
			      CDN_DP_WR_FAILING_EDGE_VSYNC |
			      (video_mode ? CDN_DP_NO_VIDEO_MODE : 0));

	/* Reset PHY config */
	reg32 = CDN_PHY_COMMON_CONFIG | CDN_PHY_TRAINING_TYPE(1);
	if (!(dp->host.lanes_cnt & SCRAMBLER_EN))
		reg32 |= CDN_PHY_SCRAMBLER_BYPASS;
	cdn_dp_register_write(dp, CDN_DPTX_PHY_CONFIG, reg32);

	return 0;
err:
	/* Reset PHY config */
	reg32 = CDN_PHY_COMMON_CONFIG | CDN_PHY_TRAINING_TYPE(1);
	if (!(dp->host.lanes_cnt & SCRAMBLER_EN))
		reg32 |= CDN_PHY_SCRAMBLER_BYPASS;
	cdn_dp_register_write(dp, CDN_DPTX_PHY_CONFIG, reg32);

	drm_dp_dpcd_writeb(&dp->aux, DP_TRAINING_PATTERN_SET,
			   DP_TRAINING_PATTERN_DISABLE);

	return -EIO;
}

static void cdn_mhdp_enable(struct drm_bridge *bridge)
{
	struct cdn_dp_device *dp = bridge_to_dp(bridge);
	struct drm_display_mode *mode;
	struct drm_display_info *disp_info = &dp->connector.display_info;
	enum vic_pxl_encoding_format pxlfmt;
	int pxlclock;
	unsigned int rate, tu_size = 30, vs, vs_f, bpp, required_bandwidth,
		     available_bandwidth, dp_framer_sp = 0, msa_horizontal_1,
		     msa_vertical_1, bnd_hsync2vsync, hsync2vsync_pol_ctrl,
		     misc0 = 0, misc1 = 0, line_thresh = 0, pxl_repr,
		     front_porch, back_porch, msa_h0, msa_v0, hsync, vsync,
		     dp_vertical_1, line_thresh1, line_thresh2;
	u32 reg_rd_resp;

	unsigned int size = DP_RECEIVER_CAP_SIZE, dp_framer_global_config,
		     video_mode, training_interval_us;
	u8 reg0[size], reg8, amp[2];

	mode = &bridge->encoder->crtc->state->adjusted_mode;
	pxlclock = mode->crtc_clock;

	/*
	 * Upon power-on reset/device disconnection: [2:0] bits should be 0b001
	 * and [7:5] bits 0b000.
	 */
	drm_dp_dpcd_writeb(&dp->aux, DP_SET_POWER, 1);

	drm_dp_link_probe(&dp->aux, &dp->link);

	dev_dbg(dp->dev, "Set sink device power state via DPCD\n");
	drm_dp_link_power_up(&dp->aux, &dp->link);
	/* FIXME (CDNS): do we have to wait for 100ms before going on? */
	mdelay(100);

	dp->sink.link_rate = dp->link.rate;
	dp->sink.lanes_cnt = dp->link.num_lanes;
	dp->sink.enhanced = !!(dp->link.capabilities &
			DP_LINK_CAP_ENHANCED_FRAMING);

	drm_dp_dpcd_read(&dp->aux, DP_DPCD_REV, reg0, size);

	dp->sink.pattern_supp = PTS1 | PTS2;
	if (drm_dp_tps3_supported(reg0))
		dp->sink.pattern_supp |= PTS3;
	if (drm_dp_tps4_supported(reg0))
		dp->sink.pattern_supp |= PTS4;

	dp->sink.fast_link = !!(reg0[DP_MAX_DOWNSPREAD] &
			DP_NO_AUX_HANDSHAKE_LINK_TRAINING);

	dp->link.rate = max_link_rate(dp->host, dp->sink);
	dp->link.num_lanes = min_t(u8, dp->sink.lanes_cnt,
			dp->host.lanes_cnt & GENMASK(2, 0));

	reg8 = reg0[DP_TRAINING_AUX_RD_INTERVAL] &
		DP_TRAINING_AUX_RD_INTERVAL_MASK;
	switch (reg8) {
	case 0:
		training_interval_us = 400;
		break;
	case 1:
	case 2:
	case 3:
	case 4:
		training_interval_us = 4000 << (reg8 - 1);
		break;
	default:
		dev_err(dp->dev,
			"wrong training interval returned by DPCD: %d\n",
			reg8);
		return;
	}

	cdn_dp_register_read(dp, CDN_DP_FRAMER_GLOBAL_CONFIG, &reg_rd_resp);

	dp_framer_global_config = reg_rd_resp;

	video_mode = !(dp_framer_global_config & CDN_DP_NO_VIDEO_MODE);

	if (dp_framer_global_config & CDN_DP_FRAMER_EN)
		cdn_dp_register_write(dp, CDN_DP_FRAMER_GLOBAL_CONFIG,
				      dp_framer_global_config &
				      ~CDN_DP_FRAMER_EN);

	/* Spread AMP if required, enable 8b/10b coding */
	amp[0] = (dp->host.lanes_cnt & SSC) ? DP_SPREAD_AMP_0_5 : 0;
	amp[1] = DP_SET_ANSI_8B10B;
	drm_dp_dpcd_write(&dp->aux, DP_DOWNSPREAD_CTRL, amp, 2);

	if (dp->host.fast_link & dp->sink.fast_link) {
		/* FIXME: implement fastlink */
		DRM_DEV_DEBUG_KMS(dp->dev, "fastlink\n");
	} else {
		int lt_result = cdn_mhdp_link_training(dp, video_mode,
						       training_interval_us);
		if (lt_result) {
			dev_err(dp->dev, "Link training failed. Exiting.\n");
			return;
		}
	}

	rate = dp->link.rate / 1000;

	/* FIXME: what about Y_ONLY? how is it handled in the kernel? */
	if (disp_info->color_formats & DRM_COLOR_FORMAT_YCRCB444)
		pxlfmt = YCBCR_4_4_4;
	else if (disp_info->color_formats & DRM_COLOR_FORMAT_YCRCB422)
		pxlfmt = YCBCR_4_2_2;
	else if (disp_info->color_formats & DRM_COLOR_FORMAT_YCRCB420)
		pxlfmt = YCBCR_4_2_0;
	else
		pxlfmt = PXL_RGB;

	/* if YCBCR supported and stream not SD, use ITU709 */
	/* FIXME: handle ITU version with YCBCR420 when supported */
	if ((pxlfmt == YCBCR_4_4_4 ||
			pxlfmt == YCBCR_4_2_2) && mode->crtc_vdisplay >= 720)
		misc0 = DP_YCBCR_COEFFICIENTS_ITU709;

	switch (pxlfmt) {
	case PXL_RGB:
		bpp = disp_info->bpc * 3;
		pxl_repr = CDN_DP_FRAMER_RGB << CDN_DP_FRAMER_PXL_FORMAT;
		misc0 |= DP_COLOR_FORMAT_RGB;
		break;
	case YCBCR_4_4_4:
		bpp = disp_info->bpc * 3;
		pxl_repr = CDN_DP_FRAMER_YCBCR444 << CDN_DP_FRAMER_PXL_FORMAT;
		misc0 |= DP_COLOR_FORMAT_YCbCr444 | DP_TEST_DYNAMIC_RANGE_CEA;
		break;
	case YCBCR_4_2_2:
		bpp = disp_info->bpc * 2;
		pxl_repr = CDN_DP_FRAMER_YCBCR422 << CDN_DP_FRAMER_PXL_FORMAT;
		misc0 |= DP_COLOR_FORMAT_YCbCr422 | DP_TEST_DYNAMIC_RANGE_CEA;
		break;
	case YCBCR_4_2_0:
		bpp = disp_info->bpc * 3 / 2;
		pxl_repr = CDN_DP_FRAMER_YCBCR420 << CDN_DP_FRAMER_PXL_FORMAT;
		break;
	default:
		bpp = disp_info->bpc;
		pxl_repr = CDN_DP_FRAMER_Y_ONLY << CDN_DP_FRAMER_PXL_FORMAT;
	}

	switch (disp_info->bpc) {
	case 6:
		misc0 |= DP_TEST_BIT_DEPTH_6;
		pxl_repr |= CDN_DP_FRAMER_6_BPC;
		break;
	case 8:
		misc0 |= DP_TEST_BIT_DEPTH_8;
		pxl_repr |= CDN_DP_FRAMER_8_BPC;
		break;
	case 10:
		misc0 |= DP_TEST_BIT_DEPTH_10;
		pxl_repr |= CDN_DP_FRAMER_10_BPC;
		break;
	case 12:
		misc0 |= DP_TEST_BIT_DEPTH_12;
		pxl_repr |= CDN_DP_FRAMER_12_BPC;
		break;
	case 16:
		misc0 |= DP_TEST_BIT_DEPTH_16;
		pxl_repr |= CDN_DP_FRAMER_16_BPC;
		break;
	}

	/* find optimal tu_size */
	required_bandwidth = pxlclock * bpp / 8;
	available_bandwidth = dp->link.num_lanes * rate;
	do {
		tu_size += 2;

		vs_f = tu_size * required_bandwidth / available_bandwidth;
		vs = vs_f / 1000;
		vs_f = vs_f % 1000;
		/* FIXME downspreading? It's unused is what I've been told. */
	} while ((vs == 1 || ((vs_f > 850 || vs_f < 100) && vs_f != 0) ||
			tu_size - vs < 2) && tu_size < 64);

	if (vs > 64)
		return;

	bnd_hsync2vsync = CDN_IP_BYPASS_V_INTERFACE;
	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		bnd_hsync2vsync |= CDN_IP_DET_INTERLACE_FORMAT;

	cdn_dp_register_write(dp, BND_HSYNC2VSYNC, bnd_hsync2vsync);

	if (mode->flags & DRM_MODE_FLAG_INTERLACE &&
			mode->flags & DRM_MODE_FLAG_PHSYNC)
		hsync2vsync_pol_ctrl = CDN_H2V_HSYNC_POL_ACTIVE_LOW |
			CDN_H2V_VSYNC_POL_ACTIVE_LOW;
	else
		hsync2vsync_pol_ctrl = 0;

	cdn_dp_register_write(dp, CDN_HSYNC2VSYNC_POL_CTRL,
			      hsync2vsync_pol_ctrl);

	cdn_dp_register_write(dp, CDN_DP_FRAMER_TU, CDN_DP_FRAMER_TU_VS(vs) |
			      CDN_DP_FRAMER_TU_SIZE(tu_size) |
			      CDN_DP_FRAMER_TU_CNT_RST_EN);

	cdn_dp_register_write(dp, CDN_DP_FRAMER_PXL_REPR, pxl_repr);

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		dp_framer_sp |= CDN_DP_FRAMER_INTERLACE;
	if (mode->flags & DRM_MODE_FLAG_NHSYNC)
		dp_framer_sp |= CDN_DP_FRAMER_HSYNC_POL_LOW;
	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		dp_framer_sp |= CDN_DP_FRAMER_VSYNC_POL_LOW;
	cdn_dp_register_write(dp, CDN_DP_FRAMER_SP, dp_framer_sp);

	front_porch = mode->crtc_hsync_start - mode->crtc_hdisplay;
	back_porch = mode->crtc_htotal - mode->crtc_hsync_end;
	cdn_dp_register_write(dp, CDN_DP_FRONT_BACK_PORCH,
			      CDN_DP_FRONT_PORCH(front_porch) |
			      CDN_DP_BACK_PORCH(back_porch));

	cdn_dp_register_write(dp, CDN_DP_BYTE_COUNT,
			      mode->crtc_hdisplay * bpp / 8);

	msa_h0 = mode->crtc_htotal - mode->crtc_hsync_start;
	cdn_dp_register_write(dp, CDN_DP_MSA_HORIZONTAL_0,
			      CDN_DP_MSAH0_H_TOTAL(mode->crtc_htotal) |
			      CDN_DP_MSAH0_HSYNC_START(msa_h0));

	hsync = mode->crtc_hsync_end - mode->crtc_hsync_start;
	msa_horizontal_1 = CDN_DP_MSAH1_HSYNC_WIDTH(hsync) |
		CDN_DP_MSAH1_HDISP_WIDTH(mode->crtc_hdisplay);
	if (mode->flags & DRM_MODE_FLAG_NHSYNC)
		msa_horizontal_1 |= CDN_DP_MSAH1_HSYNC_POL_LOW;
	cdn_dp_register_write(dp, CDN_DP_MSA_HORIZONTAL_1, msa_horizontal_1);

	msa_v0 = mode->crtc_vtotal - mode->crtc_vsync_start;
	cdn_dp_register_write(dp, CDN_DP_MSA_VERTICAL_0,
			      CDN_DP_MSAV0_V_TOTAL(mode->crtc_vtotal) |
			      CDN_DP_MSAV0_VSYNC_START(msa_v0));

	vsync = mode->crtc_vsync_end - mode->crtc_vsync_start;
	msa_vertical_1 = CDN_DP_MSAV1_VSYNC_WIDTH(vsync) |
		CDN_DP_MSAV1_VDISP_WIDTH(mode->crtc_vdisplay);
	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		msa_vertical_1 |= CDN_DP_MSAV1_VSYNC_POL_LOW;
	cdn_dp_register_write(dp, CDN_DP_MSA_VERTICAL_1, msa_vertical_1);

	if ((mode->flags & DRM_MODE_FLAG_INTERLACE) &&
			mode->crtc_vtotal % 2 == 0)
		misc1 = DP_TEST_INTERLACED;
	if (pxlfmt == Y_ONLY)
		misc1 |= DP_TEST_COLOR_FORMAT_RAW_Y_ONLY;
	/* FIXME: use VSC SDP for Y420 */
	/* FIXME: (CDN) no code for Y420 in bare metal test */
	if (pxlfmt == YCBCR_4_2_0)
		misc1 = DP_TEST_VSC_SDP;

	cdn_dp_register_write(dp, CDN_DP_MSA_MISC, misc0 | (misc1 << 8));

	/* FIXME: to be changed if MST mode */
	cdn_dp_register_write(dp, CDN_DP_STREAM_CONFIG, 1);

	cdn_dp_register_write(dp, CDN_DP_HORIZONTAL,
			      CDN_DP_H_HSYNC_WIDTH(hsync) |
			      CDN_DP_H_H_TOTAL(mode->crtc_hdisplay));

	cdn_dp_register_write(dp, CDN_DP_VERTICAL_0,
			      CDN_DP_V0_VHEIGHT(mode->crtc_vdisplay) |
			      CDN_DP_V0_VSTART(msa_v0));

	dp_vertical_1 = CDN_DP_V1_VTOTAL(mode->crtc_vtotal);
	if ((mode->flags & DRM_MODE_FLAG_INTERLACE) &&
			mode->crtc_vtotal % 2 == 0)
		dp_vertical_1 |= CDN_DP_V1_VTOTAL_EVEN;

	cdn_dp_register_write(dp, CDN_DP_VERTICAL_1, dp_vertical_1);

	cdn_dp_register_write_field(dp, CDN_DP_VB_ID, 2, 1,
				    (mode->flags & DRM_MODE_FLAG_INTERLACE) ?
				    CDN_DP_VB_ID_INTERLACED : 0);

	line_thresh1 = ((vs + 1) << 5) * 8 / bpp;
	line_thresh2 = (pxlclock << 5) / 1000 / rate * (vs + 1) - (1 << 5);
	line_thresh = line_thresh1 - line_thresh2 / dp->link.num_lanes;
	line_thresh = (line_thresh >> 5) + 2;
	cdn_dp_register_write(dp, CDN_DP_LINE_THRESH,
			      line_thresh & GENMASK(5, 0));

	cdn_dp_register_write(dp, CDN_DP_RATE_GOVERNOR_STATUS,
			      CDN_DP_RG_TU_VS_DIFF((tu_size - vs > 3) ?
			      0 : tu_size - vs));

	cdn_dp_set_video_status(dp, 1);

	/* __simu_enable_mhdp(dp->regs); */
}

static void cdn_mhdp_disable(struct drm_bridge *bridge)
{
	struct cdn_dp_device *dp = bridge_to_dp(bridge);

	/* __simu_disable_mhdp(dp->regs); */

	cdn_dp_set_video_status(dp, 0);

	drm_dp_link_power_down(&dp->aux, &dp->link);
}

static int cdn_mhdp_attach(struct drm_bridge *bridge)
{
	struct cdn_dp_device *dp = bridge_to_dp(bridge);
	struct drm_connector *conn = &dp->connector;
	int ret;

	conn->polled = DRM_CONNECTOR_POLL_CONNECT |
		DRM_CONNECTOR_POLL_DISCONNECT;

	ret = drm_connector_init(bridge->dev, conn,
				 &cdn_dp_atomic_connector_funcs,
				 DRM_MODE_CONNECTOR_DisplayPort);
	if (ret) {
		dev_err(dp->dev, "failed to init connector\n");
		return ret;
	}

	drm_connector_helper_add(conn, &cdn_dp_connector_helper_funcs);

	ret = drm_mode_connector_attach_encoder(conn, bridge->encoder);
	if (ret) {
		dev_err(dp->dev, "failed to attach connector to encoder\n");
		return ret;
	}

	return 0;
}

static const struct drm_bridge_funcs cdn_mhdp_bridge_funcs = {
	.enable = cdn_mhdp_enable,
	.disable = cdn_mhdp_disable,
	.attach = cdn_mhdp_attach,
};

static ssize_t cdn_mhdp_transfer(struct drm_dp_aux *aux,
				 struct drm_dp_aux_msg *msg)
{
	struct cdn_dp_device *dp = dev_get_drvdata(aux->dev);
	int ret;

	if (msg->request != DP_AUX_NATIVE_WRITE &&
			msg->request != DP_AUX_NATIVE_READ)
		return -ENOTSUPP;

	if (msg->request == DP_AUX_NATIVE_WRITE) {
		int i;

		for (i = 0; i < msg->size; ++i) {
			ret = cdn_dp_dpcd_write(dp,
						msg->address + i,
						*((u8 *)msg->buffer + i));
			if (!ret)
				continue;

			DRM_DEV_ERROR(dp->dev, "Failed to write DPCD\n");

			return i;
		}
	} else {
		ret = cdn_dp_dpcd_read(dp, msg->address, msg->buffer,
				       msg->size);
		if (ret) {
			DRM_DEV_ERROR(dp->dev, "Failed to read DPCD\n");
			return 0;
		}
	}

	return msg->size;
}

int cdn_mhdp_probe(struct cdn_dp_device *dp)
{
	unsigned long clk_rate;
	const struct firmware *fw;
	int ret;
	u32 reg;

	dp->core_clk = devm_clk_get(dp->dev, "clk");
	if (IS_ERR(dp->core_clk)) {
		DRM_DEV_ERROR(dp->dev, "cannot get core_clk_dp\n");
		return PTR_ERR(dp->core_clk);
	}

	drm_dp_aux_init(&dp->aux);
	dp->aux.dev = dp->dev;
	dp->aux.transfer = cdn_mhdp_transfer;

	clk_rate = clk_get_rate(dp->core_clk);
	cdn_dp_set_fw_clk(dp, clk_rate);

	ret = request_firmware(&fw, CDN_DP_FIRMWARE, dp->dev);
	if (ret) {
		dev_err(dp->dev, "failed to load firmware (%s), ret: %d\n",
			CDN_DP_FIRMWARE, ret);
		return ret;
	}

	memcpy_toio(dp->regs + ADDR_IMEM, fw->data, fw->size);

	release_firmware(fw);

	/* __simu_configure_mhdp(dp->regs); */

	/* un-reset ucpu */
	writel(0, dp->regs + APB_CTRL);

	/* check the keep alive register to make sure fw working */
	ret = readx_poll_timeout(readl, dp->regs + KEEP_ALIVE,
				 reg, reg, 2000, FW_ALIVE_TIMEOUT_US);
	if (ret < 0) {
		DRM_DEV_ERROR(dp->dev, "failed to loaded the FW reg = %x\n",
			      reg);
		return -EINVAL;
	}

	/*
	 * FIXME (CDNS): how are the characteristics/features of the host
	 * defined? Will they be always hardcoded?
	 */
	/* FIXME: link rate 2.7; num_lanes = 2, */
	/* FIXME: read capabilities from PHY */
	/* FIXME: get number of lanes */
	dp->host.link_rate = drm_dp_bw_code_to_link_rate(DP_LINK_BW_5_4);
	dp->host.lanes_cnt = LANE_4 | SCRAMBLER_EN;
	dp->host.volt_swing = VOLTAGE_LEVEL_3;
	dp->host.pre_emphasis = PRE_EMPHASIS_LEVEL_2;
	dp->host.pattern_supp = PTS1 | PTS2 | PTS3 | PTS4;
	dp->host.fast_link = 0;
	dp->host.lane_mapping = LANE_MAPPING_FLIPPED;
	dp->host.enhanced = true;

	dp->bridge.of_node = dp->dev->of_node;
	dp->bridge.funcs = &cdn_mhdp_bridge_funcs;

	ret = cdn_dp_set_firmware_active(dp, true);
	if (ret) {
		DRM_DEV_ERROR(dp->dev, "active ucpu failed: %d\n", ret);
		return ret;
	}

	/* __simu_phy_reset(dp->regs); */

	ret = readl_poll_timeout(dp->regs + SW_EVENTS0, reg,
				 reg & DPTX_HPD_EVENT, 500,
				 HPD_EVENT_TIMEOUT);
	if (ret) {
		dev_err(dp->dev, "no HPD received %d\n", reg);
		return -ENODEV;
	}

	drm_bridge_add(&dp->bridge);

	/* __simu_configure2_mhdp(dp->regs); */

	return 0;
}

static int cdn_dp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct of_device_id *match;
	struct resource *res;
	struct cdn_dp_data *dp_data;
	struct cdn_dp_port *port;
	struct cdn_dp_device *dp;
	struct extcon_dev *extcon;
	struct phy *phy;
	int i;

	dp = devm_kzalloc(dev, sizeof(*dp), GFP_KERNEL);
	if (!dp)
		return -ENOMEM;
	dp->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	dp->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(dp->regs)) {
		DRM_DEV_ERROR(dev, "ioremap reg failed\n");
		return PTR_ERR(dp->regs);
	}

	match = of_match_node(cdn_dp_dt_ids, pdev->dev.of_node);
	if (!match)
		return -EINVAL;

	dp->mhdp_ip = !strcmp("cdns,mhdp", match->compatible);

	if (dp->mhdp_ip) {
		cdn_mhdp_probe(dp);
		goto skip_phy_init;
	}

	dp_data = (struct cdn_dp_data *)match->data;

	for (i = 0; i < dp_data->max_phy; i++) {
		extcon = extcon_get_edev_by_phandle(dev, i);
		phy = devm_of_phy_get_by_index(dev, dev->of_node, i);

		if (PTR_ERR(extcon) == -EPROBE_DEFER ||
		    PTR_ERR(phy) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		if (IS_ERR(extcon) || IS_ERR(phy))
			continue;

		port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
		if (!port)
			return -ENOMEM;

		port->extcon = extcon;
		port->phy = phy;
		port->dp = dp;
		port->id = i;
		dp->port[dp->ports++] = port;
	}

	if (!dp->ports) {
		DRM_DEV_ERROR(dev, "missing extcon or phy\n");
		return -EINVAL;
	}

	mutex_init(&dp->lock);
	dev_set_drvdata(dev, dp);

skip_phy_init:

	cdn_dp_audio_codec_init(dp, dev);

	return component_add(dev, &cdn_dp_component_ops);
}

static int cdn_dp_remove(struct platform_device *pdev)
{
	struct cdn_dp_device *dp = platform_get_drvdata(pdev);
	int ret;

	platform_device_unregister(dp->audio_pdev);

	if (dp->mhdp_ip) {
		drm_bridge_remove(&dp->bridge);

		ret = cdn_dp_set_firmware_active(dp, false);
		if (ret) {
			DRM_DEV_ERROR(dp->dev, "disabling fw failed: %d\n",
				      ret);
			return ret;
		}
	} else {
		cdn_dp_suspend(dp->dev);
		component_del(&pdev->dev, &cdn_dp_component_ops);
	}

	return 0;
}

static void cdn_dp_shutdown(struct platform_device *pdev)
{
	struct cdn_dp_device *dp = platform_get_drvdata(pdev);

	cdn_dp_suspend(dp->dev);
}

static const struct dev_pm_ops cdn_dp_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(cdn_dp_suspend,
				cdn_dp_resume)
};

struct platform_driver cdn_dp_driver = {
	.probe = cdn_dp_probe,
	.remove = cdn_dp_remove,
	.shutdown = cdn_dp_shutdown,
	.driver = {
		   .name = "cdn-dp",
		   .owner = THIS_MODULE,
		   .of_match_table = of_match_ptr(cdn_dp_dt_ids),
		   .pm = &cdn_dp_pm_ops,
	},
};
