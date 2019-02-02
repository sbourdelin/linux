/*
 * Silicon Motion SM712 frame buffer device
 *
 * Copyright (C) 2006 Silicon Motion Technology Corp.
 * Authors:	Ge Wang, gewang@siliconmotion.com
 *		Boyod boyod.yang@siliconmotion.com.cn
 *
 * Copyright (C) 2009 Lemote, Inc.
 * Author: Wu Zhangjin, wuzhangjin@gmail.com
 *
 * Copyright (C) 2014, 2019 Yifeng Li.
 * Author:   Yifeng Li <tomli@tomli.me>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 *
 *  This file incorporates work covered by the following copyright and
 *  permission notice:
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

#define FB_ACCEL_SMI_LYNX 88

#define SCREEN_X_RES      1024
#define SCREEN_Y_RES      600
#define SCREEN_BPP        16

/*Assume SM712 graphics chip has 4MB VRAM */
#define SM712_VIDEOMEMORYSIZE	  0x00400000
/*Assume SM722 graphics chip has 8MB VRAM */
#define SM722_VIDEOMEMORYSIZE	  0x00800000

#define dac_reg	(0x3c8)
#define dac_val	(0x3c9)

extern void __iomem *smtc_regbaseaddress;
#define smtc_mmiowb(dat, reg)	writeb(dat, smtc_regbaseaddress + reg)

#define smtc_mmiorb(reg)	readb(smtc_regbaseaddress + reg)

#define SIZE_SR00_SR04      (0x04 - 0x00 + 1)
#define SIZE_SR10_SR24      (0x24 - 0x10 + 1)
#define SIZE_SR30_SR75      (0x75 - 0x30 + 1)
#define SIZE_SR80_SR93      (0x93 - 0x80 + 1)
#define SIZE_SRA0_SRAF      (0xAF - 0xA0 + 1)
#define SIZE_GR00_GR08      (0x08 - 0x00 + 1)
#define SIZE_AR00_AR14      (0x14 - 0x00 + 1)
#define SIZE_CR00_CR18      (0x18 - 0x00 + 1)
#define SIZE_CR30_CR4D      (0x4D - 0x30 + 1)
#define SIZE_CR90_CRA7      (0xA7 - 0x90 + 1)

static inline void smtc_crtcw(u8 reg, u8 val)
{
	smtc_mmiowb(reg, 0x3d4);
	smtc_mmiowb(val, 0x3d5);
}

static inline void smtc_grphw(u8 reg, u8 val)
{
	smtc_mmiowb(reg, 0x3ce);
	smtc_mmiowb(val, 0x3cf);
}

static inline void smtc_attrw(u8 reg, u8 val)
{
	smtc_mmiorb(0x3da);
	smtc_mmiowb(reg, 0x3c0);
	smtc_mmiorb(0x3c1);
	smtc_mmiowb(val, 0x3c0);
}

static inline void smtc_seqw(u8 reg, u8 val)
{
	smtc_mmiowb(reg, 0x3c4);
	smtc_mmiowb(val, 0x3c5);
}

static inline u8 smtc_seqr(u8 reg)
{
	smtc_mmiowb(reg, 0x3c4);
	return smtc_mmiorb(0x3c5);
}

/*
 * DPR (2D drawing engine)
 */
#define DPR_COORDS(x, y)		(((x) << 16) | (y))

#define SCR_DE_STATUS			0x16
#define SCR_DE_STATUS_MASK		0x18
#define SCR_DE_ENGINE_IDLE		0x10

#define DPR_BASE			0x00408000
#define DPR_SRC_COORDS			0x00
#define DPR_DST_COORDS			0x04
#define DPR_SPAN_COORDS			0x08
#define DPR_DE_CTRL			0x0c
#define DPR_PITCH			0x10
#define DPR_FG_COLOR			0x14
#define DPR_BG_COLOR			0x18
#define DPR_STRETCH			0x1c
#define DPR_DE_FORMAT_SELECT		0x1e
#define DPR_COLOR_COMPARE		0x20
#define DPR_COLOR_COMPARE_MASK		0x24
#define DPR_BYTE_BIT_MASK		0x28
#define DPR_CROP_TOPLEFT_COORDS		0x2c
#define DPR_CROP_BOTRIGHT_COORDS	0x30
#define DPR_MONO_PATTERN_LO32		0x34
#define DPR_MONO_PATTERN_HI32		0x38
#define DPR_SRC_WINDOW			0x3c
#define DPR_SRC_BASE			0x40
#define DPR_DST_BASE			0x44

#define DE_CTRL_START			0x80000000
#define DE_CTRL_RTOL			0x08000000
#define DE_CTRL_COMMAND_MASK		0x001f0000
#define DE_CTRL_COMMAND_SHIFT			16
#define DE_CTRL_COMMAND_BITBLT			0x00
#define DE_CTRL_COMMAND_SOLIDFILL		0x01
#define DE_CTRL_COMMAND_HOSTWRITE		0x08
#define DE_CTRL_ROP2_SELECT		0x00008000
#define DE_CTRL_ROP2_SRC_IS_PATTERN	0x00004000
#define DE_CTRL_ROP2_SHIFT			0
#define DE_CTRL_ROP2_COPY			0x0c
#define DE_CTRL_HOST_SHIFT			22
#define DE_CTRL_HOST_SRC_IS_MONO		0x01
#define DE_CTRL_FORMAT_XY			0x00
#define DE_CTRL_FORMAT_24BIT			0x30

/*
 * 32-bit I/O for 2D opeartions.
 */
extern void __iomem *smtc_dprbaseaddress;   /* DPR, 2D control registers */

static inline u8 smtc_dprr(u8 reg)
{
	return readl(smtc_dprbaseaddress + reg);
}

static inline void smtc_dprw(u8 reg, u32 val)
{
	writel(val, smtc_dprbaseaddress + reg);
}

static inline void smtc_dprw_16(u8 reg, u16 val)
{
	writew(val, smtc_dprbaseaddress + reg);
}

static inline u32 pad_to_dword(const u8 *bytes, int length)
{
	u32 dword = 0;

	switch (length) {
#ifdef __BIG_ENDIAN
	case 3:
		dword |= bytes[2] << 8;
	/* fallthrough */
	case 2:
		dword |= bytes[1] << 16;
	/* fallthrough */
	case 1:
		dword |= bytes[0] << 24;
		break;
#else
	case 3:
		dword |= bytes[2] << 16;
	/* fallthrough */
	case 2:
		dword |= bytes[1] << 8;
	/* fallthrough */
	case 1:
		dword |= bytes[0];
		break;
#endif
	}
	return dword;
}

/* The next structure holds all information relevant for a specific video mode.
 */

struct modeinit {
	int mmsizex;
	int mmsizey;
	int bpp;
	int hz;
	unsigned char init_misc;
	unsigned char init_sr00_sr04[SIZE_SR00_SR04];
	unsigned char init_sr10_sr24[SIZE_SR10_SR24];
	unsigned char init_sr30_sr75[SIZE_SR30_SR75];
	unsigned char init_sr80_sr93[SIZE_SR80_SR93];
	unsigned char init_sra0_sraf[SIZE_SRA0_SRAF];
	unsigned char init_gr00_gr08[SIZE_GR00_GR08];
	unsigned char init_ar00_ar14[SIZE_AR00_AR14];
	unsigned char init_cr00_cr18[SIZE_CR00_CR18];
	unsigned char init_cr30_cr4d[SIZE_CR30_CR4D];
	unsigned char init_cr90_cra7[SIZE_CR90_CRA7];
};

#ifdef __BIG_ENDIAN
#define pal_rgb(r, g, b, val)	(((r & 0xf800) >> 8) | \
				((g & 0xe000) >> 13) | \
				((g & 0x1c00) << 3) | \
				((b & 0xf800) >> 3))
#define big_addr		0x800000
#define mmio_addr		0x00800000
#define seqw17()		smtc_seqw(0x17, 0x30)
#define big_pixel_depth(p, d)	{if (p == 24) {p = 32; d = 32; } }
#define big_swap(p)		((p & 0xff00ff00 >> 8) | (p & 0x00ff00ff << 8))
#else
#define pal_rgb(r, g, b, val)	val
#define big_addr		0
#define mmio_addr		0x00c00000
#define seqw17()		do { } while (0)
#define big_pixel_depth(p, d)	do { } while (0)
#define big_swap(p)		p
#endif
