/*
 * Copyright (C) 2012 Red Hat
 * based in parts on udlfb.c:
 * Copyright (C) 2009 Roberto De Ioris <roberto@unbit.it>
 * Copyright (C) 2009 Jaya Kumar <jayakumar.lkml@gmail.com>
 * Copyright (C) 2009 Bernie Thompson <bernie@plugable.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_edid.h>
#include <drm/drm_crtc_helper.h>
#include "udl_drv.h"

#define NR_USB_REQUEST_CHANNEL 0x12

/* dummy connector to just get EDID,
   all UDL appear to have a DVI-D */

static u8 *udl_get_edid(struct udl_device *udl)
{
	u8 *block;
	char *rbuf;
	int ret, i;

	block = kmalloc(EDID_LENGTH, GFP_KERNEL);
	if (block == NULL)
		return NULL;

	rbuf = kmalloc(2, GFP_KERNEL);
	if (rbuf == NULL)
		goto error;

	for (i = 0; i < EDID_LENGTH; i++) {
		ret = usb_control_msg(udl->udev,
				      usb_rcvctrlpipe(udl->udev, 0), (0x02),
				      (0x80 | (0x02 << 5)), i << 8, 0xA1, rbuf, 2,
				      HZ);
		if (ret < 1) {
			DRM_ERROR("Read EDID byte %d failed err %x\n", i, ret);
			goto error;
		}
		block[i] = rbuf[1];
	}

	kfree(rbuf);
	return block;

error:
	kfree(block);
	kfree(rbuf);
	return NULL;
}

/*
 * This is necessary before we can communicate with the display controller.
 */
static int udl_select_std_channel(struct udl_device *udl)
{
	int ret;
	u8 set_def_chn[] = {0x57, 0xCD, 0xDC, 0xA7,
			    0x1C, 0x88, 0x5E, 0x15,
			    0x60, 0xFE, 0xC6, 0x97,
			    0x16, 0x3D, 0x47, 0xF2};

	ret = usb_control_msg(udl->udev,
			      usb_sndctrlpipe(udl->udev, 0),
			      NR_USB_REQUEST_CHANNEL,
			      (USB_DIR_OUT | USB_TYPE_VENDOR), 0, 0,
			      set_def_chn, sizeof(set_def_chn),
			      USB_CTRL_SET_TIMEOUT);
	return ret < 0 ? ret : 0;
}

static int udl_get_modes(struct drm_connector *connector)
{
	struct udl_device *udl = connector->dev->dev_private;
	struct edid *edid;
	int ret;

	edid = (struct edid *)udl_get_edid(udl);
	if (!edid) {
		drm_mode_connector_update_edid_property(connector, NULL);
		return 0;
	}

	/*
	 * We only read the main block, but if the monitor reports extension
	 * blocks then the drm edid code expects them to be present, so patch
	 * the extension count to 0.
	 */
	edid->checksum += edid->extensions;
	edid->extensions = 0;

	drm_mode_connector_update_edid_property(connector, edid);
	ret = drm_add_edid_modes(connector, edid);
	kfree(edid);
	return ret;
}

static int udl_mode_valid(struct drm_connector *connector,
			  struct drm_display_mode *mode)
{
	struct udl_device *udl = connector->dev->dev_private;
	if (!udl->sku_pixel_limit)
		return 0;

	if (mode->vdisplay * mode->hdisplay > udl->sku_pixel_limit)
		return MODE_VIRTUAL_Y;

	return 0;
}

static enum drm_connector_status
udl_detect(struct drm_connector *connector, bool force)
{
	if (drm_device_is_unplugged(connector->dev))
		return connector_status_disconnected;
	return connector_status_connected;
}

static struct drm_encoder*
udl_best_single_encoder(struct drm_connector *connector)
{
	int enc_id = connector->encoder_ids[0];
	return drm_encoder_find(connector->dev, enc_id);
}

static int udl_connector_set_property(struct drm_connector *connector,
				      struct drm_property *property,
				      uint64_t val)
{
	return 0;
}

static void udl_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
	kfree(connector);
}

static const struct drm_connector_helper_funcs udl_connector_helper_funcs = {
	.get_modes = udl_get_modes,
	.mode_valid = udl_mode_valid,
	.best_encoder = udl_best_single_encoder,
};

static const struct drm_connector_funcs udl_connector_funcs = {
	.dpms = drm_helper_connector_dpms,
	.detect = udl_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = udl_connector_destroy,
	.set_property = udl_connector_set_property,
};

int udl_connector_init(struct drm_device *dev, struct drm_encoder *encoder)
{
	struct drm_connector *connector;
	int ret;

	connector = kzalloc(sizeof(struct drm_connector), GFP_KERNEL);
	if (!connector)
		return -ENOMEM;

	drm_connector_init(dev, connector, &udl_connector_funcs, DRM_MODE_CONNECTOR_DVII);
	drm_connector_helper_add(connector, &udl_connector_helper_funcs);

	ret = udl_select_std_channel(connector->dev->dev_private);
	if (ret)
		DRM_ERROR("Selecting channel failed err %x\n", ret);

	drm_connector_register(connector);
	drm_mode_connector_attach_encoder(connector, encoder);

	drm_object_attach_property(&connector->base,
				      dev->mode_config.dirty_info_property,
				      1);
	return 0;
}
