/*
 * Silicon Motion SM7XX frame buffer device
 *
 * Copyright (C) 2006 Silicon Motion Technology Corp.
 * Authors:  Ge Wang, gewang@siliconmotion.com
 *	     Boyod boyod.yang@siliconmotion.com.cn
 *
 * Copyright (C) 2009 Lemote, Inc.
 * Author:   Wu Zhangjin, wuzhangjin@gmail.com
 *
 * Copyright (C) 2011 Igalia, S.L.
 * Author:   Javier M. Mellid <jmunhoz@igalia.com>
 *
 * Copyright (C) 2014, 2019 Yifeng Li.
 * Author:   Yifeng Li <tomli@tomli.me>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of this archive for
 * more details.
 *
 * Framebuffer driver for Silicon Motion SM710, SM712, SM721 and SM722 chips
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *    Copyright (c) 2009, 2010 Miodrag Vallat.
 *
 *    Permission to use, copy, modify, and distribute this software for any
 *    purpose with or without fee is hereby granted, provided that the above
 *    copyright notice and this permission notice appear in all copies.
 *
 *    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/io.h>
#include <linux/fb.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/console.h>
#include <linux/screen_info.h>
#include <linux/delay.h>

#include <linux/pm.h>

#include "sm712.h"

/*
 * Private structure
 */
struct smtcfb_info {
	struct pci_dev *pdev;
	struct fb_info *fb;
	u16 chip_id;
	u8  chip_rev_id;

	void __iomem *lfb;	/* linear frame buffer */
	void __iomem *dp_port;  /* drawing processor data port */
	void __iomem *dp_regs;	/* drawing processor control regs */
	void __iomem *vp_regs;	/* video processor control regs */
	void __iomem *cp_regs;	/* capture processor control regs */
	void __iomem *mmio;	/* memory map IO port */

	bool accel;		/* whether to actually use drawing processor */

	u_int width;
	u_int height;
	u_int hz;

	u32 colreg[17];
};

void __iomem *smtc_regbaseaddress;	/* Memory Map IO starting address */
void __iomem *smtc_dprbaseaddress;	/* DPR, 2D control registers */

static const struct fb_var_screeninfo smtcfb_var = {
	.xres           = 1024,
	.yres           = 600,
	.xres_virtual   = 1024,
	.yres_virtual   = 600,
	.bits_per_pixel = 16,
	.red            = {16, 8, 0},
	.green          = {8, 8, 0},
	.blue           = {0, 8, 0},
	.activate       = FB_ACTIVATE_NOW,
	.height         = -1,
	.width          = -1,
	.vmode          = FB_VMODE_NONINTERLACED,
	.nonstd         = 0,
	.accel_flags    = FB_ACCELF_TEXT,
};

static struct fb_fix_screeninfo smtcfb_fix = {
	.id             = "smXXXfb",
	.type           = FB_TYPE_PACKED_PIXELS,
	.visual         = FB_VISUAL_TRUECOLOR,
	.line_length    = 800 * 3,
	.accel          = FB_ACCEL_SMI_LYNX,
	.type_aux       = 0,
	.xpanstep       = 0,
	.ypanstep       = 0,
	.ywrapstep      = 0,
};

struct vesa_mode {
	char index[6];
	u16  lfb_width;
	u16  lfb_height;
	u16  lfb_depth;
};

static const struct vesa_mode vesa_mode_table[] = {
	{"0x311", 640,  480,  16},
	{"0x314", 800,  600,  16},
	{"0x317", 1024, 768,  16},

	{"0x312", 640,  480,  24},
	{"0x315", 800,  600,  24},
	{"0x318", 1024, 768,  24},

	{"0x329", 640,  480,  32},
	{"0x32e", 800,  600,  32},
	{"0x338", 1024, 768,  32},
};

/**********************************************************************
			 SM712 Mode table.

 The modesetting in sm712fb is an ugly hack. First, all the registers
 are programmed by hardcoded register arrays, which makes it difficult
 to support different variations of color depths, refresh rates, CRT/LCD
 panel, etc of the same resolution. Second, it means the standard
 fb_find_mode() cannot be used and a confusing non-standard "vga="
 parameter is needed. Third, there's only minimum differences between
 some modes, yet around 70 lines of code and 100 registers are needed to
 be indepentently specified for each mode. Fourth, the register between
 some modes are inconsistent: the register configuration of different
 color depths in 640 x 480 modes are identical, but for 800 x 600 modes
 it's completely different. Also, some modes can drive the LCD panel
 properly yet some other modes will only show a white screen of death on
 the LCD panel. Fifth, there is a specific hack for Lemote Loongson 8089D
 laptop, the 1024x768, 16-bit color mode was modified to drive its LCD panel
 and changed to 1024x600, but the original mode was not preserved, so
 1024x768 16-bit color mode is completely unsupported. And previously,
 some modes are listed, such as 1280x1024 modes, but never supported by
 the register configuration arrays, so they are now removed. And some modes
 are partially implemented but neither listed nor supported, i.e. the 8-bit
 color modes, so they have been removed from vesa_mode_table, too.

 I'm not the original author of the code, fixing these problems requires a
 complete rewrite of modesetting code, which is well-beyond my motivation.

 See Documentation/fb/sm712fb.txt for more information.
**********************************************************************/
static const struct modeinit vgamode[] = {
	{
		/*  mode#0: 640 x 480  16Bpp  60Hz */
		640, 480, 16, 60,
		/*  Init_MISC */
		0xE3,
		{	/*  Init_SR0_SR4 */
			0x03, 0x01, 0x0F, 0x00, 0x0E,
		},
		{	/*  Init_SR10_SR24 */
			0xFF, 0xBE, 0xEF, 0xFF, 0x00, 0x0E, 0x17, 0x2C,
			0x99, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xC4, 0x30, 0x02, 0x01, 0x01,
		},
		{	/*  Init_SR30_SR75 */
			0x32, 0x03, 0xA0, 0x09, 0xC0, 0x32, 0x32, 0x32,
			0x32, 0x32, 0x32, 0x32, 0x00, 0x00, 0x03, 0xFF,
			0x00, 0xFC, 0x00, 0x00, 0x20, 0x18, 0x00, 0xFC,
			0x20, 0x0C, 0x44, 0x20, 0x00, 0x32, 0x32, 0x32,
			0x04, 0x24, 0x63, 0x4F, 0x52, 0x0B, 0xDF, 0xEA,
			0x04, 0x50, 0x19, 0x32, 0x32, 0x00, 0x00, 0x32,
			0x01, 0x80, 0x7E, 0x1A, 0x1A, 0x00, 0x00, 0x00,
			0x50, 0x03, 0x74, 0x14, 0x07, 0x82, 0x07, 0x04,
			0x00, 0x45, 0x30, 0x30, 0x40, 0x30,
		},
		{	/*  Init_SR80_SR93 */
			0xFF, 0x07, 0x00, 0x6F, 0x7F, 0x7F, 0xFF, 0x32,
			0xF7, 0x00, 0x00, 0x00, 0xEF, 0xFF, 0x32, 0x32,
			0x00, 0x00, 0x00, 0x00,
		},
		{	/*  Init_SRA0_SRAF */
			0x00, 0xFF, 0xBF, 0xFF, 0xFF, 0xED, 0xED, 0xED,
			0x7B, 0xFF, 0xFF, 0xFF, 0xBF, 0xEF, 0xFF, 0xDF,
		},
		{	/*  Init_GR00_GR08 */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
			0xFF,
		},
		{	/*  Init_AR00_AR14 */
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
			0x41, 0x00, 0x0F, 0x00, 0x00,
		},
		{	/*  Init_CR00_CR18 */
			0x5F, 0x4F, 0x4F, 0x00, 0x53, 0x1F, 0x0B, 0x3E,
			0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xEA, 0x0C, 0xDF, 0x50, 0x40, 0xDF, 0x00, 0xE3,
			0xFF,
		},
		{	/*  Init_CR30_CR4D */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x03, 0x20,
			0x00, 0x00, 0x00, 0x40, 0x00, 0xE7, 0xFF, 0xFD,
			0x5F, 0x4F, 0x00, 0x54, 0x00, 0x0B, 0xDF, 0x00,
			0xEA, 0x0C, 0x2E, 0x00, 0x4F, 0xDF,
		},
		{	/*  Init_CR90_CRA7 */
			0x56, 0xDD, 0x5E, 0xEA, 0x87, 0x44, 0x8F, 0x55,
			0x0A, 0x8F, 0x55, 0x0A, 0x00, 0x00, 0x18, 0x00,
			0x11, 0x10, 0x0B, 0x0A, 0x0A, 0x0A, 0x0A, 0x00,
		},
	},
	{
		/*  mode#1: 640 x 480  24Bpp  60Hz */
		640, 480, 24, 60,
		/*  Init_MISC */
		0xE3,
		{	/*  Init_SR0_SR4 */
			0x03, 0x01, 0x0F, 0x00, 0x0E,
		},
		{	/*  Init_SR10_SR24 */
			0xFF, 0xBE, 0xEF, 0xFF, 0x00, 0x0E, 0x17, 0x2C,
			0x99, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xC4, 0x30, 0x02, 0x01, 0x01,
		},
		{	/*  Init_SR30_SR75 */
			0x32, 0x03, 0xA0, 0x09, 0xC0, 0x32, 0x32, 0x32,
			0x32, 0x32, 0x32, 0x32, 0x00, 0x00, 0x03, 0xFF,
			0x00, 0xFC, 0x00, 0x00, 0x20, 0x18, 0x00, 0xFC,
			0x20, 0x0C, 0x44, 0x20, 0x00, 0x32, 0x32, 0x32,
			0x04, 0x24, 0x63, 0x4F, 0x52, 0x0B, 0xDF, 0xEA,
			0x04, 0x50, 0x19, 0x32, 0x32, 0x00, 0x00, 0x32,
			0x01, 0x80, 0x7E, 0x1A, 0x1A, 0x00, 0x00, 0x00,
			0x50, 0x03, 0x74, 0x14, 0x07, 0x82, 0x07, 0x04,
			0x00, 0x45, 0x30, 0x30, 0x40, 0x30,
		},
		{	/*  Init_SR80_SR93 */
			0xFF, 0x07, 0x00, 0x6F, 0x7F, 0x7F, 0xFF, 0x32,
			0xF7, 0x00, 0x00, 0x00, 0xEF, 0xFF, 0x32, 0x32,
			0x00, 0x00, 0x00, 0x00,
		},
		{	/*  Init_SRA0_SRAF */
			0x00, 0xFF, 0xBF, 0xFF, 0xFF, 0xED, 0xED, 0xED,
			0x7B, 0xFF, 0xFF, 0xFF, 0xBF, 0xEF, 0xFF, 0xDF,
		},
		{	/*  Init_GR00_GR08 */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
			0xFF,
		},
		{	/*  Init_AR00_AR14 */
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
			0x41, 0x00, 0x0F, 0x00, 0x00,
		},
		{	/*  Init_CR00_CR18 */
			0x5F, 0x4F, 0x4F, 0x00, 0x53, 0x1F, 0x0B, 0x3E,
			0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xEA, 0x0C, 0xDF, 0x50, 0x40, 0xDF, 0x00, 0xE3,
			0xFF,
		},
		{	/*  Init_CR30_CR4D */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x03, 0x20,
			0x00, 0x00, 0x00, 0x40, 0x00, 0xE7, 0xFF, 0xFD,
			0x5F, 0x4F, 0x00, 0x54, 0x00, 0x0B, 0xDF, 0x00,
			0xEA, 0x0C, 0x2E, 0x00, 0x4F, 0xDF,
		},
		{	/*  Init_CR90_CRA7 */
			0x56, 0xDD, 0x5E, 0xEA, 0x87, 0x44, 0x8F, 0x55,
			0x0A, 0x8F, 0x55, 0x0A, 0x00, 0x00, 0x18, 0x00,
			0x11, 0x10, 0x0B, 0x0A, 0x0A, 0x0A, 0x0A, 0x00,
		},
	},
	{
		/*  mode#0: 640 x 480  32Bpp  60Hz */
		640, 480, 32, 60,
		/*  Init_MISC */
		0xE3,
		{	/*  Init_SR0_SR4 */
			0x03, 0x01, 0x0F, 0x00, 0x0E,
		},
		{	/*  Init_SR10_SR24 */
			0xFF, 0xBE, 0xEF, 0xFF, 0x00, 0x0E, 0x17, 0x2C,
			0x99, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xC4, 0x30, 0x02, 0x01, 0x01,
		},
		{	/*  Init_SR30_SR75 */
			0x32, 0x03, 0xA0, 0x09, 0xC0, 0x32, 0x32, 0x32,
			0x32, 0x32, 0x32, 0x32, 0x00, 0x00, 0x03, 0xFF,
			0x00, 0xFC, 0x00, 0x00, 0x20, 0x18, 0x00, 0xFC,
			0x20, 0x0C, 0x44, 0x20, 0x00, 0x32, 0x32, 0x32,
			0x04, 0x24, 0x63, 0x4F, 0x52, 0x0B, 0xDF, 0xEA,
			0x04, 0x50, 0x19, 0x32, 0x32, 0x00, 0x00, 0x32,
			0x01, 0x80, 0x7E, 0x1A, 0x1A, 0x00, 0x00, 0x00,
			0x50, 0x03, 0x74, 0x14, 0x07, 0x82, 0x07, 0x04,
			0x00, 0x45, 0x30, 0x30, 0x40, 0x30,
		},
		{	/*  Init_SR80_SR93 */
			0xFF, 0x07, 0x00, 0x6F, 0x7F, 0x7F, 0xFF, 0x32,
			0xF7, 0x00, 0x00, 0x00, 0xEF, 0xFF, 0x32, 0x32,
			0x00, 0x00, 0x00, 0x00,
		},
		{	/*  Init_SRA0_SRAF */
			0x00, 0xFF, 0xBF, 0xFF, 0xFF, 0xED, 0xED, 0xED,
			0x7B, 0xFF, 0xFF, 0xFF, 0xBF, 0xEF, 0xFF, 0xDF,
		},
		{	/*  Init_GR00_GR08 */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
			0xFF,
		},
		{	/*  Init_AR00_AR14 */
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
			0x41, 0x00, 0x0F, 0x00, 0x00,
		},
		{	/*  Init_CR00_CR18 */
			0x5F, 0x4F, 0x4F, 0x00, 0x53, 0x1F, 0x0B, 0x3E,
			0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xEA, 0x0C, 0xDF, 0x50, 0x40, 0xDF, 0x00, 0xE3,
			0xFF,
		},
		{	/*  Init_CR30_CR4D */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x03, 0x20,
			0x00, 0x00, 0x00, 0x40, 0x00, 0xE7, 0xFF, 0xFD,
			0x5F, 0x4F, 0x00, 0x54, 0x00, 0x0B, 0xDF, 0x00,
			0xEA, 0x0C, 0x2E, 0x00, 0x4F, 0xDF,
		},
		{	/*  Init_CR90_CRA7 */
			0x56, 0xDD, 0x5E, 0xEA, 0x87, 0x44, 0x8F, 0x55,
			0x0A, 0x8F, 0x55, 0x0A, 0x00, 0x00, 0x18, 0x00,
			0x11, 0x10, 0x0B, 0x0A, 0x0A, 0x0A, 0x0A, 0x00,
		},
	},

	{	/*  mode#2: 800 x 600  16Bpp  60Hz */
		800, 600, 16, 60,
		/*  Init_MISC */
		0x2B,
		{	/*  Init_SR0_SR4 */
			0x03, 0x01, 0x0F, 0x03, 0x0E,
		},
		{	/*  Init_SR10_SR24 */
			0xFF, 0xBE, 0xEE, 0xFF, 0x00, 0x0E, 0x17, 0x2C,
			0x99, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xC4, 0x30, 0x02, 0x01, 0x01,
		},
		{	/*  Init_SR30_SR75 */
			0x34, 0x03, 0x20, 0x09, 0xC0, 0x24, 0x24, 0x24,
			0x24, 0x24, 0x24, 0x24, 0x00, 0x00, 0x03, 0xFF,
			0x00, 0xFC, 0x00, 0x00, 0x20, 0x38, 0x00, 0xFC,
			0x20, 0x0C, 0x44, 0x20, 0x00, 0x24, 0x24, 0x24,
			0x04, 0x48, 0x83, 0x63, 0x68, 0x72, 0x57, 0x58,
			0x04, 0x55, 0x59, 0x24, 0x24, 0x00, 0x00, 0x24,
			0x01, 0x80, 0x7A, 0x1A, 0x1A, 0x00, 0x00, 0x00,
			0x50, 0x03, 0x74, 0x14, 0x1C, 0x85, 0x35, 0x13,
			0x02, 0x45, 0x30, 0x35, 0x40, 0x20,
		},
		{	/*  Init_SR80_SR93 */
			0x00, 0x00, 0x00, 0x6F, 0x7F, 0x7F, 0xFF, 0x24,
			0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x24, 0x24,
			0x00, 0x00, 0x00, 0x00,
		},
		{	/*  Init_SRA0_SRAF */
			0x00, 0xFF, 0xBF, 0xFF, 0xFF, 0xED, 0xED, 0xED,
			0x7B, 0xFF, 0xFF, 0xFF, 0xBF, 0xEF, 0xBF, 0xDF,
		},
		{	/*  Init_GR00_GR08 */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
			0xFF,
		},
		{	/*  Init_AR00_AR14 */
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
			0x41, 0x00, 0x0F, 0x00, 0x00,
		},
		{	/*  Init_CR00_CR18 */
			0x7F, 0x63, 0x63, 0x00, 0x68, 0x18, 0x72, 0xF0,
			0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x58, 0x0C, 0x57, 0x64, 0x40, 0x57, 0x00, 0xE3,
			0xFF,
		},
		{	/*  Init_CR30_CR4D */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x33, 0x03, 0x20,
			0x00, 0x00, 0x00, 0x40, 0x00, 0xE7, 0xBF, 0xFD,
			0x7F, 0x63, 0x00, 0x69, 0x18, 0x72, 0x57, 0x00,
			0x58, 0x0C, 0xE0, 0x20, 0x63, 0x57,
		},
		{	/*  Init_CR90_CRA7 */
			0x56, 0x4B, 0x5E, 0x55, 0x86, 0x9D, 0x8E, 0xAA,
			0xDB, 0x2A, 0xDF, 0x33, 0x00, 0x00, 0x18, 0x00,
			0x20, 0x1F, 0x1A, 0x19, 0x0F, 0x0F, 0x0F, 0x00,
		},
	},
	{	/*  mode#3: 800 x 600  24Bpp  60Hz */
		800, 600, 24, 60,
		0x2B,
		{	/*  Init_SR0_SR4 */
			0x03, 0x01, 0x0F, 0x03, 0x0E,
		},
		{	/*  Init_SR10_SR24 */
			0xFF, 0xBE, 0xEE, 0xFF, 0x00, 0x0E, 0x17, 0x2C,
			0x99, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xC4, 0x30, 0x02, 0x01, 0x01,
		},
		{	/*  Init_SR30_SR75 */
			0x36, 0x03, 0x20, 0x09, 0xC0, 0x36, 0x36, 0x36,
			0x36, 0x36, 0x36, 0x36, 0x00, 0x00, 0x03, 0xFF,
			0x00, 0xFC, 0x00, 0x00, 0x20, 0x18, 0x00, 0xFC,
			0x20, 0x0C, 0x44, 0x20, 0x00, 0x36, 0x36, 0x36,
			0x04, 0x48, 0x83, 0x63, 0x68, 0x72, 0x57, 0x58,
			0x04, 0x55, 0x59, 0x36, 0x36, 0x00, 0x00, 0x36,
			0x01, 0x80, 0x7E, 0x1A, 0x1A, 0x00, 0x00, 0x00,
			0x50, 0x03, 0x74, 0x14, 0x1C, 0x85, 0x35, 0x13,
			0x02, 0x45, 0x30, 0x30, 0x40, 0x20,
		},
		{	/*  Init_SR80_SR93 */
			0xFF, 0x07, 0x00, 0x6F, 0x7F, 0x7F, 0xFF, 0x36,
			0xF7, 0x00, 0x00, 0x00, 0xEF, 0xFF, 0x36, 0x36,
			0x00, 0x00, 0x00, 0x00,
		},
		{	/*  Init_SRA0_SRAF */
			0x00, 0xFF, 0xBF, 0xFF, 0xFF, 0xED, 0xED, 0xED,
			0x7B, 0xFF, 0xFF, 0xFF, 0xBF, 0xEF, 0xBF, 0xDF,
		},
		{	/*  Init_GR00_GR08 */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
			0xFF,
		},
		{	/*  Init_AR00_AR14 */
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
			0x41, 0x00, 0x0F, 0x00, 0x00,
		},
		{	/*  Init_CR00_CR18 */
			0x7F, 0x63, 0x63, 0x00, 0x68, 0x18, 0x72, 0xF0,
			0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x58, 0x0C, 0x57, 0x64, 0x40, 0x57, 0x00, 0xE3,
			0xFF,
		},
		{	/*  Init_CR30_CR4D */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x33, 0x03, 0x20,
			0x00, 0x00, 0x00, 0x40, 0x00, 0xE7, 0xBF, 0xFD,
			0x7F, 0x63, 0x00, 0x69, 0x18, 0x72, 0x57, 0x00,
			0x58, 0x0C, 0xE0, 0x20, 0x63, 0x57,
		},
		{	/*  Init_CR90_CRA7 */
			0x56, 0x4B, 0x5E, 0x55, 0x86, 0x9D, 0x8E, 0xAA,
			0xDB, 0x2A, 0xDF, 0x33, 0x00, 0x00, 0x18, 0x00,
			0x20, 0x1F, 0x1A, 0x19, 0x0F, 0x0F, 0x0F, 0x00,
		},
	},
	{	/*  mode#7: 800 x 600  32Bpp  60Hz */
		800, 600, 32, 60,
		/*  Init_MISC */
		0x2B,
		{	/*  Init_SR0_SR4 */
			0x03, 0x01, 0x0F, 0x03, 0x0E,
		},
		{	/*  Init_SR10_SR24 */
			0xFF, 0xBE, 0xEE, 0xFF, 0x00, 0x0E, 0x17, 0x2C,
			0x99, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xC4, 0x30, 0x02, 0x01, 0x01,
		},
		{	/*  Init_SR30_SR75 */
			0x34, 0x03, 0x20, 0x09, 0xC0, 0x24, 0x24, 0x24,
			0x24, 0x24, 0x24, 0x24, 0x00, 0x00, 0x03, 0xFF,
			0x00, 0xFC, 0x00, 0x00, 0x20, 0x38, 0x00, 0xFC,
			0x20, 0x0C, 0x44, 0x20, 0x00, 0x24, 0x24, 0x24,
			0x04, 0x48, 0x83, 0x63, 0x68, 0x72, 0x57, 0x58,
			0x04, 0x55, 0x59, 0x24, 0x24, 0x00, 0x00, 0x24,
			0x01, 0x80, 0x7A, 0x1A, 0x1A, 0x00, 0x00, 0x00,
			0x50, 0x03, 0x74, 0x14, 0x1C, 0x85, 0x35, 0x13,
			0x02, 0x45, 0x30, 0x35, 0x40, 0x20,
		},
		{	/*  Init_SR80_SR93 */
			0x00, 0x00, 0x00, 0x6F, 0x7F, 0x7F, 0xFF, 0x24,
			0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x24, 0x24,
			0x00, 0x00, 0x00, 0x00,
		},
		{	/*  Init_SRA0_SRAF */
			0x00, 0xFF, 0xBF, 0xFF, 0xFF, 0xED, 0xED, 0xED,
			0x7B, 0xFF, 0xFF, 0xFF, 0xBF, 0xEF, 0xBF, 0xDF,
		},
		{	/*  Init_GR00_GR08 */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
			0xFF,
		},
		{	/*  Init_AR00_AR14 */
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
			0x41, 0x00, 0x0F, 0x00, 0x00,
		},
		{	/*  Init_CR00_CR18 */
			0x7F, 0x63, 0x63, 0x00, 0x68, 0x18, 0x72, 0xF0,
			0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x58, 0x0C, 0x57, 0x64, 0x40, 0x57, 0x00, 0xE3,
			0xFF,
		},
		{	/*  Init_CR30_CR4D */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x33, 0x03, 0x20,
			0x00, 0x00, 0x00, 0x40, 0x00, 0xE7, 0xBF, 0xFD,
			0x7F, 0x63, 0x00, 0x69, 0x18, 0x72, 0x57, 0x00,
			0x58, 0x0C, 0xE0, 0x20, 0x63, 0x57,
		},
		{	/*  Init_CR90_CRA7 */
			0x56, 0x4B, 0x5E, 0x55, 0x86, 0x9D, 0x8E, 0xAA,
			0xDB, 0x2A, 0xDF, 0x33, 0x00, 0x00, 0x18, 0x00,
			0x20, 0x1F, 0x1A, 0x19, 0x0F, 0x0F, 0x0F, 0x00,
		},
	},
	/* We use 1024x768 table to light 1024x600 panel for lemote */
	{	/*  mode#4: 1024 x 600  16Bpp  60Hz  */
		1024, 600, 16, 60,
		/*  Init_MISC */
		0xEB,
		{	/*  Init_SR0_SR4 */
			0x03, 0x01, 0x0F, 0x00, 0x0E,
		},
		{	/*  Init_SR10_SR24 */
			0xC8, 0x40, 0x14, 0x60, 0x00, 0x0A, 0x17, 0x20,
			0x51, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xC4, 0x30, 0x02, 0x00, 0x01,
		},
		{	/*  Init_SR30_SR75 */
			0x22, 0x03, 0x24, 0x09, 0xC0, 0x22, 0x22, 0x22,
			0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x03, 0xFF,
			0x00, 0xFC, 0x00, 0x00, 0x20, 0x18, 0x00, 0xFC,
			0x20, 0x0C, 0x44, 0x20, 0x00, 0x22, 0x22, 0x22,
			0x06, 0x68, 0xA7, 0x7F, 0x83, 0x24, 0xFF, 0x03,
			0x00, 0x60, 0x59, 0x22, 0x22, 0x00, 0x00, 0x22,
			0x01, 0x80, 0x7A, 0x1A, 0x1A, 0x00, 0x00, 0x00,
			0x50, 0x03, 0x16, 0x02, 0x0D, 0x82, 0x09, 0x02,
			0x04, 0x45, 0x3F, 0x30, 0x40, 0x20,
		},
		{	/*  Init_SR80_SR93 */
			0xFF, 0x07, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x3A,
			0xF7, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x3A, 0x3A,
			0x00, 0x00, 0x00, 0x00,
		},
		{	/*  Init_SRA0_SRAF */
			0x00, 0xFB, 0x9F, 0x01, 0x00, 0xED, 0xED, 0xED,
			0x7B, 0xFB, 0xFF, 0xFF, 0x97, 0xEF, 0xBF, 0xDF,
		},
		{	/*  Init_GR00_GR08 */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
			0xFF,
		},
		{	/*  Init_AR00_AR14 */
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
			0x41, 0x00, 0x0F, 0x00, 0x00,
		},
		{	/*  Init_CR00_CR18 */
			0xA3, 0x7F, 0x7F, 0x00, 0x85, 0x16, 0x24, 0xF5,
			0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x03, 0x09, 0xFF, 0x80, 0x40, 0xFF, 0x00, 0xE3,
			0xFF,
		},
		{	/*  Init_CR30_CR4D */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x02, 0x20,
			0x00, 0x00, 0x00, 0x40, 0x00, 0xFF, 0xBF, 0xFF,
			0xA3, 0x7F, 0x00, 0x82, 0x0b, 0x6f, 0x57, 0x00,
			0x5c, 0x0f, 0xE0, 0xe0, 0x7F, 0x57,
		},
		{	/*  Init_CR90_CRA7 */
			0x55, 0xD9, 0x5D, 0xE1, 0x86, 0x1B, 0x8E, 0x26,
			0xDA, 0x8D, 0xDE, 0x94, 0x00, 0x00, 0x18, 0x00,
			0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x15, 0x03,
		},
	},
	{	/*  mode#5: 1024 x 768  24Bpp  60Hz */
		1024, 768, 24, 60,
		/*  Init_MISC */
		0xEB,
		{	/*  Init_SR0_SR4 */
			0x03, 0x01, 0x0F, 0x03, 0x0E,
		},
		{	/*  Init_SR10_SR24 */
			0xF3, 0xB6, 0xC0, 0xDD, 0x00, 0x0E, 0x17, 0x2C,
			0x99, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xC4, 0x30, 0x02, 0x01, 0x01,
		},
		{	/*  Init_SR30_SR75 */
			0x38, 0x03, 0x20, 0x09, 0xC0, 0x3A, 0x3A, 0x3A,
			0x3A, 0x3A, 0x3A, 0x3A, 0x00, 0x00, 0x03, 0xFF,
			0x00, 0xFC, 0x00, 0x00, 0x20, 0x18, 0x00, 0xFC,
			0x20, 0x0C, 0x44, 0x20, 0x00, 0x00, 0x00, 0x3A,
			0x06, 0x68, 0xA7, 0x7F, 0x83, 0x24, 0xFF, 0x03,
			0x00, 0x60, 0x59, 0x3A, 0x3A, 0x00, 0x00, 0x3A,
			0x01, 0x80, 0x7E, 0x1A, 0x1A, 0x00, 0x00, 0x00,
			0x50, 0x03, 0x74, 0x14, 0x3B, 0x0D, 0x09, 0x02,
			0x04, 0x45, 0x30, 0x30, 0x40, 0x20,
		},
		{	/*  Init_SR80_SR93 */
			0xFF, 0x07, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x3A,
			0xF7, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x3A, 0x3A,
			0x00, 0x00, 0x00, 0x00,
		},
		{	/*  Init_SRA0_SRAF */
			0x00, 0xFB, 0x9F, 0x01, 0x00, 0xED, 0xED, 0xED,
			0x7B, 0xFB, 0xFF, 0xFF, 0x97, 0xEF, 0xBF, 0xDF,
		},
		{	/*  Init_GR00_GR08 */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
			0xFF,
		},
		{	/*  Init_AR00_AR14 */
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
			0x41, 0x00, 0x0F, 0x00, 0x00,
		},
		{	/*  Init_CR00_CR18 */
			0xA3, 0x7F, 0x7F, 0x00, 0x85, 0x16, 0x24, 0xF5,
			0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x03, 0x09, 0xFF, 0x80, 0x40, 0xFF, 0x00, 0xE3,
			0xFF,
		},
		{	/*  Init_CR30_CR4D */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x02, 0x20,
			0x00, 0x00, 0x00, 0x40, 0x00, 0xFF, 0xBF, 0xFF,
			0xA3, 0x7F, 0x00, 0x86, 0x15, 0x24, 0xFF, 0x00,
			0x01, 0x07, 0xE5, 0x20, 0x7F, 0xFF,
		},
		{	/*  Init_CR90_CRA7 */
			0x55, 0xD9, 0x5D, 0xE1, 0x86, 0x1B, 0x8E, 0x26,
			0xDA, 0x8D, 0xDE, 0x94, 0x00, 0x00, 0x18, 0x00,
			0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x15, 0x03,
		},
	},
	{	/*  mode#4: 1024 x 768  32Bpp  60Hz */
		1024, 768, 32, 60,
		/*  Init_MISC */
		0xEB,
		{	/*  Init_SR0_SR4 */
			0x03, 0x01, 0x0F, 0x03, 0x0E,
		},
		{	/*  Init_SR10_SR24 */
			0xF3, 0xB6, 0xC0, 0xDD, 0x00, 0x0E, 0x17, 0x2C,
			0x99, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xC4, 0x32, 0x02, 0x01, 0x01,
		},
		{	/*  Init_SR30_SR75 */
			0x38, 0x03, 0x20, 0x09, 0xC0, 0x3A, 0x3A, 0x3A,
			0x3A, 0x3A, 0x3A, 0x3A, 0x00, 0x00, 0x03, 0xFF,
			0x00, 0xFC, 0x00, 0x00, 0x20, 0x18, 0x00, 0xFC,
			0x20, 0x0C, 0x44, 0x20, 0x00, 0x00, 0x00, 0x3A,
			0x06, 0x68, 0xA7, 0x7F, 0x83, 0x24, 0xFF, 0x03,
			0x00, 0x60, 0x59, 0x3A, 0x3A, 0x00, 0x00, 0x3A,
			0x01, 0x80, 0x7E, 0x1A, 0x1A, 0x00, 0x00, 0x00,
			0x50, 0x03, 0x74, 0x14, 0x3B, 0x0D, 0x09, 0x02,
			0x04, 0x45, 0x30, 0x30, 0x40, 0x20,
		},
		{	/*  Init_SR80_SR93 */
			0xFF, 0x07, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x3A,
			0xF7, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x3A, 0x3A,
			0x00, 0x00, 0x00, 0x00,
		},
		{	/*  Init_SRA0_SRAF */
			0x00, 0xFB, 0x9F, 0x01, 0x00, 0xED, 0xED, 0xED,
			0x7B, 0xFB, 0xFF, 0xFF, 0x97, 0xEF, 0xBF, 0xDF,
		},
		{	/*  Init_GR00_GR08 */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
			0xFF,
		},
		{	/*  Init_AR00_AR14 */
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
			0x41, 0x00, 0x0F, 0x00, 0x00,
		},
		{	/*  Init_CR00_CR18 */
			0xA3, 0x7F, 0x7F, 0x00, 0x85, 0x16, 0x24, 0xF5,
			0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x03, 0x09, 0xFF, 0x80, 0x40, 0xFF, 0x00, 0xE3,
			0xFF,
		},
		{	/*  Init_CR30_CR4D */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x02, 0x20,
			0x00, 0x00, 0x00, 0x40, 0x00, 0xFF, 0xBF, 0xFF,
			0xA3, 0x7F, 0x00, 0x86, 0x15, 0x24, 0xFF, 0x00,
			0x01, 0x07, 0xE5, 0x20, 0x7F, 0xFF,
		},
		{	/*  Init_CR90_CRA7 */
			0x55, 0xD9, 0x5D, 0xE1, 0x86, 0x1B, 0x8E, 0x26,
			0xDA, 0x8D, 0xDE, 0x94, 0x00, 0x00, 0x18, 0x00,
			0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x15, 0x03,
		},
	},
};


/* prototypes of two cross-referenced functions */
static void smtcfb_reset_accel(void);
static int smtcfb_init_accel(struct smtcfb_info *fb);

static struct screen_info smtc_scr_info;
static char *mode_option;
static bool accel = true;  /* can be ignored if not supported */
static bool accel_status_reported;

/* process command line options, get vga and accel parameter */
static void __init sm7xx_vga_setup(char *options)
{
	int i;
	char *this_opt;

	if (!options || !*options)
		return;

	smtc_scr_info.lfb_width = 0;
	smtc_scr_info.lfb_height = 0;
	smtc_scr_info.lfb_depth = 0;

	pr_debug("%s = %s\n", __func__, options);

	for (i = 0; i < ARRAY_SIZE(vesa_mode_table); i++) {
		if (strstr(options, vesa_mode_table[i].index)) {
			smtc_scr_info.lfb_width  = vesa_mode_table[i].lfb_width;
			smtc_scr_info.lfb_height =
						vesa_mode_table[i].lfb_height;
			smtc_scr_info.lfb_depth  = vesa_mode_table[i].lfb_depth;
			break;
		}
	}

	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!*this_opt)
			continue;

		if (!strcmp(this_opt, "accel:0"))
			accel = false;
		else if (!strcmp(this_opt, "accel:1"))
			accel = true;
	}
	accel_status_reported = false;
}

static void sm712_setpalette(int regno, unsigned int red, unsigned int green,
			     unsigned int blue, struct fb_info *info)
{
	/* set bit 5:4 = 01 (write LCD RAM only) */
	smtc_seqw(0x66, (smtc_seqr(0x66) & 0xC3) | 0x10);

	smtc_mmiowb(regno, dac_reg);
	smtc_mmiowb(red >> 10, dac_val);
	smtc_mmiowb(green >> 10, dac_val);
	smtc_mmiowb(blue >> 10, dac_val);
}

/* chan_to_field
 *
 * convert a colour value into a field position
 *
 * from pxafb.c
 */

static inline unsigned int chan_to_field(unsigned int chan,
					 struct fb_bitfield *bf)
{
	chan &= 0xffff;
	chan >>= 16 - bf->length;
	return chan << bf->offset;
}

static int smtc_blank(int blank_mode, struct fb_info *info)
{
	/* clear DPMS setting */
	switch (blank_mode) {
	case FB_BLANK_UNBLANK:
		/* Screen On: HSync: On, VSync : On */
		smtc_seqw(0x01, (smtc_seqr(0x01) & (~0x20)));
		smtc_seqw(0x6a, 0x16);
		smtc_seqw(0x6b, 0x02);
		smtc_seqw(0x21, (smtc_seqr(0x21) & 0x77));
		smtc_seqw(0x22, (smtc_seqr(0x22) & (~0x30)));
		smtc_seqw(0x23, (smtc_seqr(0x23) & (~0xc0)));
		smtc_seqw(0x24, (smtc_seqr(0x24) | 0x01));
		smtc_seqw(0x31, (smtc_seqr(0x31) | 0x03));
		break;
	case FB_BLANK_NORMAL:
		/* Screen Off: HSync: On, VSync : On   Soft blank */
		smtc_seqw(0x01, (smtc_seqr(0x01) & (~0x20)));
		smtc_seqw(0x6a, 0x16);
		smtc_seqw(0x6b, 0x02);
		smtc_seqw(0x22, (smtc_seqr(0x22) & (~0x30)));
		smtc_seqw(0x23, (smtc_seqr(0x23) & (~0xc0)));
		smtc_seqw(0x24, (smtc_seqr(0x24) | 0x01));
		smtc_seqw(0x31, ((smtc_seqr(0x31) & (~0x07)) | 0x00));
		break;
	case FB_BLANK_VSYNC_SUSPEND:
		/* Screen On: HSync: On, VSync : Off */
		smtc_seqw(0x01, (smtc_seqr(0x01) | 0x20));
		smtc_seqw(0x20, (smtc_seqr(0x20) & (~0xB0)));
		smtc_seqw(0x6a, 0x0c);
		smtc_seqw(0x6b, 0x02);
		smtc_seqw(0x21, (smtc_seqr(0x21) | 0x88));
		smtc_seqw(0x22, ((smtc_seqr(0x22) & (~0x30)) | 0x20));
		smtc_seqw(0x23, ((smtc_seqr(0x23) & (~0xc0)) | 0x20));
		smtc_seqw(0x24, (smtc_seqr(0x24) & (~0x01)));
		smtc_seqw(0x31, ((smtc_seqr(0x31) & (~0x07)) | 0x00));
		smtc_seqw(0x34, (smtc_seqr(0x34) | 0x80));
		break;
	case FB_BLANK_HSYNC_SUSPEND:
		/* Screen On: HSync: Off, VSync : On */
		smtc_seqw(0x01, (smtc_seqr(0x01) | 0x20));
		smtc_seqw(0x20, (smtc_seqr(0x20) & (~0xB0)));
		smtc_seqw(0x6a, 0x0c);
		smtc_seqw(0x6b, 0x02);
		smtc_seqw(0x21, (smtc_seqr(0x21) | 0x88));
		smtc_seqw(0x22, ((smtc_seqr(0x22) & (~0x30)) | 0x10));
		smtc_seqw(0x23, ((smtc_seqr(0x23) & (~0xc0)) | 0xD8));
		smtc_seqw(0x24, (smtc_seqr(0x24) & (~0x01)));
		smtc_seqw(0x31, ((smtc_seqr(0x31) & (~0x07)) | 0x00));
		smtc_seqw(0x34, (smtc_seqr(0x34) | 0x80));
		break;
	case FB_BLANK_POWERDOWN:
		/* Screen On: HSync: Off, VSync : Off */
		smtc_seqw(0x01, (smtc_seqr(0x01) | 0x20));
		smtc_seqw(0x20, (smtc_seqr(0x20) & (~0xB0)));
		smtc_seqw(0x6a, 0x0c);
		smtc_seqw(0x6b, 0x02);
		smtc_seqw(0x21, (smtc_seqr(0x21) | 0x88));
		smtc_seqw(0x22, ((smtc_seqr(0x22) & (~0x30)) | 0x30));
		smtc_seqw(0x23, ((smtc_seqr(0x23) & (~0xc0)) | 0xD8));
		smtc_seqw(0x24, (smtc_seqr(0x24) & (~0x01)));
		smtc_seqw(0x31, ((smtc_seqr(0x31) & (~0x07)) | 0x00));
		smtc_seqw(0x34, (smtc_seqr(0x34) | 0x80));
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int smtc_setcolreg(unsigned int regno, unsigned int red,
			  unsigned int green, unsigned int blue,
			  unsigned int trans, struct fb_info *info)
{
	struct smtcfb_info *sfb;
	u32 val;

	sfb = info->par;

	if (regno > 255)
		return 1;

	switch (sfb->fb->fix.visual) {
	case FB_VISUAL_DIRECTCOLOR:
	case FB_VISUAL_TRUECOLOR:
		/*
		 * 16/32 bit true-colour, use pseudo-palette for 16 base color
		 */
		if (regno >= 16)
			break;
		if (sfb->fb->var.bits_per_pixel == 16) {
			u32 *pal = sfb->fb->pseudo_palette;

			val = chan_to_field(red, &sfb->fb->var.red);
			val |= chan_to_field(green, &sfb->fb->var.green);
			val |= chan_to_field(blue, &sfb->fb->var.blue);
			pal[regno] = pal_rgb(red, green, blue, val);
		} else {
			u32 *pal = sfb->fb->pseudo_palette;

			val = chan_to_field(red, &sfb->fb->var.red);
			val |= chan_to_field(green, &sfb->fb->var.green);
			val |= chan_to_field(blue, &sfb->fb->var.blue);
			pal[regno] = big_swap(val);
		}
		break;

	case FB_VISUAL_PSEUDOCOLOR:
		/* color depth 8 bit */
		sm712_setpalette(regno, red, green, blue, info);
		break;

	default:
		return 1;	/* unknown type */
	}

	return 0;
}

static ssize_t smtcfb_read(struct fb_info *info, char __user *buf,
			   size_t count, loff_t *ppos)
{
	unsigned long p = *ppos;

	u32 *buffer, *dst;
	u32 __iomem *src;
	int c, i, cnt = 0, err = 0;
	unsigned long total_size;

	if (!info || !info->screen_base)
		return -ENODEV;

	if (info->state != FBINFO_STATE_RUNNING)
		return -EPERM;

	total_size = info->screen_size;

	if (total_size == 0)
		total_size = info->fix.smem_len;

	if (p >= total_size)
		return 0;

	if (count >= total_size)
		count = total_size;

	if (count + p > total_size)
		count = total_size - p;

	buffer = kmalloc((count > PAGE_SIZE) ? PAGE_SIZE : count, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	src = (u32 __iomem *)(info->screen_base + p);

	if (info->fbops->fb_sync)
		info->fbops->fb_sync(info);

	while (count) {
		c = (count > PAGE_SIZE) ? PAGE_SIZE : count;
		dst = buffer;
		for (i = c >> 2; i--;) {
			*dst = fb_readl(src++);
			*dst = big_swap(*dst);
			dst++;
		}
		if (c & 3) {
			u8 *dst8 = (u8 *)dst;
			u8 __iomem *src8 = (u8 __iomem *)src;

			for (i = c & 3; i--;) {
				if (i & 1) {
					*dst8++ = fb_readb(++src8);
				} else {
					*dst8++ = fb_readb(--src8);
					src8 += 2;
				}
			}
			src = (u32 __iomem *)src8;
		}

		if (copy_to_user(buf, buffer, c)) {
			err = -EFAULT;
			break;
		}
		*ppos += c;
		buf += c;
		cnt += c;
		count -= c;
	}

	kfree(buffer);

	return (err) ? err : cnt;
}

static ssize_t smtcfb_write(struct fb_info *info, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	unsigned long p = *ppos;

	u32 *buffer, *src;
	u32 __iomem *dst;
	int c, i, cnt = 0, err = 0;
	unsigned long total_size;

	if (!info || !info->screen_base)
		return -ENODEV;

	if (info->state != FBINFO_STATE_RUNNING)
		return -EPERM;

	total_size = info->screen_size;

	if (total_size == 0)
		total_size = info->fix.smem_len;

	if (p > total_size)
		return -EFBIG;

	if (count > total_size) {
		err = -EFBIG;
		count = total_size;
	}

	if (count + p > total_size) {
		if (!err)
			err = -ENOSPC;

		count = total_size - p;
	}

	buffer = kmalloc((count > PAGE_SIZE) ? PAGE_SIZE : count, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	dst = (u32 __iomem *)(info->screen_base + p);

	if (info->fbops->fb_sync)
		info->fbops->fb_sync(info);

	while (count) {
		c = (count > PAGE_SIZE) ? PAGE_SIZE : count;
		src = buffer;

		if (copy_from_user(src, buf, c)) {
			err = -EFAULT;
			break;
		}

		for (i = c >> 2; i--;) {
			fb_writel(big_swap(*src), dst++);
			src++;
		}
		if (c & 3) {
			u8 *src8 = (u8 *)src;
			u8 __iomem *dst8 = (u8 __iomem *)dst;

			for (i = c & 3; i--;) {
				if (i & 1) {
					fb_writeb(*src8++, ++dst8);
				} else {
					fb_writeb(*src8++, --dst8);
					dst8 += 2;
				}
			}
			dst = (u32 __iomem *)dst8;
		}

		*ppos += c;
		buf += c;
		cnt += c;
		count -= c;
	}

	kfree(buffer);

	return (cnt) ? cnt : err;
}

static void sm7xx_set_timing(struct smtcfb_info *sfb)
{
	int i = 0, j = 0;
	u32 m_nscreenstride;

	dev_dbg(&sfb->pdev->dev,
		"sfb->width=%d sfb->height=%d sfb->fb->var.bits_per_pixel=%d sfb->hz=%d\n",
		sfb->width, sfb->height, sfb->fb->var.bits_per_pixel, sfb->hz);

	for (j = 0; j < ARRAY_SIZE(vgamode); j++) {
		if (vgamode[j].mmsizex != sfb->width ||
		    vgamode[j].mmsizey != sfb->height ||
		    vgamode[j].bpp != sfb->fb->var.bits_per_pixel ||
		    vgamode[j].hz != sfb->hz)
			continue;

		dev_dbg(&sfb->pdev->dev,
			"vgamode[j].mmsizex=%d vgamode[j].mmSizeY=%d vgamode[j].bpp=%d vgamode[j].hz=%d\n",
			vgamode[j].mmsizex, vgamode[j].mmsizey,
			vgamode[j].bpp, vgamode[j].hz);

		dev_dbg(&sfb->pdev->dev, "vgamode index=%d\n", j);

		smtc_mmiowb(0x0, 0x3c6);

		smtc_seqw(0, 0x1);

		smtc_mmiowb(vgamode[j].init_misc, 0x3c2);

		/* init SEQ register SR00 - SR04 */
		for (i = 0; i < SIZE_SR00_SR04; i++)
			smtc_seqw(i, vgamode[j].init_sr00_sr04[i]);

		/* init SEQ register SR10 - SR24 */
		for (i = 0; i < SIZE_SR10_SR24; i++)
			smtc_seqw(i + 0x10, vgamode[j].init_sr10_sr24[i]);

		/* init SEQ register SR30 - SR75 */
		for (i = 0; i < SIZE_SR30_SR75; i++)
			if ((i + 0x30) != 0x62 && (i + 0x30) != 0x6a &&
			    (i + 0x30) != 0x6b)
				smtc_seqw(i + 0x30,
					  vgamode[j].init_sr30_sr75[i]);

		/* init SEQ register SR80 - SR93 */
		for (i = 0; i < SIZE_SR80_SR93; i++)
			smtc_seqw(i + 0x80, vgamode[j].init_sr80_sr93[i]);

		/* init SEQ register SRA0 - SRAF */
		for (i = 0; i < SIZE_SRA0_SRAF; i++)
			smtc_seqw(i + 0xa0, vgamode[j].init_sra0_sraf[i]);

		/* init Graphic register GR00 - GR08 */
		for (i = 0; i < SIZE_GR00_GR08; i++)
			smtc_grphw(i, vgamode[j].init_gr00_gr08[i]);

		/* init Attribute register AR00 - AR14 */
		for (i = 0; i < SIZE_AR00_AR14; i++)
			smtc_attrw(i, vgamode[j].init_ar00_ar14[i]);

		/* init CRTC register CR00 - CR18 */
		for (i = 0; i < SIZE_CR00_CR18; i++)
			smtc_crtcw(i, vgamode[j].init_cr00_cr18[i]);

		/* init CRTC register CR30 - CR4D */
		for (i = 0; i < SIZE_CR30_CR4D; i++)
			smtc_crtcw(i + 0x30, vgamode[j].init_cr30_cr4d[i]);

		/* init CRTC register CR90 - CRA7 */
		for (i = 0; i < SIZE_CR90_CRA7; i++)
			smtc_crtcw(i + 0x90, vgamode[j].init_cr90_cra7[i]);
	}
	smtc_mmiowb(0x67, 0x3c2);

	/* set VPR registers */
	writel(0x0, sfb->vp_regs + 0x0C);
	writel(0x0, sfb->vp_regs + 0x40);

	/* set data width */
	m_nscreenstride = (sfb->width * sfb->fb->var.bits_per_pixel) / 64;
	switch (sfb->fb->var.bits_per_pixel) {
	case 8:
		writel(0x0, sfb->vp_regs + 0x0);
		break;
	case 16:
		writel(0x00020000, sfb->vp_regs + 0x0);
		break;
	case 24:
		writel(0x00040000, sfb->vp_regs + 0x0);
		break;
	case 32:
		writel(0x00030000, sfb->vp_regs + 0x0);
		break;
	}
	writel((u32)(((m_nscreenstride + 2) << 16) | m_nscreenstride),
	       sfb->vp_regs + 0x10);
}

static void smtc_set_timing(struct smtcfb_info *sfb)
{
	switch (sfb->chip_id) {
	case 0x710:
	case 0x712:
	case 0x720:
		sm7xx_set_timing(sfb);
		break;
	}
}

static void smtcfb_setmode(struct smtcfb_info *sfb)
{
	switch (sfb->fb->var.bits_per_pixel) {
	case 32:
		sfb->fb->fix.visual       = FB_VISUAL_TRUECOLOR;
		sfb->fb->fix.line_length  = sfb->fb->var.xres * 4;
		sfb->fb->var.red.length   = 8;
		sfb->fb->var.green.length = 8;
		sfb->fb->var.blue.length  = 8;
		sfb->fb->var.red.offset   = 16;
		sfb->fb->var.green.offset = 8;
		sfb->fb->var.blue.offset  = 0;
		break;
	case 24:
		sfb->fb->fix.visual       = FB_VISUAL_TRUECOLOR;
		sfb->fb->fix.line_length  = sfb->fb->var.xres * 3;
		sfb->fb->var.red.length   = 8;
		sfb->fb->var.green.length = 8;
		sfb->fb->var.blue.length  = 8;
		sfb->fb->var.red.offset   = 16;
		sfb->fb->var.green.offset = 8;
		sfb->fb->var.blue.offset  = 0;
		break;
	case 8:
		sfb->fb->fix.visual       = FB_VISUAL_PSEUDOCOLOR;
		sfb->fb->fix.line_length  = sfb->fb->var.xres;
		sfb->fb->var.red.length   = 3;
		sfb->fb->var.green.length = 3;
		sfb->fb->var.blue.length  = 2;
		sfb->fb->var.red.offset   = 5;
		sfb->fb->var.green.offset = 2;
		sfb->fb->var.blue.offset  = 0;
		break;
	case 16:
	default:
		sfb->fb->fix.visual       = FB_VISUAL_TRUECOLOR;
		sfb->fb->fix.line_length  = sfb->fb->var.xres * 2;
		sfb->fb->var.red.length   = 5;
		sfb->fb->var.green.length = 6;
		sfb->fb->var.blue.length  = 5;
		sfb->fb->var.red.offset   = 11;
		sfb->fb->var.green.offset = 5;
		sfb->fb->var.blue.offset  = 0;
		break;
	}

	sfb->width  = sfb->fb->var.xres;
	sfb->height = sfb->fb->var.yres;
	sfb->hz = 60;

	/*
	 * We reset the 2D engine twice, once before the modesetting, once
	 * after the modesetting (mandatory), since users may chance the
	 * mode on-the-fly. Just be safe.
	 */
	smtcfb_reset_accel();

	smtc_set_timing(sfb);

	/*
	 * Currently, 2D acceleration is only supported on SM712 with
	 * little-endian CPUs, it's disabled on Big Endian systems and SM720
	 * chips as a safety measure. Since I don't have monetary or hardware
	 * support from any company or OEMs, I don't have the hardware and
	 * it's completely untested. I should be also to purchase a Big Endian
	 * test platform and add proper support soon. I still have to spend
	 * 200 USD+ to purchase this piece of 1998's hardware, yikes! If you
	 * have a Big-Endian platform with SM7xx available for testing, please
	 * send an E-mail to Tom, thanks!
	 */
#ifdef __BIG_ENDIAN
	sfb->accel = false;
	if (accel)
		dev_info(&sfb->pdev->dev,
			"2D acceleration is unsupported on Big Endian.\n");
#endif
	if (!accel) {
		sfb->accel = false;
		dev_info(&sfb->pdev->dev,
			"2D acceleration is disabled by the user.\n");
	}

	/* reset 2D engine after a modesetting is mandatory */
	smtcfb_reset_accel();
	smtcfb_init_accel(sfb);
}

static int smtc_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	/* sanity checks */
	if (var->xres_virtual < var->xres)
		var->xres_virtual = var->xres;

	if (var->yres_virtual < var->yres)
		var->yres_virtual = var->yres;

	/* set valid default bpp */
	if ((var->bits_per_pixel != 8)  && (var->bits_per_pixel != 16) &&
	    (var->bits_per_pixel != 24) && (var->bits_per_pixel != 32))
		var->bits_per_pixel = 16;

	return 0;
}

static int smtc_set_par(struct fb_info *info)
{
	smtcfb_setmode(info->par);

	return 0;
}

static struct fb_ops smtcfb_ops = {
	.owner        = THIS_MODULE,
	.fb_check_var = smtc_check_var,
	.fb_set_par   = smtc_set_par,
	.fb_setcolreg = smtc_setcolreg,
	.fb_blank     = smtc_blank,
	.fb_fillrect  = cfb_fillrect,
	.fb_imageblit = cfb_imageblit,
	.fb_copyarea  = cfb_copyarea,
	.fb_read      = smtcfb_read,
	.fb_write     = smtcfb_write,
};

static int smtcfb_wait(struct smtcfb_info *fb)
{
	int i;
	u8 reg;

	smtc_dprr(DPR_DE_CTRL);
	for (i = 0; i < 10000; i++) {
		reg = smtc_seqr(SCR_DE_STATUS);
		if ((reg & SCR_DE_STATUS_MASK) == SCR_DE_ENGINE_IDLE)
			return 0;
		udelay(1);
	}
	dev_err(&fb->pdev->dev, "2D engine hang detected!\n");
	return -EBUSY;
}

static void
smtcfb_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	u32 width = rect->width, height = rect->height;
	u32 dx = rect->dx, dy = rect->dy;
	u32 color;

	struct smtcfb_info *sfb = info->par;

	if (unlikely(info->state != FBINFO_STATE_RUNNING))
		return;

	if (unlikely(rect->rop != ROP_COPY)) {
		/*
		 * It must be ROP_XOR. It's only used to combine a hardware
		 * cursor with the screen, and should never occur. Included
		 * for completeness. If one wants to implement hardware cursor
		 * (you don't, hardware only has RGB332 cursor), ROP2_XOR
		 * should be implemented here.
		 */
		cfb_fillrect(info, rect);
		return;
	}

	if ((rect->dx >= info->var.xres_virtual) ||
	    (rect->dy >= info->var.yres_virtual))
		return;

	if (info->fix.visual == FB_VISUAL_TRUECOLOR ||
	    info->fix.visual == FB_VISUAL_DIRECTCOLOR)
		color = ((u32 *) (info->pseudo_palette))[rect->color];
	else
		color = rect->color;

	if (sfb->fb->var.bits_per_pixel == 24) {
		/*
		 * In 24-bit mode, all x, y coordinates and widths (but not
		 * height) must be multipiled by three.
		 */
		dx *= 3;
		dy *= 3;
		width *= 3;

		/*
		 * In 24-bit color mode, SOLIDFILL will sometimes put random
		 * color stripes of garbage on the screen, it seems to be a
		 * hardware bug. Alternatively, we initialize MONO_PATTERN_LOW
		 * & HIGH with 0xffffffff (all ones, and we have already set
		 * that in smtcfb_init_accel). Since the color of this mono
		 * pattern is controlled by DPR_FG_COLOR, BITBLTing it with
		 * ROP_COPY is effectively a rectfill().
		 */
		smtc_dprw(DPR_FG_COLOR, color);
		smtc_dprw(DPR_DST_COORDS, DPR_COORDS(dx, dy));
		smtc_dprw(DPR_SPAN_COORDS, DPR_COORDS(width, height));
		smtc_dprw(DPR_DE_CTRL, DE_CTRL_START | DE_CTRL_ROP2_SELECT |
				DE_CTRL_ROP2_SRC_IS_PATTERN |
				(DE_CTRL_COMMAND_BITBLT <<
						DE_CTRL_COMMAND_SHIFT) |
				(DE_CTRL_ROP2_COPY <<
						DE_CTRL_ROP2_SHIFT));
	} else {
		smtc_dprw(DPR_FG_COLOR, color);
		smtc_dprw(DPR_DST_COORDS, DPR_COORDS(dx, dy));
		smtc_dprw(DPR_SPAN_COORDS, DPR_COORDS(width, height));
		smtc_dprw(DPR_DE_CTRL, DE_CTRL_START | DE_CTRL_ROP2_SELECT |
				(DE_CTRL_COMMAND_SOLIDFILL <<
						DE_CTRL_COMMAND_SHIFT) |
				(DE_CTRL_ROP2_COPY <<
						DE_CTRL_ROP2_SHIFT));
	}
	smtcfb_wait(sfb);
}

static void
smtcfb_copyarea(struct fb_info *info, const struct fb_copyarea *area)
{
	u32 sx = area->sx, sy = area->sy;
	u32 dx = area->dx, dy = area->dy;
	u32 height = area->height, width = area->width;
	u32 direction;

	struct smtcfb_info *sfb = info->par;

	if (unlikely(info->state != FBINFO_STATE_RUNNING))
		return;
	if ((sx >= info->var.xres_virtual) || (sy >= info->var.yres_virtual))
		return;

	if (sy < dy || (sy == dy && sx <= dx)) {
		sx += width - 1;
		dx += width - 1;
		sy += height - 1;
		dy += height - 1;
		direction = DE_CTRL_RTOL;
	} else {
		direction = 0;
	}

	if (sfb->fb->var.bits_per_pixel == 24) {
		sx *= 3;
		sy *= 3;
		dx *= 3;
		dy *= 3;
		width *= 3;
		if (direction == DE_CTRL_RTOL) {
			/*
			 * some hardware shenanigan from the original git
			 * commit, that is never clearly mentioned in the
			 * official datasheet. Not sure whether it even
			 * works correctly.
			 */
			sx += 2;
			dx += 2;
		}
	}

	smtc_dprw(DPR_SRC_COORDS, DPR_COORDS(sx, sy));
	smtc_dprw(DPR_DST_COORDS, DPR_COORDS(dx, dy));
	smtc_dprw(DPR_SPAN_COORDS, DPR_COORDS(width, height));
	smtc_dprw(DPR_DE_CTRL,
			DE_CTRL_START | DE_CTRL_ROP2_SELECT | direction |
			(DE_CTRL_COMMAND_BITBLT << DE_CTRL_COMMAND_SHIFT) |
			(DE_CTRL_ROP2_COPY << DE_CTRL_ROP2_SHIFT));
	smtcfb_wait(sfb);
}

static void
smtcfb_imageblit(struct fb_info *info, const struct fb_image *image)
{
	u32 dx = image->dx, dy = image->dy;
	u32 width = image->width, height = image->height;
	u32 fg_color, bg_color;

	u32 total_bytes, total_dwords, leftovers;
	u32 i;
	u32 idx = 0;
	u32 scanline = image->width >> 3;

	struct smtcfb_info *sfb = info->par;

	if (unlikely(info->state != FBINFO_STATE_RUNNING))
		return;
	if ((image->dx >= info->var.xres_virtual) ||
	    (image->dy >= info->var.yres_virtual))
		return;

	if (unlikely(image->depth != 1)) {
		/* unsupported depth, fallback to draw Tux */
		cfb_imageblit(info, image);
		return;
	}

	if (info->fix.visual == FB_VISUAL_TRUECOLOR ||
	    info->fix.visual == FB_VISUAL_DIRECTCOLOR) {
		fg_color = ((u32 *) (info->pseudo_palette))[image->fg_color];
		bg_color = ((u32 *) (info->pseudo_palette))[image->bg_color];
	} else {
		fg_color = image->fg_color;
		bg_color = image->bg_color;
	}

	/* total bytes we need to write */
	total_bytes = (width + 7) / 8;
	total_dwords = (total_bytes & ~3) / 4;
	leftovers = total_bytes & 3;

	if (sfb->fb->var.bits_per_pixel == 24) {
		dx *= 3;
		dy *= 3;
		width *= 3;
	}
	smtc_dprw(DPR_SRC_COORDS, 0);
	smtc_dprw(DPR_DST_COORDS, DPR_COORDS(dx, dy));
	smtc_dprw(DPR_SPAN_COORDS, DPR_COORDS(width, height));
	smtc_dprw(DPR_FG_COLOR, fg_color);
	smtc_dprw(DPR_BG_COLOR, bg_color);
	smtc_dprw(DPR_DE_CTRL, DE_CTRL_START | DE_CTRL_ROP2_SELECT |
			(DE_CTRL_COMMAND_HOSTWRITE << DE_CTRL_COMMAND_SHIFT) |
			(DE_CTRL_HOST_SRC_IS_MONO << DE_CTRL_HOST_SHIFT) |
			(DE_CTRL_ROP2_COPY << DE_CTRL_ROP2_SHIFT));

	for (i = 0; i < height; i++) {
		iowrite32_rep(sfb->dp_port, &image->data[idx], total_dwords);
		if (leftovers) {
			/*
			 * We can set info->pixmap.scan_align/buf_align = 4
			 * for automatic padding. But it would be sometimes
			 * incompatible with cfb_*(), especially imageblit()
			 * when depth = 1. In case we need to fallback (e.g.
			 * debugging), it would be inconvenient, so we pad it
			 * manually.
			 */
			writel_relaxed(
				pad_to_dword(
					&image->data[idx + total_dwords * 4],
					leftovers),
				sfb->dp_port);
		}
		idx += scanline;
	}
	mb();  /* ensure all writes to dp_port have finished */
	smtcfb_wait(sfb);
}

static void smtcfb_reset_accel(void)
{
	u8 reg;

	/* enable Zoom Video Port, 2D Drawing Engine and Video Processor */
	smtc_seqw(0x21, smtc_seqr(0x21) & 0xf8);

	/* abort pending 2D Drawing Engine operations */
	reg = smtc_seqr(0x15);
	smtc_seqw(0x15, reg | 0x30);
	smtc_seqw(0x15, reg);
}

/*
 * Function smtcfb_reset_accel(); should be called before calling
 * this function
 */
static int smtcfb_init_accel(struct smtcfb_info *fb)
{

	if (accel && !fb->accel) {
		fb->fb->flags |= FBINFO_HWACCEL_NONE;
		return 0;
	} else if (!accel && !fb->accel) {
		fb->fb->flags |= FBINFO_HWACCEL_DISABLED;
		return 0;
	}

	if (smtcfb_wait(fb) != 0) {
		fb->fb->flags |= FBINFO_HWACCEL_NONE;
		dev_err(&fb->pdev->dev,
			"2D acceleration initialization failed!\n");
		fb->accel = false;
		return -1;
	}

	smtc_dprw(DPR_CROP_TOPLEFT_COORDS, DPR_COORDS(0, 0));

	/* same width for DPR_PITCH and DPR_SRC_WINDOW */
	smtc_dprw(DPR_PITCH, DPR_COORDS(fb->fb->var.xres, fb->fb->var.xres));
	smtc_dprw(DPR_SRC_WINDOW,
			DPR_COORDS(fb->fb->var.xres, fb->fb->var.xres));

	switch (fb->fb->var.bits_per_pixel) {
	case 8:
		smtc_dprw_16(DPR_DE_FORMAT_SELECT,
				DE_CTRL_FORMAT_XY | DE_CTRL_FORMAT_8BIT);
		break;
	case 16:
		smtc_dprw_16(DPR_DE_FORMAT_SELECT,
				DE_CTRL_FORMAT_XY | DE_CTRL_FORMAT_16BIT);
		break;
	case 24:
		smtc_dprw_16(DPR_DE_FORMAT_SELECT,
				DE_CTRL_FORMAT_XY | DE_CTRL_FORMAT_24BIT);
		smtc_dprw(DPR_PITCH,
				DPR_COORDS(fb->fb->var.xres * 3,
						fb->fb->var.xres * 3));
		break;
	case 32:
		smtc_dprw_16(DPR_DE_FORMAT_SELECT,
				DE_CTRL_FORMAT_XY | DE_CTRL_FORMAT_32BIT);
		break;
	}

	smtc_dprw(DPR_BYTE_BIT_MASK, 0xffffffff);
	smtc_dprw(DPR_COLOR_COMPARE_MASK, 0);
	smtc_dprw(DPR_COLOR_COMPARE, 0);
	smtc_dprw(DPR_SRC_BASE, 0);
	smtc_dprw(DPR_DST_BASE, 0);
	smtc_dprw(DPR_MONO_PATTERN_LO32, 0xffffffff);
	smtc_dprw(DPR_MONO_PATTERN_HI32, 0xffffffff);
	smtc_dprr(DPR_DST_BASE);

	smtcfb_ops.fb_copyarea = smtcfb_copyarea;
	smtcfb_ops.fb_fillrect = smtcfb_fillrect;
	smtcfb_ops.fb_imageblit = smtcfb_imageblit;
	fb->fb->flags |= FBINFO_HWACCEL_COPYAREA |
			 FBINFO_HWACCEL_FILLRECT |
			 FBINFO_HWACCEL_IMAGEBLIT |
			 FBINFO_READS_FAST;

	/* don't spam the kernel log after each modesetting */
	if (!accel_status_reported)
		dev_info(&fb->pdev->dev, "2D acceleration is enabled.\n");
	accel_status_reported = true;

	return 0;
}

/*
 * Unmap in the memory mapped IO registers
 */

static void smtc_unmap_mmio(struct smtcfb_info *sfb)
{
	if (sfb && smtc_regbaseaddress)
		smtc_regbaseaddress = NULL;
}

/*
 * Map in the screen memory
 */

static int smtc_map_smem(struct smtcfb_info *sfb,
			 struct pci_dev *pdev, u_long smem_len)
{
	sfb->fb->fix.smem_start = pci_resource_start(pdev, 0);

	if (sfb->fb->var.bits_per_pixel == 32)
		sfb->fb->fix.smem_start += big_addr;

	sfb->fb->fix.smem_len = smem_len;

	sfb->fb->screen_base = sfb->lfb;

	if (!sfb->fb->screen_base) {
		dev_err(&pdev->dev,
			"%s: unable to map screen memory\n", sfb->fb->fix.id);
		return -ENOMEM;
	}

	return 0;
}

/*
 * Unmap in the screen memory
 *
 */
static void smtc_unmap_smem(struct smtcfb_info *sfb)
{
	if (sfb && sfb->fb->screen_base) {
		iounmap(sfb->fb->screen_base);
		sfb->fb->screen_base = NULL;
	}
}

/*
 * We need to wake up the device and make sure its in linear memory mode.
 */
static inline void sm7xx_init_hw(void)
{
	outb_p(0x18, 0x3c4);
	outb_p(0x11, 0x3c5);
}

static int smtcfb_pci_probe(struct pci_dev *pdev,
			    const struct pci_device_id *ent)
{
	struct smtcfb_info *sfb;
	struct fb_info *info;
	u_long smem_size = 0x00800000;	/* default 8MB */
	int err;
	unsigned long mmio_base;

	dev_info(&pdev->dev, "Silicon Motion display driver.\n");

	err = pci_enable_device(pdev);	/* enable SMTC chip */
	if (err)
		return err;

	err = pci_request_region(pdev, 0, "sm7xxfb");
	if (err < 0) {
		dev_err(&pdev->dev, "cannot reserve framebuffer region\n");
		goto failed_regions;
	}

	sprintf(smtcfb_fix.id, "sm%Xfb", ent->device);

	info = framebuffer_alloc(sizeof(*sfb), &pdev->dev);
	if (!info) {
		dev_err(&pdev->dev, "framebuffer_alloc failed\n");
		err = -ENOMEM;
		goto failed_free;
	}

	sfb = info->par;
	sfb->fb = info;
	sfb->chip_id = ent->device;
	sfb->pdev = pdev;
	info->flags = FBINFO_FLAG_DEFAULT;
	info->fbops = &smtcfb_ops;
	info->fix = smtcfb_fix;
	info->var = smtcfb_var;
	info->pseudo_palette = sfb->colreg;
	info->par = sfb;

	pci_set_drvdata(pdev, sfb);

	sm7xx_init_hw();

	/* get mode parameter from smtc_scr_info */
	if (smtc_scr_info.lfb_width != 0) {
		sfb->fb->var.xres = smtc_scr_info.lfb_width;
		sfb->fb->var.yres = smtc_scr_info.lfb_height;
		sfb->fb->var.bits_per_pixel = smtc_scr_info.lfb_depth;
	} else {
		/* default resolution 1024x600 16bit mode */
		sfb->fb->var.xres = SCREEN_X_RES;
		sfb->fb->var.yres = SCREEN_Y_RES;
		sfb->fb->var.bits_per_pixel = SCREEN_BPP;
	}

	big_pixel_depth(sfb->fb->var.bits_per_pixel, smtc_scr_info.lfb_depth);
	/* Map address and memory detection */
	mmio_base = pci_resource_start(pdev, 0);
	pci_read_config_byte(pdev, PCI_REVISION_ID, &sfb->chip_rev_id);

	switch (sfb->chip_id) {
	case 0x710:
	case 0x712:
		sfb->fb->fix.mmio_start = mmio_base + 0x00400000;
		sfb->fb->fix.mmio_len = 0x00400000;
		smem_size = SM712_VIDEOMEMORYSIZE;
		sfb->lfb = ioremap(mmio_base, mmio_addr);
		if (!sfb->lfb) {
			dev_err(&pdev->dev,
				"%s: unable to map memory mapped IO!\n",
				sfb->fb->fix.id);
			err = -ENOMEM;
			goto failed_fb;
		}

		sfb->mmio = sfb->lfb + 0x00700000;
		sfb->dp_port = sfb->lfb + 0x00400000;
		sfb->dp_regs = sfb->lfb + 0x00408000;
		sfb->vp_regs = sfb->lfb + 0x0040c000;

		smtc_regbaseaddress = sfb->mmio;
		smtc_dprbaseaddress = sfb->dp_regs;
		sfb->accel = accel;
		if (sfb->fb->var.bits_per_pixel == 32) {
			sfb->lfb += big_addr;
			dev_info(&pdev->dev, "sfb->lfb=%p\n", sfb->lfb);
		}

		/* set MCLK = 14.31818 * (0x16 / 0x2) */
		smtc_seqw(0x6a, 0x16);
		smtc_seqw(0x6b, 0x02);
		smtc_seqw(0x62, 0x3e);
		/* enable PCI burst */
		smtc_seqw(0x17, 0x20);
		/* enable word swap */
		if (sfb->fb->var.bits_per_pixel == 32)
			seqw17();
		break;
	case 0x720:
		sfb->fb->fix.mmio_start = mmio_base;
		sfb->fb->fix.mmio_len = 0x00200000;
		smem_size = SM722_VIDEOMEMORYSIZE;
		sfb->dp_regs = ioremap(mmio_base, 0x00a00000);
		sfb->lfb = sfb->dp_regs + 0x00200000;
		sfb->mmio = sfb->dp_regs + 0x000c0000;
		sfb->vp_regs = sfb->dp_regs + 0x800;

		smtc_regbaseaddress = sfb->mmio;
		smtc_dprbaseaddress = sfb->dp_regs;
		sfb->accel = false;
		if (accel)
			dev_info(&pdev->dev,
				"2D acceleration is unsupported on SM720\n");

		smtc_seqw(0x62, 0xff);
		smtc_seqw(0x6a, 0x0d);
		smtc_seqw(0x6b, 0x02);
		break;
	default:
		dev_err(&pdev->dev,
			"No valid Silicon Motion display chip was detected!\n");

		goto failed_fb;
	}

	/* can support 32 bpp */
	if (sfb->fb->var.bits_per_pixel == 15)
		sfb->fb->var.bits_per_pixel = 16;

	sfb->fb->var.xres_virtual = sfb->fb->var.xres;
	sfb->fb->var.yres_virtual = sfb->fb->var.yres;
	err = smtc_map_smem(sfb, pdev, smem_size);
	if (err)
		goto failed;

	smtcfb_setmode(sfb);

	err = register_framebuffer(info);
	if (err < 0)
		goto failed;

	dev_info(&pdev->dev,
		 "Silicon Motion SM%X Rev%X primary display mode %dx%d-%d Init Complete.\n",
		 sfb->chip_id, sfb->chip_rev_id, sfb->fb->var.xres,
		 sfb->fb->var.yres, sfb->fb->var.bits_per_pixel);

	return 0;

failed:
	dev_err(&pdev->dev, "Silicon Motion, Inc. primary display init fail.\n");

	smtc_unmap_smem(sfb);
	smtc_unmap_mmio(sfb);
failed_fb:
	framebuffer_release(info);

failed_free:
	pci_release_region(pdev, 0);

failed_regions:
	pci_disable_device(pdev);

	return err;
}

/*
 * 0x710 (LynxEM)
 * 0x712 (LynxEM+)
 * 0x720 (Lynx3DM, Lynx3DM+)
 */
static const struct pci_device_id smtcfb_pci_table[] = {
	{ PCI_DEVICE(0x126f, 0x710), },
	{ PCI_DEVICE(0x126f, 0x712), },
	{ PCI_DEVICE(0x126f, 0x720), },
	{0,}
};

MODULE_DEVICE_TABLE(pci, smtcfb_pci_table);

static void smtcfb_pci_remove(struct pci_dev *pdev)
{
	struct smtcfb_info *sfb;

	sfb = pci_get_drvdata(pdev);
	smtc_unmap_smem(sfb);
	smtc_unmap_mmio(sfb);
	unregister_framebuffer(sfb->fb);
	framebuffer_release(sfb->fb);
	pci_release_region(pdev, 0);
	pci_disable_device(pdev);
}

static int __maybe_unused smtcfb_pci_suspend(struct device *device)
{
	struct pci_dev *pdev = to_pci_dev(device);
	struct smtcfb_info *sfb;

	sfb = pci_get_drvdata(pdev);

	/* set the hw in sleep mode use external clock and self memory refresh
	 * so that we can turn off internal PLLs later on
	 */
	smtc_seqw(0x20, (smtc_seqr(0x20) | 0xc0));
	smtc_seqw(0x69, (smtc_seqr(0x69) & 0xf7));

	console_lock();
	fb_set_suspend(sfb->fb, 1);
	console_unlock();

	/* additionally turn off all function blocks including internal PLLs */
	smtc_seqw(0x21, 0xff);

	return 0;
}

static int __maybe_unused smtcfb_pci_resume(struct device *device)
{
	struct pci_dev *pdev = to_pci_dev(device);
	struct smtcfb_info *sfb;

	sfb = pci_get_drvdata(pdev);

	/* reinit hardware */
	sm7xx_init_hw();
	switch (sfb->chip_id) {
	case 0x710:
	case 0x712:
		/* set MCLK = 14.31818 *  (0x16 / 0x2) */
		smtc_seqw(0x6a, 0x16);
		smtc_seqw(0x6b, 0x02);
		smtc_seqw(0x62, 0x3e);
		/* enable PCI burst */
		smtc_seqw(0x17, 0x20);
		if (sfb->fb->var.bits_per_pixel == 32)
			seqw17();
		break;
	case 0x720:
		smtc_seqw(0x62, 0xff);
		smtc_seqw(0x6a, 0x0d);
		smtc_seqw(0x6b, 0x02);
		break;
	}

	smtc_seqw(0x34, (smtc_seqr(0x34) | 0xc0));
	smtc_seqw(0x33, ((smtc_seqr(0x33) | 0x08) & 0xfb));

	smtcfb_setmode(sfb);

	console_lock();
	fb_set_suspend(sfb->fb, 0);
	console_unlock();

	return 0;
}

static SIMPLE_DEV_PM_OPS(sm7xx_pm_ops, smtcfb_pci_suspend, smtcfb_pci_resume);

static struct pci_driver smtcfb_driver = {
	.name = "smtcfb",
	.id_table = smtcfb_pci_table,
	.probe = smtcfb_pci_probe,
	.remove = smtcfb_pci_remove,
	.driver.pm  = &sm7xx_pm_ops,
};

static int __init sm712fb_init(void)
{
	char *option = NULL;

	if (fb_get_options("sm712fb", &option))
		return -ENODEV;
	if (option && *option)
		mode_option = option;
	sm7xx_vga_setup(mode_option);

	return pci_register_driver(&smtcfb_driver);
}

module_init(sm712fb_init);

static void __exit sm712fb_exit(void)
{
	pci_unregister_driver(&smtcfb_driver);
}

module_exit(sm712fb_exit);

module_param(accel, bool, 0444);
MODULE_PARM_DESC(accel, "Use Acceleration (2D Drawing) Engine (default = 1)");

MODULE_AUTHOR("Siliconmotion ");
MODULE_DESCRIPTION("Framebuffer driver for SMI Graphic Cards");
MODULE_LICENSE("GPL");
