/*
 *  This file is provided under a dual BSD/GPLv2 license.  When using or
 *  redistributing this file, you may do so under either license.
 *
 *  GPL LICENSE SUMMARY
 *
 *  Copyright(c) 2015-17 Intel Corporation.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of version 2 of the GNU General Public License as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  BSD LICENSE
 *
 *  Copyright(c) 2015-17 Intel Corporation.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in
 *      the documentation and/or other materials provided with the
 *      distribution.
 *    * Neither the name of Intel Corporation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * First written by Hardik T Shah
 * Rewrite by Vinod
 */

#include <linux/acpi.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include <linux/soundwire/soundwire.h>
#include "sdw_intel_shim.h"
#include "sdw_cadence.h"

/*
 * IO Calls
 */

static enum sdw_command_response cdns_fill_msg_resp(struct cdns_sdw *sdw,
				struct sdw_msg *msg, int count, int offset)
{
	int nack = 0, no_ack = 0;
	int i;

	/* check response for the two writes */
	for (i = 0; i < count; i++) {
		if (!(sdw->response_buf[i] & CDNS_MCP_RESP_ACK)) {
			no_ack = 1;
			dev_err(sdw->dev, "Msg Ack not recevied\n");
			if (sdw->response_buf[i] & CDNS_MCP_RESP_NACK) {
				nack = 1;
				dev_err(sdw->dev, "Msg NACK recevied\n");
			}
		}
	}

	/*
	 * For NACK or NO ack, don't return err if we are in Broadcast mode
	 */
	if (nack) {
		dev_err(sdw->dev, "Msg NACKed for slave %d\n", msg->device);
		return -EREMOTEIO;
	} else if (no_ack) {
		dev_err(sdw->dev, "Msg ignored for slave %d\n", msg->device);
		return -EREMOTEIO;
	}

	/* fill the response */
	for (i = 0; i < count; i++)
		msg->buf[i + offset] =
			sdw->response_buf[i] >> SDW_REG_SHIFT(CDNS_MCP_RESP_RDATA);

	return SDW_CMD_OK;
}


/*
 * IRQ handling
 */

static int cdns_read_response(struct cdns_sdw *sdw)
{
	u32 num_resp, cmd_base;
	int i;

	num_resp = cdns_sdw_readl(sdw, CDNS_MCP_FIFOSTAT);
	num_resp &= CDNS_MCP_RX_FIFO_AVAIL;

	cmd_base = CDNS_MCP_CMD_BASE;

	for (i = 0; i < num_resp; i++) {
		sdw->response_buf[i] = cdns_sdw_readl(sdw, cmd_base);
		cmd_base += CDNS_MCP_CMD_LEN;
	}

	return 0;
}

static int cdns_update_slave_status(struct cdns_sdw *sdw,
					u32 slave0, u32 slave1)
{
	enum sdw_slave_status status[SDW_MAX_DEVICES];
	u64 slave, mask;
	int i;

	/* combine the two status */
	slave = ((u64)slave1 << 32) | slave0;

	memset(status, 0, sizeof(status));

	for (i = 0; i <= SDW_MAX_DEVICES; i++) {
		mask = (slave >> ( i * CDNS_MCP_SLAVE_STATUS_NUM)) &
				CDNS_MCP_SLAVE_STATUS_BITS;

		switch (mask) {
		case CDNS_MCP_SLAVE_INTSTAT_NPRESENT:
			status[i] = SDW_SLAVE_NOT_PRESENT;
			break;

		case CDNS_MCP_SLAVE_INTSTAT_ATTACHED:
			status[i] = SDW_SLAVE_PRESENT;
			break;

		case CDNS_MCP_SLAVE_INTSTAT_ALERT:
			status[i] = SDW_SLAVE_ALERT;
			break;

		case CDNS_MCP_SLAVE_INTSTAT_RESERVED:
			status[i] = SDW_SLAVE_RESERVED;
			break;

		default:
			dev_err(sdw->dev,
				"found invalid status %llx for slave %i\n",
				mask, i);
			break;
		}
	}

	return sdw_handle_slave_status(&sdw->bus, status);
}

static irqreturn_t cdns_irq(int irq, void *dev_id)
{
	struct cdns_sdw *sdw = dev_id;
	u32 int_status;
	int ret = IRQ_HANDLED;

	int_status = cdns_sdw_readl(sdw, CDNS_MCP_INTSTAT);
	if (!(int_status & CDNS_MCP_INT_IRQ)) {
		return IRQ_NONE;
	} else {
		cdns_read_response(sdw);
		if (sdw->async) {
			cdns_fill_msg_resp(sdw,
					sdw->async->msg, sdw->async->length, 0);
			complete(&sdw->async->complete);
			sdw->async = NULL;
		} else
			complete(&sdw->tx_complete);
	}

	if (int_status & CDNS_MCP_INT_CTRL_CLASH) {
		/* slave is driving data line during control word */
		dev_err_ratelimited(sdw->dev, "Bus clash for control word\n");
		WARN_ONCE(1, "Bus clash for control word\n");
		int_status |= CDNS_MCP_INT_CTRL_CLASH;
	}

	if (int_status & CDNS_MCP_INT_DATA_CLASH) {
		/*
		 * multiple slaves trying to driver bus, or issue with
		 * ownership of data bits or slave gone bonkers
		 */
		dev_err_ratelimited(sdw->dev, "Bus clash for data word\n");
		WARN_ONCE(1, "Bus clash for data word\n");
		int_status |= CDNS_MCP_INT_DATA_CLASH;
	}

	if (int_status & CDNS_MCP_INT_SLAVE_MASK) {
		/* mask the slave interrupt and wake thread */
		cdns_sdw_updatel(sdw, CDNS_SDW_INTMASK,
				CDNS_MCP_INT_SLAVE_MASK, 0);
		int_status &= ~CDNS_MCP_INT_SLAVE_MASK;
		ret = IRQ_WAKE_THREAD;
	}

	cdns_sdw_writel(sdw, CDNS_MCP_INTSTAT, int_status);
	return ret;
}

static irqreturn_t cdns_thread(int irq, void *dev_id)
{
	struct cdns_sdw *sdw = dev_id;
	u32 slave0, slave1;

	dev_info(sdw->dev, "Slave status change\n");
	slave0 = cdns_sdw_readl(sdw, CDNS_MCP_SLAVE_INTSTAT0);
	slave1 = cdns_sdw_readl(sdw, CDNS_MCP_SLAVE_INTSTAT1);
	cdns_update_slave_status(sdw, slave0, slave1);
	cdns_sdw_writel(sdw, CDNS_MCP_SLAVE_INTSTAT0, slave0);
	cdns_sdw_writel(sdw, CDNS_MCP_SLAVE_INTSTAT1, slave1);

	/* clear and unmask slave interrupt now */
	cdns_sdw_writel(sdw, CDNS_MCP_INTSTAT, CDNS_MCP_INT_SLAVE_MASK);
	cdns_sdw_updatel(sdw, CDNS_SDW_INTMASK,
				CDNS_MCP_INT_SLAVE_MASK, CDNS_MCP_INT_SLAVE_MASK);

	return IRQ_HANDLED;
}

/*
 * init routines
 */

static void cdns_enable_interrupt(struct cdns_sdw *sdw)
{
	u32 mask;

	cdns_sdw_writel(sdw, CDNS_MCP_SLAVE_INTMASK0,
				CDNS_MCP_SLAVE_INTMASK0_MASK);
	cdns_sdw_writel(sdw, CDNS_MCP_SLAVE_INTMASK1,
				CDNS_MCP_SLAVE_INTMASK1_MASK);

	/* Enable slave interrupts */
	mask = CDNS_MCP_INT_SLAVE_RSVD | CDNS_MCP_INT_SLAVE_ALERT |
		CDNS_MCP_INT_SLAVE_ATTACH | CDNS_MCP_INT_SLAVE_NATTACH |
		CDNS_MCP_INT_CTRL_CLASH | CDNS_MCP_INT_DATA_CLASH |
		CDNS_MCP_INT_RX_WL | CDNS_MCP_INT_IRQ | CDNS_MCP_INT_DPINT;

	cdns_sdw_writel(sdw, CDNS_SDW_INTMASK, mask);

	return;
}

static int cdns_config_update(struct cdns_sdw *sdw)
{
	volatile u32 config_update;
	int timeout = 10;
	bool config_updated = false;

	/* Bit is self-cleared when configuration gets updated. */
	cdns_sdw_writel(sdw, CDNS_MCP_CONFIG_UPDATE, CDNS_MCP_CONFIG_UPDATE_BIT);

	/* Wait for config update bit to be self cleared */
	do {
		config_update = cdns_sdw_readl(sdw, CDNS_MCP_CONFIG_UPDATE);
		if ((config_update & CDNS_MCP_CONFIG_UPDATE_BIT) == 0) {
			config_updated = true;
			break;
		}
		timeout--;
		/* Wait for 20ms between each try */
		msleep(20);

	} while (timeout != 0);

	if (config_updated ==  false) {
		dev_err(sdw->dev, "Config update timedout\n");
		return -EIO;
	}

	return 0;
}

static int cdns_sdw_init(struct cdns_sdw *sdw, bool first_init)
{
	u32 val;

	/* Set clock divider */
	cdns_sdw_updatel(sdw, CDNS_MCP_CLK_CTRL0, CDNS_DEFAULT_CLK_DIVIDER, CDNS_DEFAULT_CLK_DIVIDER);
	cdns_sdw_updatel(sdw, CDNS_MCP_CLK_CTRL1, CDNS_DEFAULT_CLK_DIVIDER, CDNS_DEFAULT_CLK_DIVIDER);

	/* Set the default frame shape */
	cdns_sdw_writel(sdw, CDNS_MCP_FRAME_SHAPE_INIT, CDNS_DEFAULT_FRAME_SHAPE);

	/* Set SSP interval to default value for both banks */
	cdns_sdw_writel(sdw, CDNS_MCP_SSP_CTRL0, CDNS_DEFAULT_SSP_INTERVAL);
	cdns_sdw_writel(sdw, CDNS_MCP_SSP_CTRL1, CDNS_DEFAULT_SSP_INTERVAL);

	/* Set cmd accept mode */
	cdns_sdw_updatel(sdw, CDNS_MCP_CONTROL, CDNS_MCP_CONTROL_CMD_ACCEPT, CDNS_MCP_CONTROL_CMD_ACCEPT);

	/* configure mcp config */

	/* set cmd retry */
	val = CDNS_MCP_CONFIG_MPREQ_DELAY;

	/* set multi-mode */
	val |= CDNS_MCP_CONFIG_MMASTER;

	/* Disable auto bus release */
	val &= ~CDNS_MCP_CONFIG_BUS_REL;

	/* Disable sniffer mode */
	val &= ~CDNS_MCP_CONFIG_SNIFFER;

	/* Set cmd mode for Tx and Rx cmds */
	val &= ~CDNS_MCP_CONFIG_CMD;

	/* Set operation to normal */
	val &= ~CDNS_MCP_CONFIG_OP;
	val |= CDNS_MCP_CONFIG_OP_NORMAL;

	cdns_sdw_writel(sdw, CDNS_MCP_CONFIG, val);

	/* enable interrupt and configuration */
	cdns_enable_interrupt(sdw);
	cdns_config_update(sdw);

	return 0;
}

static int cdns_sdw_probe(struct platform_device *pdev)
{
	struct cdns_sdw *sdw;
	int ret;

	sdw = devm_kzalloc(&pdev->dev, sizeof(*sdw), GFP_KERNEL);
	if (!sdw)
		return -ENOMEM;

	sdw->instance = pdev->id;
	sdw->res = dev_get_platdata(&pdev->dev);
	sdw->dev = &pdev->dev;
	init_completion(&sdw->tx_complete);

	sdw->bus.acpi_enabled = true;
	sdw->bus.dev = &pdev->dev;
	sdw->bus.link_id = pdev->id;

	platform_set_drvdata(pdev, sdw);

	/* acquire irq */
	ret = request_threaded_irq(sdw->res->irq, cdns_irq,
			cdns_thread, IRQF_SHARED, KBUILD_MODNAME, sdw);
	if (ret < 0) {
		dev_err(sdw->dev, "unable to grab IRQ %d, disabling device\n",
				sdw->res->irq);
		return ret;
	}

	/* enable pm and power up IO */
	pm_runtime_set_autosuspend_delay(&pdev->dev, 3000);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);

	/* init the controller */
	ret = cdns_sdw_init(sdw, true);
	if (ret < 0)
		return ret;

	/* now register the bus */
	sdw_add_bus_master(&sdw->bus);

	/*
	 * Suspending the device after audio suspend delay (3 secs).
	 *
	 * By this time all the slave would have enumerated. Initial clock
	 * freq is 9.6MHz and frame shape is 48x2, so there are 200000
	 * frames in a second, so minimum 600000 frames before device
	 * suspends. Spec says slave should get attached to bus in 4096
	 * error free frames after reset. So this should be enough to make
	 * sure device gets attached to bus.
	 */
	pm_runtime_mark_last_busy(&pdev->dev);
	pm_runtime_put_sync_autosuspend(&pdev->dev);

	return 0;
}

static int cdns_sdw_remove(struct platform_device *pdev)
{
	struct cdns_sdw *sdw;

	sdw = platform_get_drvdata(pdev);

	free_irq(sdw->res->irq, sdw);

	sdw_delete_bus_master(&sdw->bus);

	return 0;
}

/*
 * PM calls
 */

#ifdef CONFIG_PM

static int cdns_sdw_suspend(struct device *dev)
{
	struct cdns_sdw *sdw;
	volatile u32 status;
	unsigned long timeout;
	int ret;

	sdw = dev_get_drvdata(dev);

	/* check suspend status */
	status = cdns_sdw_readl(sdw, CDNS_MCP_STAT);
	if (status & CDNS_MCP_STAT_CLK_STOP) {
		dev_info(dev, "Clock is already stopped\n");
		return 0;
	}

	/* Disable block wakeup */
	cdns_sdw_updatel(sdw, CDNS_MCP_CONTROL, CDNS_MCP_CONTROL_BLOCK_WAKEUP,
					CDNS_MCP_CONTROL_BLOCK_WAKEUP);

	/* prepare slaves for clock stop */
	ret = sdw_bus_prep_clk_stop(&sdw->bus);
	if (ret)
		return ret;

	/* enter clock stop */
	ret = sdw_bus_clk_stop(&sdw->bus);
	if (ret)
		return ret;

	/* wait for clock to be stopped */
	timeout = jiffies + msecs_to_jiffies(100);
	while ((cdns_sdw_readl(sdw, CDNS_MCP_STAT) & CDNS_MCP_STAT_CLK_STOP)
			&& time_before(jiffies, timeout))
		udelay(50);

	status = cdns_sdw_readl(sdw, CDNS_MCP_STAT) & CDNS_MCP_STAT_CLK_STOP;
	if (status) {
		dev_err(dev, "Clock stop failed\n");
		ret = -EBUSY;
	}

	return 0;
}

static int cdns_sdw_resume(struct device *dev)
{
	struct cdns_sdw *sdw;
	volatile u32 status;
	int ret;

	sdw = dev_get_drvdata(dev);
	/* check resume status */
	status = cdns_sdw_readl(sdw, CDNS_MCP_STAT) & CDNS_MCP_STAT_CLK_STOP;
	if (!status) {
		dev_info(dev, "Clock is already running\n");
		return 0;
	}

	ret = cdns_sdw_init(sdw, false);
	if (ret)
		return ret;

	ret = sdw_bus_clk_stop_exit(&sdw->bus);
	if (ret)
		return ret;

	return 0;
}

#endif

static const struct dev_pm_ops cdns_sdw_pm = {
	SET_RUNTIME_PM_OPS(cdns_sdw_suspend, cdns_sdw_resume, NULL)
};

static struct platform_driver cdns_sdw_drv = {
	.probe = cdns_sdw_probe,
	.remove = cdns_sdw_remove,
	.driver = {
		.name = "int-sdw",
		.pm = &cdns_sdw_pm,

	},
};

module_platform_driver(cdns_sdw_drv);

MODULE_ALIAS("platform:int-sdw");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Intel Soundwire driver");
