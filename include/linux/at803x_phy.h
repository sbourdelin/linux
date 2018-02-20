/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _AT803X_PHY_H
#define _PHY_AT803X_PHY

#define ATH8030_PHY_ID		0x004dd076
#define ATH8031_PHY_ID		0x004dd074
#define ATH8035_PHY_ID		0x004dd072
#define AT803X_PHY_ID_MASK	0xffffffef

#define AT8031_HIBERNATE	0x0B
#define AT8031_PS_HIB_EN	0x8000 /* Hibernate enable */

int at803x_debug_reg_mask(struct phy_device *phydev, u16 reg,
			  u16 clear, u16 set);

#endif /* _AT803X_PHY_H */
