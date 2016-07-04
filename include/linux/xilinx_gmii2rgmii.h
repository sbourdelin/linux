#ifndef _GMII2RGMII_H
#define _GMII2RGMII_H

#include <linux/of.h>
#include <linux/phy.h>
#include <linux/mii.h>

#define XILINX_GMII2RGMII_FULLDPLX		BMCR_FULLDPLX
#define XILINX_GMII2RGMII_SPEED1000		BMCR_SPEED1000
#define XILINX_GMII2RGMII_SPEED100		BMCR_SPEED100
#define XILINX_GMII2RGMII_REG_NUM			0x10

struct gmii2rgmii {
	struct net_device	*dev;
	struct mii_bus		*mii_bus;
	struct phy_device	*gmii2rgmii_phy_dev;
	void			*platform_data;
	int (*mdio_write)(struct mii_bus *bus, int mii_id, int reg,
			  u16 val);
	void (*fix_mac_speed)(struct gmii2rgmii *xphy, unsigned int speed);
};

extern int gmii2rgmii_phyprobe(struct gmii2rgmii *xphy);
#endif
