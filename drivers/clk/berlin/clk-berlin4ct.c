/*
 * Copyright (c) 2015 Marvell Technology Group Ltd.
 *
 * Author: Jisheng Zhang <jszhang@marvell.com>
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
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/clk-provider.h>

#include "clk.h"

static struct clk_onecell_data gateclk_data;
static struct clk_onecell_data clk_data;

static const struct gateclk_desc berlin4ct_gates[] __initconst = {
	{ "tspsysclk",		"perifsysclk",	0 },
	{ "usb0coreclk",	"perifsysclk",	1 },
	{ "zspsysclk",		"perifsysclk",	2 },
	{ "sdiosysclk",		"perifsysclk",	3 },
	{ "ethcoreclk",		"perifsysclk",	4 },
	{ "pcie0sys",		"perifsysclk",	6 },
	{ "sata0core",		"perifsysclk",	7 },
	{ "nfcsysclk",		"perifsysclk",	8 },
	{ "emmcsysclk",		"perifsysclk",	9 },
	{ "ihb0sysclk",		"perifsysclk",	10 },
};

static void __init berlin4ct_gateclk_setup(struct device_node *np)
{
	int n = ARRAY_SIZE(berlin4ct_gates);

	berlin_gateclk_setup(np, berlin4ct_gates, &gateclk_data, n);
}
CLK_OF_DECLARE(berlin4ct_gateclk, "marvell,berlin4ct-gateclk",
	       berlin4ct_gateclk_setup);

static const struct clk_desc berlin4ct_descs[] __initconst = {
	{ "cpufastrefclk",	0x0 },
	{ "memfastrefclk",	0x4 },
	{ "cfgclk",		0x20,	CLK_IGNORE_UNUSED },
	{ "perifsysclk",	0x24,	CLK_IGNORE_UNUSED },
	{ "hbclk",		0x28 },
	{ "atbclk",		0x2c },
	{ "decoderclk",		0x40 },
	{ "decoderm3clk",	0x44 },
	{ "decoderpcubeclk",	0x48 },
	{ "encoderclk",		0x4c },
	{ "ovpcoreclk",		0x50 },
	{ "gfx2dcoreclk",	0x60 },
	{ "gfx3dcoreclk",	0x64 },
	{ "gfx3dshclk",		0x68 },
	{ "gfx3dsysclk",	0x6c },
	{ "gfx2dsysclk",	0x70 },
	{ "aviosysclk",		0x80 },
	{ "vppsysclk",		0x84 },
	{ "eddcclk",		0x88 },
	{ "aviobiuclk",		0x8c },
	{ "zspclk",		0xa0 },
	{ "tspclk",		0xc0 },
	{ "tsprefclk",		0xc4 },
	{ "ndsclk",		0xc8 },
	{ "nocsclk",		0xcc },
	{ "apbcoreclk",		0xd0,	CLK_IGNORE_UNUSED },
	{ "emmcclk",		0xe0 },
	{ "sd0clk",		0xe4 },
	{ "sd1clk",		0xe8 },
	{ "dllmstrefclk",	0xec },
	{ "gethrgmiiclk",	0xf0 },
	{ "gethrgmiisysclk",	0xf4 },
	{ "usim0clk",		0x100 },
	{ "pcietestclk",	0x110 },
	{ "usb2testclk",	0x120 },
	{ "usb3testclk",	0x124 },
	{ "usb3coreclk",	0x128 },
	{ "nfceccclk",		0x130 },
	{ "bcmclk",		0x140 },
};

static void __init berlin4ct_clk_setup(struct device_node *np)
{
	int n = ARRAY_SIZE(berlin4ct_descs);

	berlin_clk_setup(np, berlin4ct_descs, &clk_data, n);
}
CLK_OF_DECLARE(berlin4ct_clk, "marvell,berlin4ct-clk",
	       berlin4ct_clk_setup);
