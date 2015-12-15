/* Copyright (c) 2015, The Linux Foundation. All rights reserved.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 and
* only version 2 as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/

#ifndef _EMAC_PHY_H_
#define _EMAC_PHY_H_

enum emac_flow_ctrl {
	EMAC_FC_NONE,
	EMAC_FC_RX_PAUSE,
	EMAC_FC_TX_PAUSE,
	EMAC_FC_FULL,
	EMAC_FC_DEFAULT
};

/* emac_phy
 * @base register file base address space.
 * @irq phy interrupt number.
 * @external true when external phy is used.
 * @addr mii address.
 * @id vendor id.
 * @cur_fc_mode flow control mode in effect.
 * @req_fc_mode flow control mode requested by caller.
 * @disable_fc_autoneg Do not auto-negotiate flow control.
 */
struct emac_phy {
	void __iomem			*base;
	int				irq;

	bool				external;
	bool				uses_gpios;
	u32				addr;
	u16				id[2];
	bool				autoneg;
	u32				autoneg_advertised;
	u32				link_speed;
	bool				link_up;
	/* lock - synchronize access to mdio bus */
	struct mutex			lock;

	/* flow control configuration */
	enum emac_flow_ctrl		cur_fc_mode;
	enum emac_flow_ctrl		req_fc_mode;
	bool				disable_fc_autoneg;
};

struct emac_adapter;
struct platform_device;

int  emac_phy_read(struct emac_adapter *adpt, u16 phy_addr, u16 reg_addr,
		   u16 *phy_data);
int  emac_phy_write(struct emac_adapter *adpt, u16 phy_addr, u16 reg_addr,
		    u16 phy_data);
int  emac_phy_config(struct platform_device *pdev, struct emac_adapter *adpt);
int  emac_phy_up(struct emac_adapter *adpt);
void emac_phy_down(struct emac_adapter *adpt);
void emac_phy_reset(struct emac_adapter *adpt);
void emac_phy_periodic_check(struct emac_adapter *adpt);
int  emac_phy_external_init(struct emac_adapter *adpt);
int  emac_phy_link_setup(struct emac_adapter *adpt, u32 speed, bool autoneg,
			 bool fc);
int  emac_phy_link_check(struct emac_adapter *adpt, u32 *speed, bool *link_up);
void emac_phy_link_speed_get(struct emac_adapter *adpt, u32 *speed);

#endif /* _EMAC_PHY_H_ */
