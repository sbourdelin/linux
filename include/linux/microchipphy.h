/*
 * Copyright (C) 2015 Microchip Technology
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _MICROCHIPPHY_H
#define _MICROCHIPPHY_H

#define LAN88XX_INT_MASK			(0x19)
#define LAN88XX_INT_MASK_MDINTPIN_EN		(0x8000)
#define LAN88XX_INT_MASK_SPEED_CHANGE		(0x4000)
#define LAN88XX_INT_MASK_LINK_CHANGE		(0x2000)
#define LAN88XX_INT_MASK_FDX_CHANGE		(0x1000)
#define LAN88XX_INT_MASK_AUTONEG_ERR		(0x0800)
#define LAN88XX_INT_MASK_AUTONEG_DONE		(0x0400)
#define LAN88XX_INT_MASK_POE_DETECT		(0x0200)
#define LAN88XX_INT_MASK_SYMBOL_ERR		(0x0100)
#define LAN88XX_INT_MASK_FAST_LINK_FAIL		(0x0080)
#define LAN88XX_INT_MASK_WOL_EVENT		(0x0040)
#define LAN88XX_INT_MASK_EXTENDED_INT		(0x0020)
#define LAN88XX_INT_MASK_RESERVED		(0x0010)
#define LAN88XX_INT_MASK_FALSE_CARRIER		(0x0008)
#define LAN88XX_INT_MASK_LINK_SPEED_DS		(0x0004)
#define LAN88XX_INT_MASK_MASTER_SLAVE_DONE	(0x0002)
#define LAN88XX_INT_MASK_RX__ER			(0x0001)

#define LAN88XX_INT_STS				(0x1A)
#define LAN88XX_INT_STS_INT_ACTIVE		(0x8000)
#define LAN88XX_INT_STS_SPEED_CHANGE		(0x4000)
#define LAN88XX_INT_STS_LINK_CHANGE		(0x2000)
#define LAN88XX_INT_STS_FDX_CHANGE		(0x1000)
#define LAN88XX_INT_STS_AUTONEG_ERR		(0x0800)
#define LAN88XX_INT_STS_AUTONEG_DONE		(0x0400)
#define LAN88XX_INT_STS_POE_DETECT		(0x0200)
#define LAN88XX_INT_STS_SYMBOL_ERR		(0x0100)
#define LAN88XX_INT_STS_FAST_LINK_FAIL		(0x0080)
#define LAN88XX_INT_STS_WOL_EVENT		(0x0040)
#define LAN88XX_INT_STS_EXTENDED_INT		(0x0020)
#define LAN88XX_INT_STS_RESERVED		(0x0010)
#define LAN88XX_INT_STS_FALSE_CARRIER		(0x0008)
#define LAN88XX_INT_STS_LINK_SPEED_DS		(0x0004)
#define LAN88XX_INT_STS_MASTER_SLAVE_DONE	(0x0002)
#define LAN88XX_INT_STS_RX_ER			(0x0001)

#define LAN88XX_EXT_PAGE_ACCESS			(0x1F)
#define LAN88XX_EXT_PAGE_SPACE_0		(0x0000)
#define LAN88XX_EXT_PAGE_SPACE_1		(0x0001)
#define LAN88XX_EXT_PAGE_SPACE_2		(0x0002)

/* Extended Register Page 1 space */
#define LAN88XX_EXT_MODE_CTRL			(0x13)
#define LAN88XX_EXT_MODE_CTRL_MDIX_MASK		(0x000C)
#define LAN88XX_EXT_MODE_CTRL_AUTO_MDIX		(0x0000)
#define LAN88XX_EXT_MODE_CTRL_MDI		(0x0008)
#define LAN88XX_EXT_MODE_CTRL_MDI_X		(0x000C)

/* MMD 3 Registers */
#define	LAN88XX_MMD3_CHIP_ID			(32877)
#define	LAN88XX_MMD3_CHIP_REV			(32878)

#endif /* _MICROCHIPPHY_H */
