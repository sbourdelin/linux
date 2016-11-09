/*
 * Synopsys Designware HDMI RX PHY generic interface
 *
 * Copyright (C) 2016 Synopsys, Inc.
 * Jose Abreu <joabreu@synopsys.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __DW_PHY_PDATA_H__
#define __DW_PHY_PDATA_H__

#define DW_PHY_IOCTL_EQ_INIT		_IOW('R', 1, int)
#define DW_PHY_IOCTL_SET_HDMI2		_IOW('R', 2, int)
#define DW_PHY_IOCTL_SET_SCRAMBLING	_IOW('R', 3, int)
#define DW_PHY_IOCTL_CONFIG		_IOW('R', 4, int)

struct dw_phy_command {
	int result;
	unsigned char res;
	bool hdmi2;
	bool nacq;
	bool scrambling;
};

struct dw_phy_funcs {
	void (*write)(void *arg, u16 val, u16 addr);
	u16 (*read)(void *arg, u16 addr);
	void (*reset)(void *arg, int enable);
	void (*pddq)(void *arg, int enable);
	void (*svsmode)(void *arg, int enable);
	void (*zcal_reset)(void *arg);
	bool (*zcal_done)(void *arg);
	bool (*tmds_valid)(void *arg);
};

struct dw_phy_pdata {
	unsigned int version;
	unsigned int cfg_clk;
	const struct dw_phy_funcs *funcs;
	void *funcs_arg;
};

#endif /* __DW_PHY_PDATA_H__ */

