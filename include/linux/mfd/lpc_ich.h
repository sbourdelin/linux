/*
 *  linux/drivers/mfd/lpc_ich.h
 *
 *  Copyright (c) 2012 Extreme Engineering Solution, Inc.
 *  Author: Aaron Sierra <asierra@xes-inc.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License 2 as published
 *  by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#ifndef LPC_ICH_H
#define LPC_ICH_H

/* GPIO resources */
#define ICH_RES_GPIO	0
#define ICH_RES_GPE0	1

/* GPIO compatibility */
enum {
	ICH_I3100_GPIO,
	ICH_V5_GPIO,
	ICH_V6_GPIO,
	ICH_V7_GPIO,
	ICH_V9_GPIO,
	ICH_V10CORP_GPIO,
	ICH_V10CONS_GPIO,
	AVOTON_GPIO,
};

struct lpc_ich_info {
	char name[32];
	unsigned int iTCO_version;
	unsigned int gpio_version;
	u8 use_gpio;
};

/* chipset related info */
enum lpc_chipsets {
	LPC_ICH = 0,	/* ICH */
	LPC_ICH0,	/* ICH0 */
	LPC_ICH2,	/* ICH2 */
	LPC_ICH2M,	/* ICH2-M */
	LPC_ICH3,	/* ICH3-S */
	LPC_ICH3M,	/* ICH3-M */
	LPC_ICH4,	/* ICH4 */
	LPC_ICH4M,	/* ICH4-M */
	LPC_CICH,	/* C-ICH */
	LPC_ICH5,	/* ICH5 & ICH5R */
	LPC_6300ESB,	/* 6300ESB */
	LPC_ICH6,	/* ICH6 & ICH6R */
	LPC_ICH6M,	/* ICH6-M */
	LPC_ICH6W,	/* ICH6W & ICH6RW */
	LPC_631XESB,	/* 631xESB/632xESB */
	LPC_ICH7,	/* ICH7 & ICH7R */
	LPC_ICH7DH,	/* ICH7DH */
	LPC_ICH7M,	/* ICH7-M & ICH7-U */
	LPC_ICH7MDH,	/* ICH7-M DH */
	LPC_NM10,	/* NM10 */
	LPC_ICH8,	/* ICH8 & ICH8R */
	LPC_ICH8DH,	/* ICH8DH */
	LPC_ICH8DO,	/* ICH8DO */
	LPC_ICH8M,	/* ICH8M */
	LPC_ICH8ME,	/* ICH8M-E */
	LPC_ICH9,	/* ICH9 */
	LPC_ICH9R,	/* ICH9R */
	LPC_ICH9DH,	/* ICH9DH */
	LPC_ICH9DO,	/* ICH9DO */
	LPC_ICH9M,	/* ICH9M */
	LPC_ICH9ME,	/* ICH9M-E */
	LPC_ICH10,	/* ICH10 */
	LPC_ICH10R,	/* ICH10R */
	LPC_ICH10D,	/* ICH10D */
	LPC_ICH10DO,	/* ICH10DO */
	LPC_PCH,	/* PCH Desktop Full Featured */
	LPC_PCHM,	/* PCH Mobile Full Featured */
	LPC_P55,	/* P55 */
	LPC_PM55,	/* PM55 */
	LPC_H55,	/* H55 */
	LPC_QM57,	/* QM57 */
	LPC_H57,	/* H57 */
	LPC_HM55,	/* HM55 */
	LPC_Q57,	/* Q57 */
	LPC_HM57,	/* HM57 */
	LPC_PCHMSFF,	/* PCH Mobile SFF Full Featured */
	LPC_QS57,	/* QS57 */
	LPC_3400,	/* 3400 */
	LPC_3420,	/* 3420 */
	LPC_3450,	/* 3450 */
	LPC_EP80579,	/* EP80579 */
	LPC_CPT,	/* Cougar Point */
	LPC_CPTD,	/* Cougar Point Desktop */
	LPC_CPTM,	/* Cougar Point Mobile */
	LPC_PBG,	/* Patsburg */
	LPC_DH89XXCC,	/* DH89xxCC */
	LPC_PPT,	/* Panther Point */
	LPC_LPT,	/* Lynx Point */
	LPC_LPT_LP,	/* Lynx Point-LP */
	LPC_WBG,	/* Wellsburg */
	LPC_AVN,	/* Avoton SoC */
	LPC_BAYTRAIL,   /* Bay Trail SoC */
	LPC_COLETO,	/* Coleto Creek */
	LPC_WPT_LP,	/* Wildcat Point-LP */
	LPC_BRASWELL,	/* Braswell SoC */
	LPC_LEWISBURG,	/* Lewisburg */
	LPC_9S,		/* 9 Series */
};

#endif
