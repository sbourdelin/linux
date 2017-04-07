/*
 * HID driver for Topaz signature pads suitable for fbcon
 *
 * Copyright (c) 2017 Alyssa Rosenzweig
 *
 * Author: Alyssa Rosenzweig <alyssa@rosenzweig.io>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/fb.h>
#include <linux/uaccess.h>

#include "hid-ids.h"

#define MODE_CLEAR		0
#define MODE_XOR		1
#define MODE_OPAQUE		2
#define MODE_TRANSPARENT	3

struct topazfb_par {
	struct hid_device *hid;
};

static struct fb_fix_screeninfo topazfb_fix = {
	.id = "topaz",
	.type = FB_TYPE_PACKED_PIXELS,
	.visual = FB_VISUAL_MONO01,
	.accel = FB_ACCEL_NONE,
	.smem_len = (320*240>>3),
	.line_length = (320 >> 3)
};

static int topaz_send(struct hid_device *dev, u8 *packet, size_t sz)
{
	int ret;

	u8 *buf = kmemdup(packet, sz, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;

	if (!dev->ll_driver->output_report)
		return -ENODEV;

	hid_hw_output_report(dev, buf, sz);
	kfree(buf);

	return 0;
}

static int topaz_blit8(struct hid_device *dev, u16 x, u16 y, void *data)
{
	u8 packet[11 + 8] = {
		0xF2, 0x07, 0x02,		// command + mode
		(x & 0xFF00) >> 8, x & 0x00FF,  // coordinates
		(y & 0xFF00) >> 8, y & 0x00FF,
		0, 8,				// size
		0, 8
	};

	memcpy(packet + 11, data, 8);

	return topaz_send(dev, packet, sizeof(packet));
}

/* blit arbitrarily large bitmap, slicing it into 8x8 chunks */

static int topaz_bitmap(struct hid_device *dev,
			u16 x, u16 y, u16 w, u16 h,
			const u8 *data)
{
	u8 block[8];

	int dx, dy, row;

	/* compute line length, padding as necessary */
	int l = ((w + 7) >> 3) << 3;

	for (dx = 0; dx < w; dx += 8) {
		for (dy = 0; dy < h; dy += 8) {
			/* blit 8x8 block */

			memset(block, 0, sizeof(block));

			for (row = 0; row < 8 && dy + row < h; ++row)
				block[row] = data[((row + dy) * l + dx) >> 3];

			if (topaz_blit8(dev, x + dx, y + dy, block))
				return -1;
		}
	}

	return 0;
}

/* fast fill or clear, depending on which mode is specified */

static int topaz_rectangle(struct hid_device *dev,
			   u16 x, u16 y, u16 w, u16 h,
			   u8 mode)
{
	u8 packet[11] = {
		0xFF, 0x12, mode,
		(x & 0xFF00) >> 8, x & 0xFF,
		(y & 0xFF00) >> 8, y & 0xFF,
		(w & 0xFF00) >> 8, w & 0xFF,
		(h & 0xFF00) >> 8, h & 0xFF,
	};

	return topaz_send(dev, packet, sizeof(packet));
}

static int topaz_clear(struct hid_device *dev)
{
	return topaz_rectangle(dev, 0, 0, 320, 240, 2);
}

static int topaz_backlight(struct hid_device *dev, bool on)
{
	u8 packet[] = { 0x81, 0x02 | (!on) };

	return topaz_send(dev, packet, sizeof(packet));
}

static void topazfb_imageblit(struct fb_info *p, const struct fb_image *image)
{
	if (image->depth != 1) {
		pr_err("Cannot blit nonmonochrome image\n");
		return;
	}

	topaz_bitmap(((struct topazfb_par *) p->par)->hid,
			image->dx, image->dy,
			image->width, image->height,
			image->data);
}

static void topazfb_fillrect(struct fb_info *p,
			     const struct fb_fillrect *region)
{
	topaz_rectangle(((struct topazfb_par *) p->par)->hid,
			region->dx, region->dy,
			region->width, region->height,
			region->rop == ROP_XOR ? MODE_XOR : MODE_OPAQUE);
}

static int topazfb_blank(int mode, struct fb_info *p)
{
	return topaz_backlight(((struct topazfb_par *) p->par)->hid,
				mode == FB_BLANK_UNBLANK);
}

static struct fb_ops topazfb_ops = {
	.owner = THIS_MODULE,
	.fb_fillrect = topazfb_fillrect,
	.fb_imageblit = topazfb_imageblit,
	.fb_blank = topazfb_blank
};

static int topazfb_probe(struct hid_device *dev)
{
	struct fb_info *info;
	struct topazfb_par *par;

	info = framebuffer_alloc(sizeof(struct topazfb_par), NULL);

	par = info->par;
	par->hid = dev;

	info->fbops = &topazfb_ops;
	info->fix = topazfb_fix;
	info->flags = FBINFO_DEFAULT | FBINFO_HWACCEL_IMAGEBLIT
				     | FBINFO_HWACCEL_FILLRECT;

	/* this is only a pseudo frame buffer device */
	info->screen_base = NULL;
	info->screen_size = 0;

	/* LBK766 is 320x240; other models may differ */
	info->var.xres = 320;
	info->var.yres = 240;

	info->var.bits_per_pixel = 1;
	info->var.grayscale = 1;
	info->var.red.offset = 0;
	info->var.red.length = 1;
	info->var.green.offset = 0;
	info->var.green.length = 1;
	info->var.blue.offset = 0;
	info->var.blue.length = 1;
	info->var.transp.offset = 0;
	info->var.transp.length = 1;

	if (register_framebuffer(info) < 0)
		return -EINVAL;

	return 0;

}

static int topaz_probe(struct hid_device *dev, const struct hid_device_id *id)
{
	int ret;

	ret = hid_parse(dev);
	if (ret)
		goto done;

	ret = hid_hw_start(dev, HID_CONNECT_DEFAULT);
	if (ret)
		goto done;

	ret = hid_hw_open(dev);
	if (ret)
		goto done;

	ret = topaz_clear(dev);
	if (ret)
		goto done;

	ret = topazfb_probe(dev);
	if (ret)
		goto done;
done:
	return ret;
}

static void topaz_remove(struct hid_device *dev)
{
	hid_hw_stop(dev);
}

static const struct hid_device_id topaz_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_TOPAZ, USB_DEVICE_ID_TOPAZ_LBK766) },
	{ }
};

MODULE_DEVICE_TABLE(hid, topaz_devices);

static struct hid_driver topaz_driver = {
	.name = "topaz",
	.id_table = topaz_devices,
	.probe = topaz_probe,
	.remove = topaz_remove
};

module_hid_driver(topaz_driver);

MODULE_AUTHOR("Alyssa Rosenzweig <alyssa@rosenzweig.io>");
MODULE_DESCRIPTION("HID driver for Topaz signature pads");
MODULE_LICENSE("GPL");
