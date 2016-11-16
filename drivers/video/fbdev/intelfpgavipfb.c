/*
 * Copyright (C) 2016 Intel Corporation. All rights reserved
 *
 * intelfpgavipfb.c -- Intel Video and Image Processing(VIP)
 * Frame Buffer II driver
 *
 * This is based on a driver made by Thomas Chou <thomas@wytron.com.tw> and
 * Winteler Goossens <wintelergoossens@home.nl> This driver supports the
 * Intel VIP Frame Buffer II component.  More info on the hardware can be
 * found in the Intel Video and Image Processing Suite User Guide at
 * http://www.intelera.com/literature/ug/ug_vip.pdf.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/dma-mapping.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define PALETTE_SIZE	256
#define DRIVER_NAME	"intelvipfb"

/* control registers */
#define INTVIPFB2_CONTROL			0
#define INTVIPFB2_STATUS			0x4
#define INTVIPFB2_INTERRUPT			0x8
#define INTVIPFB2_FRAME_COUNTER		0xC
#define INTVIPFB2_FRAME_DROP		0x10
#define INTVIPFB2_FRAME_INFO		0x14
#define INTVIPFB2_FRAME_START		0x18
#define INTVIPFB2_FRAME_READER		0x1C

#define BITS_PER_PIXEL		32

struct intelvipfb_dev {
	struct platform_device *pdev;
	struct fb_info info;
	struct resource *reg_res;
	void __iomem *base;
	int mem_word_width;
	u32 pseudo_palette[PALETTE_SIZE];
};

static int intelvipfb_setcolreg(unsigned int regno, unsigned int red,
				unsigned int green, unsigned int blue,
				unsigned int transp, struct fb_info *info)
{
	/*
	 *  Set a single color register. The values supplied have a 32 bit
	 *  magnitude.
	 *  Return != 0 for invalid regno.
	 */

	if (regno > 255)
		return 1;

	red >>= 8;
	green >>= 8;
	blue >>= 8;
	if (regno < 255) {
		((u32 *)info->pseudo_palette)[regno] =
		((red & 255) << 16) | ((green & 255) << 8) | (blue & 255);
	}

	return 0;
}

static struct fb_ops intelvipfb_ops = {
	.owner = THIS_MODULE,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
	.fb_setcolreg = intelvipfb_setcolreg,
};

static int intelvipfb_of_setup(struct intelvipfb_dev *fbdev)
{
	struct device_node *np = fbdev->pdev->dev.of_node;
	int ret;
	u32 bits_per_color;

	ret = of_property_read_u32(np, "max-width", &fbdev->info.var.xres);
	if (ret) {
		dev_err(&fbdev->pdev->dev,
			"Missing required parameter 'max-width'");
		return ret;
	}
	fbdev->info.var.xres_virtual = fbdev->info.var.xres,

	ret = of_property_read_u32(np, "max-height", &fbdev->info.var.yres);
	if (ret) {
		dev_err(&fbdev->pdev->dev,
			"Missing required parameter 'max-height'");
		return ret;
	}
	fbdev->info.var.yres_virtual = fbdev->info.var.yres;

	ret = of_property_read_u32(np, "bits-per-color", &bits_per_color);
	if (ret) {
		dev_err(&fbdev->pdev->dev,
			"Missing required parameter 'bits-per-color'");
		return ret;
	}
	if (bits_per_color != 8) {
		dev_err(&fbdev->pdev->dev,
			"bits-per-color is set to %i.  Currently only 8 is supported.",
			bits_per_color);
		return -ENODEV;
	}
	fbdev->info.var.bits_per_pixel = BITS_PER_PIXEL;

	ret = of_property_read_u32(np, "mem-word-width",
				   &fbdev->mem_word_width);
	if (ret) {
		dev_err(&fbdev->pdev->dev,
			"Missing required parameter 'mem-word-width'");
		return ret;
	}
	if (!(fbdev->mem_word_width >= BITS_PER_PIXEL &&
	      fbdev->mem_word_width % BITS_PER_PIXEL == 0)) {
		dev_err(&fbdev->pdev->dev,
			"mem-word-width is set to %i.  must be >= 32 and multiple of 32.",
			fbdev->mem_word_width);
		return -ENODEV;
	}

	return 0;
}

static void intelvipfb_start_hw(struct intelvipfb_dev *fbdev)
{
	/*
	 * The frameinfo variable has to correspond to the size of the VIP Suite
	 * Frame Reader register 7 which will determine the maximum size used
	 * in this frameinfo
	 */
	u32 frameinfo = readl(fbdev->base +
				INTVIPFB2_FRAME_READER) & 0x00ffffff;

	writel(frameinfo, fbdev->base + INTVIPFB2_FRAME_INFO);
	writel(fbdev->info.fix.smem_start, fbdev->base + INTVIPFB2_FRAME_START);
	/* Finally set the control register to 1 to start streaming */
	writel(1, fbdev->base + INTVIPFB2_CONTROL);
}

static void intelvipfb_disable_hw(struct intelvipfb_dev *fbdev)
{
	/* set the control register to 0 to stop streaming */
	writel(0, fbdev->base + INTVIPFB2_CONTROL);
}

static int intelvipfb_setup_fb_info(struct intelvipfb_dev *fbdev)
{
	struct fb_info *info = &fbdev->info;
	int ret;

	strcpy(info->fix.id, DRIVER_NAME);
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.accel = FB_ACCEL_NONE;

	info->fbops = &intelvipfb_ops;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.height = -1;
	info->var.width = -1;
	info->var.vmode = FB_VMODE_NONINTERLACED;

	ret = intelvipfb_of_setup(fbdev);
	if (ret)
		return ret;

	/* settings for 32bit pixels */
	info->var.red.offset = 16;
	info->var.red.length = 8;
	info->var.red.msb_right = 0;
	info->var.green.offset = 8;
	info->var.green.length = 8;
	info->var.green.msb_right = 0;
	info->var.blue.offset = 0;
	info->var.blue.length = 8;
	info->var.blue.msb_right = 0;

	info->fix.line_length = (info->var.xres *
		(info->var.bits_per_pixel >> 3));
	info->fix.smem_len = info->fix.line_length * info->var.yres;

	info->pseudo_palette = fbdev->pseudo_palette;
	info->flags = FBINFO_FLAG_DEFAULT;

	return 0;
}

static int intelvipfb_probe(struct platform_device *pdev)
{
	int retval;
	void *fbmem_virt;
	struct intelvipfb_dev *fbdev;

	fbdev = devm_kzalloc(&pdev->dev, sizeof(*fbdev), GFP_KERNEL);
	if (!fbdev)
		return -ENOMEM;

	fbdev->pdev = pdev;
	fbdev->reg_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!fbdev->reg_res)
		return -ENODEV;

	fbdev->base = devm_ioremap_resource(&pdev->dev, fbdev->reg_res);
	if (IS_ERR(fbdev->base)) {
		dev_err(&pdev->dev, "devm_ioremap_resource failed\n");
		retval = PTR_ERR(fbdev->base);
		return -ENODEV;
	}

	retval = intelvipfb_setup_fb_info(fbdev);

	fbmem_virt = dma_alloc_coherent(NULL,
					fbdev->info.fix.smem_len,
					(void *)&fbdev->info.fix.smem_start,
					GFP_KERNEL);
	if (!fbmem_virt) {
		dev_err(&pdev->dev,
			"intelvipfb: unable to allocate %d Bytes fb memory\n",
			fbdev->info.fix.smem_len);
		return retval;
	}

	fbdev->info.screen_base = fbmem_virt;

	retval = fb_alloc_cmap(&fbdev->info.cmap, PALETTE_SIZE, 0);
	if (retval < 0)
		goto err_dma_free;

	platform_set_drvdata(pdev, fbdev);

	intelvipfb_start_hw(fbdev);

	retval = register_framebuffer(&fbdev->info);
	if (retval < 0)
		goto err_dealloc_cmap;

	dev_info(&pdev->dev, "fb%d: %s frame buffer device at 0x%x+0x%x\n",
		 fbdev->info.node, fbdev->info.fix.id,
		 (unsigned int)fbdev->info.fix.smem_start,
		 fbdev->info.fix.smem_len);

	return 0;

err_dealloc_cmap:
	fb_dealloc_cmap(&fbdev->info.cmap);
err_dma_free:
	dma_free_coherent(NULL, fbdev->info.fix.smem_len, fbmem_virt,
			  fbdev->info.fix.smem_start);
	return retval;
}

static int intelvipfb_remove(struct platform_device *dev)
{
	struct intelvipfb_dev *fbdev = platform_get_drvdata(dev);

	if (fbdev) {
		unregister_framebuffer(&fbdev->info);
		fb_dealloc_cmap(&fbdev->info.cmap);
		dma_free_coherent(NULL, fbdev->info.fix.smem_len,
				  fbdev->info.screen_base,
				  fbdev->info.fix.smem_start);
		intelvipfb_disable_hw(fbdev);
	}
	return 0;
}

static const struct of_device_id intelvipfb_match[] = {
	{ .compatible = "intel,vip-frame-buffer2" },
	{},
};
MODULE_DEVICE_TABLE(of, intelvipfb_match);

static struct platform_driver intelvipfb_driver = {
	.probe = intelvipfb_probe,
	.remove = intelvipfb_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = intelvipfb_match,
	},
};
module_platform_driver(intelvipfb_driver);

MODULE_DESCRIPTION("Intel VIP Frame Buffer II driver");
MODULE_AUTHOR("Chris Rauer <christopher.rauer@intel.com>");
MODULE_LICENSE("GPL v2");

