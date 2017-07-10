/*
 * Synopsys Designware HDMI Receiver controller platform data
 *
 * This Synopsys dw-hdmi-rx software and associated documentation
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

#ifndef __DW_HDMI_RX_PDATA_H__
#define __DW_HDMI_RX_PDATA_H__

#define DW_HDMI_RX_DRVNAME			"dw-hdmi-rx"

/* Notify events */
#define DW_HDMI_NOTIFY_IS_OFF		1
#define DW_HDMI_NOTIFY_INPUT_CHANGED	2
#define DW_HDMI_NOTIFY_AUDIO_CHANGED	3
#define DW_HDMI_NOTIFY_IS_STABLE	4

/**
 * struct dw_hdmi_rx_pdata - Platform Data configuration for HDMI receiver.
 *
 * @dw_5v_status: 5v status callback. Shall return the status of the given
 * input, i.e. shall be true if a cable is connected to the specified input.
 *
 * @dw_5v_clear: 5v clear callback. Shall clear the interrupt associated with
 * the 5v sense controller.
 *
 * @dw_5v_arg: Argument to be used with the 5v sense callbacks.
 *
 * @dw_zcal_reset: Impedance calibration reset callback. Shall be called when
 * the impedance calibration needs to be restarted. This is used by phy driver
 * only.
 *
 * @dw_zcal_done: Impedance calibration status callback. Shall return true if
 * the impedance calibration procedure has ended. This is used by phy driver
 * only.
 *
 * @dw_zcal_arg: Argument to be used with the ZCAL calibration callbacks.
 */
struct dw_hdmi_rx_pdata {
	/* 5V sense interface */
	bool (*dw_5v_status)(void __iomem *regs, int input);
	void (*dw_5v_clear)(void __iomem *regs);
	void __iomem *dw_5v_arg;
	/* Zcal interface */
	void (*dw_zcal_reset)(void __iomem *regs);
	bool (*dw_zcal_done)(void __iomem *regs);
	void __iomem *dw_zcal_arg;
};

#endif /* __DW_HDMI_RX_PDATA_H__ */
