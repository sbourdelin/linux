/*
 * Synopsys Designware HDMI PHY platform data
 *
 * This Synopsys dw-hdmi-phy software and associated documentation
 * (hereinafter the "Software") is an unsupported proprietary work of
 * Synopsys, Inc. unless otherwise expressly agreed to in writing between
 * Synopsys and you. The Software IS NOT an item of Licensed Software or a
 * Licensed Product under any End User Software License Agreement or
 * Agreement for Licensed Products with Synopsys or any supplement thereto.
 * Synopsys is a registered trademark of Synopsys, Inc. Other names included
 * in the SOFTWARE may be the trademarks of their respective owners.
 *
 * The contents of this file are dual-licensed; you may select either version 2
 * of the GNU General Public License (“GPL”) or the MIT license (“MIT”).
 *
 * Copyright (c) 2017 Synopsys, Inc. and/or its affiliates.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS"  WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING, BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT, OR OTHERWISE
 * ARISING FROM, OUT OF, OR IN CONNECTION WITH THE SOFTWARE THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef __DW_HDMI_PHY_PDATA_H__
#define __DW_HDMI_PHY_PDATA_H__

#define DW_PHY_E405_DRVNAME	"dw-hdmi-phy-e405"

#define DW_PHY_IOCTL_EQ_INIT		_IOW('R',1,int)
#define DW_PHY_IOCTL_SET_HDMI2		_IOW('R',2,int)
#define DW_PHY_IOCTL_SET_SCRAMBLING	_IOW('R',3,int)
#define DW_PHY_IOCTL_CONFIG		_IOW('R',4,int)

struct dw_phy_command {
	int result;
	unsigned char res;
	bool hdmi2;
	u16 nacq;
	bool scrambling;
	bool force;
};

struct dw_phy_funcs {
	void (*write) (void *arg, u16 val, u16 addr);
	u16 (*read) (void *arg, u16 addr);
	void (*reset) (void *arg, int enable);
	void (*pddq) (void *arg, int enable);
	void (*svsmode) (void *arg, int enable);
	void (*zcal_reset) (void *arg);
	bool (*zcal_done) (void *arg);
	bool (*tmds_valid) (void *arg);
};

struct dw_phy_pdata {
	unsigned int version;
	unsigned int cfg_clk;
	const struct dw_phy_funcs *funcs;
	void *funcs_arg;
};

#endif /* __DW_HDMI_PHY_PDATA_H__ */
