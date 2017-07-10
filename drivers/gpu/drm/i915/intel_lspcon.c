/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 *
 */
#include <drm/drm_edid.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_dp_dual_mode_helper.h>
#include "intel_drv.h"

/* LSPCON OUI Vendor ID(signatures) */
#define LSPCON_VENDOR_PARADE_OUI 0x001CF8
#define LSPCON_VENDOR_MCA_OUI 0x0060AD

/* AUX addresses to write AVI IF into */
#define LSPCON_MCA_AVI_IF_WRITE_OFFSET 0x5C0
#define LSPCON_MCA_AVI_IF_CTRL 0x5DF
#define  LSPCON_MCA_AVI_IF_KICKOFF (1 << 0)
#define  LSPCON_MCA_AVI_IF_HANDLED (1 << 1)

#define LSPCON_PARADE_AVI_IF_WRITE_OFFSET 0x516
#define LSPCON_PARADE_AVI_IF_CTRL 0x51E
#define  LSPCON_PARADE_AVI_IF_KICKOFF (1 << 7)
#define LSPCON_PARADE_AVI_IF_STATUS 0x51F
#define  LSPCON_PARADE_AVI_IF_HANDLED (2 << 6)

static struct intel_dp *lspcon_to_intel_dp(struct intel_lspcon *lspcon)
{
	struct intel_digital_port *dig_port =
		container_of(lspcon, struct intel_digital_port, lspcon);

	return &dig_port->dp;
}

static const char *lspcon_mode_name(enum drm_lspcon_mode mode)
{
	switch (mode) {
	case DRM_LSPCON_MODE_PCON:
		return "PCON";
	case DRM_LSPCON_MODE_LS:
		return "LS";
	case DRM_LSPCON_MODE_INVALID:
		return "INVALID";
	default:
		MISSING_CASE(mode);
		return "INVALID";
	}
}

static enum drm_lspcon_mode lspcon_get_current_mode(struct intel_lspcon *lspcon)
{
	enum drm_lspcon_mode current_mode;
	struct i2c_adapter *adapter = &lspcon_to_intel_dp(lspcon)->aux.ddc;

	if (drm_lspcon_get_mode(adapter, &current_mode)) {
		DRM_ERROR("Error reading LSPCON mode\n");
		return DRM_LSPCON_MODE_INVALID;
	}
	return current_mode;
}

static enum drm_lspcon_mode lspcon_wait_mode(struct intel_lspcon *lspcon,
					     enum drm_lspcon_mode mode)
{
	enum drm_lspcon_mode current_mode;

	current_mode = lspcon_get_current_mode(lspcon);
	if (current_mode == mode || current_mode == DRM_LSPCON_MODE_INVALID)
		goto out;

	DRM_DEBUG_KMS("Waiting for LSPCON mode %s to settle\n",
		      lspcon_mode_name(mode));

	wait_for((current_mode = lspcon_get_current_mode(lspcon)) == mode ||
		 current_mode == DRM_LSPCON_MODE_INVALID, 100);
	if (current_mode != mode)
		DRM_DEBUG_KMS("LSPCON mode hasn't settled\n");

out:
	DRM_DEBUG_KMS("Current LSPCON mode %s\n",
		      lspcon_mode_name(current_mode));

	return current_mode;
}

static int lspcon_change_mode(struct intel_lspcon *lspcon,
			      enum drm_lspcon_mode mode)
{
	int err;
	enum drm_lspcon_mode current_mode;
	struct i2c_adapter *adapter = &lspcon_to_intel_dp(lspcon)->aux.ddc;

	err = drm_lspcon_get_mode(adapter, &current_mode);
	if (err) {
		DRM_ERROR("Error reading LSPCON mode\n");
		return err;
	}

	if (current_mode == mode) {
		DRM_DEBUG_KMS("Current mode = desired LSPCON mode\n");
		return 0;
	}

	err = drm_lspcon_set_mode(adapter, mode);
	if (err < 0) {
		DRM_ERROR("LSPCON mode change failed\n");
		return err;
	}

	lspcon->mode = mode;
	DRM_DEBUG_KMS("LSPCON mode changed done\n");
	return 0;
}

static bool lspcon_wake_native_aux_ch(struct intel_lspcon *lspcon)
{
	uint8_t rev;

	if (drm_dp_dpcd_readb(&lspcon_to_intel_dp(lspcon)->aux, DP_DPCD_REV,
			      &rev) != 1) {
		DRM_DEBUG_KMS("Native AUX CH down\n");
		return false;
	}

	DRM_DEBUG_KMS("Native AUX CH up, DPCD version: %d.%d\n",
		      rev >> 4, rev & 0xf);

	return true;
}

static bool lspcon_probe(struct intel_lspcon *lspcon)
{
	enum drm_dp_dual_mode_type adaptor_type;
	struct i2c_adapter *adapter = &lspcon_to_intel_dp(lspcon)->aux.ddc;
	enum drm_lspcon_mode expected_mode;
	uint32_t vendor_oui;

	expected_mode = lspcon_wake_native_aux_ch(lspcon) ?
			DRM_LSPCON_MODE_PCON : DRM_LSPCON_MODE_LS;

	/* Lets probe the adaptor and check its type */
	adaptor_type = drm_dp_dual_mode_detect(adapter);
	if (adaptor_type != DRM_DP_DUAL_MODE_LSPCON) {
		DRM_DEBUG_KMS("No LSPCON detected, found %s\n",
			drm_dp_get_dual_mode_type_name(adaptor_type));
		return false;
	}

	/* Yay ... got a LSPCON device */
	DRM_DEBUG_KMS("LSPCON detected\n");
	lspcon->mode = lspcon_wait_mode(lspcon, expected_mode);

	/* Check if this is a Parade LSPCON or MCA LSPCON */
	vendor_oui = drm_lspcon_get_vendor_oui(adapter);
	switch (vendor_oui) {
	case LSPCON_VENDOR_MCA_OUI:
		lspcon->vendor = LSPCON_VENDOR_MCA;
		DRM_DEBUG_KMS("Vendor: Mega Chips\n");
		break;

	case LSPCON_VENDOR_PARADE_OUI:
		lspcon->vendor = LSPCON_VENDOR_PARADE;
		DRM_DEBUG_KMS("Vendor: Parade Tech\n");
		break;

	default:
		DRM_ERROR("Can't read OUI /Invalid OUI\n");
		return false;
	}

	lspcon->active = true;
	return true;
}

static void lspcon_resume_in_pcon_wa(struct intel_lspcon *lspcon)
{
	struct intel_dp *intel_dp = lspcon_to_intel_dp(lspcon);
	struct intel_digital_port *dig_port = dp_to_dig_port(intel_dp);
	struct drm_i915_private *dev_priv = to_i915(dig_port->base.base.dev);
	unsigned long start = jiffies;

	while (1) {
		if (intel_digital_port_connected(dev_priv, dig_port)) {
			DRM_DEBUG_KMS("LSPCON recovering in PCON mode after %u ms\n",
				      jiffies_to_msecs(jiffies - start));
			return;
		}

		if (time_after(jiffies, start + msecs_to_jiffies(1000)))
			break;

		usleep_range(10000, 15000);
	}

	DRM_DEBUG_KMS("LSPCON DP descriptor mismatch after resume\n");
}

bool lspcon_ycbcr420_config(struct drm_connector *connector,
			    struct intel_crtc_state *config,
			    int *clock_12bpc, int *clock_8bpc)
{
	struct drm_display_info *info = &connector->display_info;
	struct drm_display_mode *mode = &config->base.adjusted_mode;

	if (drm_mode_is_420_only(info, mode)) {
		return intel_hdmi_ycbcr420_config(connector, config,
					  clock_12bpc, clock_8bpc);
	}

	return false;
}

static bool _lspcon_write_infoframe_parade(struct drm_dp_aux *aux,
					   uint8_t *buffer, ssize_t len)
{
	u8 avi_if_ctrl;
	u8 avi_if_status;
	u8 count = 0;
	u8 retry = 5;
	u8 avi_buf[8] = {0, };
	uint16_t reg;
	ssize_t ret;

	while (count++ < 4) {

		do {
			/* Is LSPCON FW ready */
			reg = LSPCON_PARADE_AVI_IF_CTRL;
			ret = drm_dp_dpcd_read(aux, reg, &avi_if_ctrl, 1);
			if (ret < 0) {
				DRM_ERROR("DPCD read failed, add:0x%x\n", reg);
				return false;
			}

			if (avi_if_ctrl & LSPCON_PARADE_AVI_IF_KICKOFF)
				break;
			usleep_range(100, 200);
		} while (--retry);

		if (!(avi_if_ctrl & LSPCON_PARADE_AVI_IF_KICKOFF)) {
			DRM_ERROR("LSPCON FW not ready for infoframes\n");
			return false;
		}

		/*
		 * AVI Infoframe contains 31 bytes of data:
		 *	HB0 to HB2   (3 bytes header)
		 *	PB0 to PB27 (28 bytes data)
		 * As per Parade spec, while sending first block (8bytes),
		 * byte 0 is kept for request token no, and byte1 - byte7
		 * contain frame data. So we have to pack frame like this:
		 *	first block of 8 bytes: <token> <HB0-HB2> <PB0-PB3>
		 *	next 3 blocks: <PB4-PB27>
		 */
		if (count)
			memcpy(avi_buf, buffer + count * 8 - 1, 8);
		else {
			avi_buf[0] = 1;
			memcpy(&avi_buf[1], buffer, 7);
		}

		/* Write 8 bytes of data at a time */
		reg = LSPCON_PARADE_AVI_IF_WRITE_OFFSET;
		ret = drm_dp_dpcd_write(aux, reg, avi_buf, 8);
		if (ret < 0) {
			DRM_ERROR("DPCD write failed, add:0x%x\n", reg);
			return false;
		}

		/*
		 * While sending a block of 8 byes, we need to inform block
		 * number to FW, by programming bits[1:0] of ctrl reg with
		 * block number
		 */
		avi_if_ctrl = 0x80 + count;
		reg = LSPCON_PARADE_AVI_IF_CTRL;
		ret = drm_dp_dpcd_write(aux, reg, &avi_if_ctrl, 1);
		if (ret < 0) {
			DRM_ERROR("DPCD write failed, add:0x%x\n", reg);
			return false;
		}
	}

	/* Check LSPCON FW status */
	reg = LSPCON_PARADE_AVI_IF_STATUS;
	ret = drm_dp_dpcd_read(aux, reg, &avi_if_status, 1);
	if (ret < 0) {
		DRM_ERROR("DPCD write failed, address 0x%x\n", reg);
		return false;
	}

	if (avi_if_status & LSPCON_PARADE_AVI_IF_HANDLED)
		DRM_DEBUG_KMS("AVI IF handled by FW\n");

	return true;
}

static bool _lspcon_write_infoframe_mca(struct drm_dp_aux *aux,
					uint8_t *buffer, ssize_t len)
{
	int ret;
	uint32_t val = 0;
	uint16_t reg;
	uint8_t *data = buffer;

	reg = LSPCON_MCA_AVI_IF_WRITE_OFFSET;
	while (val < len) {
		ret = drm_dp_dpcd_write(aux, reg, data, 1);
		if (ret < 0) {
			DRM_ERROR("DPCD write failed, add:0x%x\n", reg);
			return false;
		}
		val++; reg++; data++;
	}

	val = 0;
	reg = LSPCON_MCA_AVI_IF_CTRL;
	ret = drm_dp_dpcd_read(aux, reg, &val, 1);
	if (ret < 0) {
		DRM_ERROR("DPCD read failed, address 0x%x\n", reg);
		return false;
	}

	/* Indicate LSPCON chip about infoframe, clear bit 1 and set bit 0 */
	val &= ~LSPCON_MCA_AVI_IF_HANDLED;
	val |= LSPCON_MCA_AVI_IF_KICKOFF;

	ret = drm_dp_dpcd_write(aux, reg, &val, 1);
	if (ret < 0) {
		DRM_ERROR("DPCD read failed, address 0x%x\n", reg);
		return false;
	}

	val = 0;
	ret = drm_dp_dpcd_read(aux, reg, &val, 1);
	if (ret < 0) {
		DRM_ERROR("DPCD read failed, address 0x%x\n", reg);
		return false;
	}

	if (val == LSPCON_MCA_AVI_IF_HANDLED)
		DRM_DEBUG_KMS("AVI IF handled by FW\n");

	return true;
}

void lspcon_write_infoframe(struct drm_encoder *encoder,
				  const struct intel_crtc_state *crtc_state,
				  union hdmi_infoframe *frame)
{
	bool ret;
	ssize_t len;
	uint8_t buf[VIDEO_DIP_DATA_SIZE];
	struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
	struct intel_lspcon *lspcon = enc_to_intel_lspcon(encoder);

	len = hdmi_infoframe_pack(frame, buf, sizeof(buf));
	if (len < 0) {
		DRM_ERROR("Failed to pack AVI IF\n");
		return;
	}

	if (lspcon->vendor == LSPCON_VENDOR_MCA)
		ret = _lspcon_write_infoframe_mca(&intel_dp->aux, buf, len);
	else
		ret = _lspcon_write_infoframe_parade(&intel_dp->aux, buf, len);

	if (!ret)
		DRM_ERROR("Failed to write AVI infoframes\n");
	else
		DRM_DEBUG_DRIVER("AVI infoframes updated successfully\n");
}

void lspcon_resume(struct intel_lspcon *lspcon)
{
	enum drm_lspcon_mode expected_mode;

	if (lspcon_wake_native_aux_ch(lspcon)) {
		expected_mode = DRM_LSPCON_MODE_PCON;
		lspcon_resume_in_pcon_wa(lspcon);
	} else {
		expected_mode = DRM_LSPCON_MODE_LS;
	}

	if (lspcon_wait_mode(lspcon, expected_mode) == DRM_LSPCON_MODE_PCON)
		return;

	if (lspcon_change_mode(lspcon, DRM_LSPCON_MODE_PCON))
		DRM_ERROR("LSPCON resume failed\n");
	else
		DRM_DEBUG_KMS("LSPCON resume success\n");
}

void lspcon_wait_pcon_mode(struct intel_lspcon *lspcon)
{
	lspcon_wait_mode(lspcon, DRM_LSPCON_MODE_PCON);
}

bool lspcon_init(struct intel_digital_port *intel_dig_port)
{
	struct intel_dp *dp = &intel_dig_port->dp;
	struct intel_lspcon *lspcon = &intel_dig_port->lspcon;
	struct drm_device *dev = intel_dig_port->base.base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_connector *connector = &dp->attached_connector->base;

	if (!IS_GEN9(dev_priv)) {
		DRM_ERROR("LSPCON is supported on GEN9 only\n");
		return false;
	}

	lspcon->active = false;
	lspcon->mode = DRM_LSPCON_MODE_INVALID;

	if (!lspcon_probe(lspcon)) {
		DRM_ERROR("Failed to probe lspcon\n");
		return false;
	}

	/*
	* In the SW state machine, lets Put LSPCON in PCON mode only.
	* In this way, it will work with both HDMI 1.4 sinks as well as HDMI
	* 2.0 sinks.
	*/
	if (lspcon->active && lspcon->mode != DRM_LSPCON_MODE_PCON) {
		if (lspcon_change_mode(lspcon, DRM_LSPCON_MODE_PCON) < 0) {
			DRM_ERROR("LSPCON mode change to PCON failed\n");
			return false;
		}
	}

	if (!intel_dp_read_dpcd(dp)) {
		DRM_ERROR("LSPCON DPCD read failed\n");
		return false;
	}

	connector->ycbcr_420_allowed = true;
	lspcon->set_infoframes = intel_ddi_set_avi_infoframe;
	lspcon->write_infoframe = lspcon_write_infoframe;
	drm_dp_read_desc(&dp->aux, &dp->desc, drm_dp_is_branch(dp->dpcd));

	DRM_DEBUG_KMS("Success: LSPCON init\n");
	return true;
}
