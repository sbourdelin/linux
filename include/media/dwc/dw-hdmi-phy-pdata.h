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
#define DW_PHY_IOCTL_CONFIG		_IOW('R',2,int)

/**
 * struct dw_phy_eq_command - Command arguments for HDMI PHY equalizer
 * 	algorithm.
 *
 * @nacq: Number of acquisitions to get.
 *
 * @force: Force equalizer algorithm even if the MPLL status didn't change
 * from previous run.
 *
 * @result: Result from the equalizer algorithm. Shall be zero if equalizer
 * ran with success or didn't run because the video mode does not need
 * equalizer (for low clock values).
 */
struct dw_phy_eq_command {
	u16 nacq;
	bool force;
	int result;
};

/**
 * struct dw_phy_config_command - Command arguments for HDMI PHY configuration
 * function.
 *
 * @color_depth: Color depth of the video mode being received.
 *
 * @hdmi2: Must be set to true if the video mode being received has a data
 * rate above 3.4Gbps (generally HDMI 2.0 video modes, like 4k@60Hz).
 *
 * @scrambling: Must be set to true if scrambling is currently enabled.
 * Scrambling is enabled by source when SCDC scrambling_en field is set to 1.
 *
 * @result: Result from the configuration function. Shall be zero if phy was
 * configured with success.
 */
struct dw_phy_config_command {
	unsigned char color_depth;
	bool hdmi2;
	bool scrambling;
	int result;
};

/**
 * struct dw_phy_funcs - Set of callbacks used to communicate between phy
 * 	and hdmi controller. Controller must correctly fill these callbacks
 * 	before probbing the phy driver.
 *
 * @write: write callback. Write value 'val' into address 'addr' of phy.
 *
 * @read: read callback. Read address 'addr' and return the value.
 *
 * @reset: reset callback. Activate phy reset. Active high.
 *
 * @pddq: pddq callback. Activate phy configuration mode. Active high.
 *
 * @svsmode: svsmode callback. Activate phy retention mode. Active low.
 *
 * @zcal_reset: zcal reset callback. Restart the impedance calibration
 * procedure. Active high. This is only used in prototyping and not in real
 * ASIC. Callback shall be empty (but non NULL) in ASIC cases.
 *
 * @zcal_done: zcal done callback. Return the current status of impedance
 * calibration procedure. This is only used in prototyping and not in real
 * ASIC. Shall return always true in ASIC cases.
 *
 * @tmds_valid: TMDS valid callback. Return the current status of TMDS signal
 * that comes from phy and feeds controller. This is read from a controller
 * register.
 */
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

/**
 * struct dw_phy_pdata - Platform data definition for Synopsys HDMI PHY.
 *
 * @funcs: set of callbacks that must be correctly filled and supplied to phy.
 * See @dw_phy_funcs.
 *
 * @funcs_arg: parameter that is supplied to callbacks along with the function
 * parameters.
 */
struct dw_phy_pdata {
	const struct dw_phy_funcs *funcs;
	void *funcs_arg;
};

#endif /* __DW_HDMI_PHY_PDATA_H__ */
