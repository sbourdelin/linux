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

static enum sdw_command_response
_cdns_xfer_msg(struct cdns_sdw *sdw, struct sdw_msg *msg, int cmd,
				int offset, int count, bool async)
{
	u32 base, i, data;
	u16 addr;
	unsigned long time;

	/* program the watermark level */
	cdns_sdw_writel(sdw, CDNS_MCP_FIFOLEVEL, count);

	base = CDNS_MCP_CMD_BASE;
	addr = msg->addr;

	for (i = 0; i < count; i++) {
		data = msg->device << SDW_REG_SHIFT(CDNS_MCP_CMD_DEV_ADDR);
		data |= cmd << SDW_REG_SHIFT(CDNS_MCP_CMD_COMMAND);
		data |= addr++  << SDW_REG_SHIFT(CDNS_MCP_CMD_REG_ADDR_L);
		if (msg->flags == SDW_MSG_FLAG_WRITE)
			data |= msg->buf[i + offset];

		data |= msg->ssp_sync << SDW_REG_SHIFT(CDNS_MCP_CMD_SSP_TAG);

		cdns_sdw_writel(sdw, base, data);
		base += CDNS_MCP_CMD_LEN;
	}

	if (async)
		return 0;

	/* wait for timeout or response */
	time = wait_for_completion_timeout(&sdw->tx_complete,
				msecs_to_jiffies(CDNS_TX_TIMEOUT));
	if (!time) {
		dev_err(sdw->dev, "Msg trf timedout\n");
		msg->len = 0;
		return SDW_CMD_FAILED;
	}

	return cdns_fill_msg_resp(sdw, msg, count, offset);
}

static int cdns_program_scp_addr(struct cdns_sdw *sdw, struct sdw_msg *msg)
{
	u32 data[2], base;
	unsigned long time;
	int nack = 0, no_ack = 0;
	int i;

	/* program RX watermark as 2 for 2 cmds */
	cdns_sdw_writel(sdw, CDNS_MCP_FIFOLEVEL, 2);

	data[0] = msg->device << SDW_REG_SHIFT(CDNS_MCP_CMD_DEV_ADDR);
	data[0] |= 0x3 << SDW_REG_SHIFT(CDNS_MCP_CMD_COMMAND);
	data[1] = data[0];

	data[0] |= SDW_SCP_ADDRPAGE1 << SDW_REG_SHIFT(CDNS_MCP_CMD_REG_ADDR_L);
	data[1] |= SDW_SCP_ADDRPAGE2 << SDW_REG_SHIFT(CDNS_MCP_CMD_REG_ADDR_L);

	data[0] |= msg->addr_page1;
	data[1] |= msg->addr_page2;

	base = CDNS_MCP_CMD_BASE;
	cdns_sdw_writel(sdw, base, data[0]);
	base += CDNS_MCP_CMD_LEN;
	cdns_sdw_writel(sdw, base, data[1]);

	time = wait_for_completion_timeout(&sdw->tx_complete,
				msecs_to_jiffies(CDNS_TX_TIMEOUT));
	if (!time) {
		dev_err(sdw->dev, "SCP Msg trf timedout\n");
		msg->len = 0;
		return -ETIMEDOUT;
	}

	/* check response for the two writes */
	for (i = 0; i < 2; i++) {
		if (!(sdw->response_buf[i] & CDNS_MCP_RESP_ACK)) {
			no_ack = 1;
			dev_err(sdw->dev, "Program SCP Ack not received\n");
			if (sdw->response_buf[i] & CDNS_MCP_RESP_NACK) {
				nack = 1;
				dev_err(sdw->dev, "Program SCP NACK rcvd\n");
			}
		}
	}

	/*
	 * For NACK or NO ack, don't return err if we are in Broadcast mode
	 */
	if (nack && (msg->device != 15)) {
		dev_err(sdw->dev, "SCP_addrpage NACKed for slave %d\n", msg->device);
		return -EREMOTEIO;
	} else if (no_ack && (msg->device != 15)) {
		dev_err(sdw->dev, "SCP_addrpage ignored for slave %d\n", msg->device);
		return -EREMOTEIO;
	}

	return 0;
}

static int cdns_prep_msg(struct cdns_sdw *sdw, struct sdw_msg *msg,
						int page, int *cmd)
{
	int ret;

	if (page) {
		ret = cdns_program_scp_addr(sdw, msg);
		if (ret) {
			msg->len = 0;
			return ret;
		}
	}

	switch (msg->flags) {
	case SDW_MSG_FLAG_READ:
		*cmd = CDNS_MCP_CMD_READ;
		break;

	case SDW_MSG_FLAG_WRITE:
		*cmd = CDNS_MCP_CMD_WRITE;
		break;

	default:
		dev_err(sdw->dev, "Invalid msg cmd: %d\n", msg->flags);
		return -EINVAL;
	}

	return 0;
}

static enum sdw_command_response
cdns_xfer_msg(struct sdw_bus *bus, struct sdw_msg *msg, int page)
{
	struct cdns_sdw *sdw = bus_to_cdns(bus);
	int cmd = 0, ret, i;

	ret = cdns_prep_msg(sdw, msg, page, &cmd);
	if (ret)
		return SDW_CMD_FAILED;

	for (i = 0; i < msg->len / CDNS_MCP_CMD_LEN; i++) {
		ret = _cdns_xfer_msg(sdw, msg, cmd, i * CDNS_MCP_CMD_LEN,
				CDNS_MCP_CMD_LEN, false);
		if (ret < 0)
			goto exit;
	}

	if (!(msg->len % CDNS_MCP_CMD_LEN))
		goto exit;

	ret = _cdns_xfer_msg(sdw, msg, cmd, i * CDNS_MCP_CMD_LEN,
			msg->len % CDNS_MCP_CMD_LEN, false);

exit:
	return ret;
}

static enum sdw_command_response
cdns_xfer_msg_async(struct sdw_bus *bus, struct sdw_msg *msg,
				int page, struct sdw_wait *wait)
{
	struct cdns_sdw *sdw = bus_to_cdns(bus);
	int cmd = 0, ret;

	/* for async only 1 message is supported */
	if (msg->len > 1)
		return -ENOTSUPP;

	ret = cdns_prep_msg(sdw, msg, page, &cmd);
	if (ret)
		return SDW_CMD_FAILED;

	sdw->async = wait;
	sdw->async->length = msg->len;

	/* don't wait for reply as caller would do so */
	return _cdns_xfer_msg(sdw, msg, cmd, 0, msg->len, true);
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

/* TODO: optimize number of arguments for below API */
static int cdns_sdw_allocate_pdi(struct cdns_sdw *sdw, struct sdw_ishim *shim,
			struct sdw_cdns_pdi **stream, u32 start, u32 num,
			u32 pdi_offset, bool pcm)
{
	const struct sdw_ishim_ops *ops = sdw->res->ops;
	struct sdw_cdns_pdi *pdi;
	int i;

	if (!num)
		return 0;

	pdi = devm_kcalloc(sdw->dev, num, sizeof(*pdi), GFP_KERNEL);
	if (!pdi)
		return -ENOMEM;

	for (i = start; i < num; i++)  {
		pdi[i].ch_count = ops->pdi_ch_cap(shim, sdw->bus.link_id, i, pcm);
		pdi[i].pdi_num = i + pdi_offset;
		pdi[i].assigned = false;
	}

	*stream = pdi;
	return 0;

}
static int cdns_sdw_pdi_init(struct cdns_sdw *sdw)
{
	struct sdw_ishim *shim = sdw->res->shim;
	const struct sdw_ishim_ops *ops = sdw->res->ops;
	struct sdw_cdns_stream_config config;
	struct sdw_cdns_streams *stream;
	int i;

	/* get the shim configuration */
	ops->pdi_init(shim, sdw->bus.link_id, &config);

	/* copy the info */
	sdw->pcm.num_bd = config.pcm_bd;
	sdw->pcm.num_in = config.pcm_in;
	sdw->pcm.num_out = config.pcm_out;
	sdw->pdm.num_bd = config.pdm_bd;
	sdw->pdm.num_in = config.pdm_in;
	sdw->pdm.num_out = config.pdm_out;

	for (i = 0; i < CDNS_MAX_PORTS; i++) {
		sdw->ports[i].allocated = false;
		sdw->ports[i].idx = i;
	}

	stream = &sdw->pcm;
	/* first two BDs are reserved for bulk transfers, allocate them */
	cdns_sdw_allocate_pdi(sdw, shim, &stream->bd,
			CDNS_PCM_PDI_OFFSET, sdw->pcm.num_bd, 0, 1);
	stream->num_bd -= CDNS_PCM_PDI_OFFSET;

	cdns_sdw_allocate_pdi(sdw, shim, &stream->in, 0,
					stream->num_in, 0, 1);
	cdns_sdw_allocate_pdi(sdw, shim, &stream->out, 0,
					stream->num_out, 0, 1);

	stream = &sdw->pdm;
	/* now allocate for PDMs */
	cdns_sdw_allocate_pdi(sdw, shim, &stream->bd, 0,
				stream->num_bd, CDNS_PDM_PDI_OFFSET, 0);
	cdns_sdw_allocate_pdi(sdw, shim, &stream->in, 0,
				stream->num_in, CDNS_PDM_PDI_OFFSET, 0);
	cdns_sdw_allocate_pdi(sdw, shim, &stream->out, 0,
				stream->num_out, CDNS_PDM_PDI_OFFSET, 0);

	return 0;
}

static int cdns_sdw_init(struct cdns_sdw *sdw, bool first_init)
{
	u32 val;

	if (sdw->res && sdw->res->shim) {
		struct sdw_ishim *shim = sdw->res->shim;
		const struct sdw_ishim_ops *ops = sdw->res->ops;

		/* we need to power up and init shim first */
		ops->link_power_up(shim, sdw->bus.link_id);
		ops->init(shim, sdw->bus.link_id);

		/* now configure the shim by setting SyncPRD and SyncPU */
		ops->sync(shim, sdw->bus.link_id, SDW_ISHIM_SYNCPRD);
		ops->sync(shim, sdw->bus.link_id, SDW_ISHIM_CMDSYNC);
	}

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

	/* Init the PDI */
	if (first_init && sdw->res && sdw->res->shim)
		cdns_sdw_pdi_init(sdw);

	/* enable interrupt and configuration */
	cdns_enable_interrupt(sdw);
	cdns_config_update(sdw);

	return 0;
}

static int cdns_ssp_interval(struct sdw_bus *bus, unsigned int ssp_interval,
				unsigned int bank)
{
	struct cdns_sdw *sdw = bus_to_cdns(bus);

	if (bank)
		cdns_sdw_writel(sdw, CDNS_MCP_SSP_CTRL1, ssp_interval);
	else
		cdns_sdw_writel(sdw, CDNS_MCP_SSP_CTRL0, ssp_interval);

	return 0;
}

static int cdns_bus_conf(struct sdw_bus *bus, struct sdw_bus_conf *conf)
{
	struct cdns_sdw *sdw = bus_to_cdns(bus);
	int mcp_clkctrl_off, mcp_clkctrl;

	int divider = bus->prop.max_freq / conf->clk_freq;

	if (conf->bank)
		mcp_clkctrl_off = CDNS_MCP_CLK_CTRL1;
	else
		mcp_clkctrl_off = CDNS_MCP_CLK_CTRL0;

	mcp_clkctrl = cdns_sdw_readl(sdw, mcp_clkctrl_off);
	mcp_clkctrl |= divider;

	cdns_sdw_writel(sdw, mcp_clkctrl_off, mcp_clkctrl);

	return 0;
}

static int cdns_pre_bank_switch(struct sdw_bus *bus)
{
	struct cdns_sdw *sdw = bus_to_cdns(bus);
	const struct sdw_ishim_ops *ops = sdw->res->ops;
	struct sdw_ishim *shim = sdw->res->shim;

	if ((bus->link_sync_mask) && (ops->sync))
		return ops->sync(shim, sdw->bus.link_id, SDW_ISHIM_CMDSYNC);

	return 0;
}

static int cdns_post_bank_switch(struct sdw_bus *bus)
{
	struct cdns_sdw *sdw = bus_to_cdns(bus);
	const struct sdw_ishim_ops *ops = sdw->res->ops;
	struct sdw_ishim *shim = sdw->res->shim;

	if (ops->sync)
		return ops->sync(shim, sdw->bus.link_id, SDW_ISHIM_SYNCGO);

	return 0;
}

static int cdns_port_params(struct sdw_bus *bus,
			struct sdw_port_params *p_params, unsigned int bank)
{
	struct cdns_sdw *sdw = bus_to_cdns(bus);
	int dpn_config = 0, dpn_config_off;

	if (bank)
		dpn_config_off = CDNS_DPN_B1_CONFIG(p_params->num);
	else
		dpn_config_off = CDNS_DPN_B0_CONFIG(p_params->num);

	dpn_config = cdns_sdw_readl(sdw, dpn_config_off);

	dpn_config |= ((p_params->bps - 1) << SDW_REG_SHIFT(CDNS_DPN_CONFIG_WL));
	dpn_config |= (p_params->flow_mode << SDW_REG_SHIFT(CDNS_DPN_CONFIG_PORT_FLOW));
	dpn_config |= (p_params->data_mode << SDW_REG_SHIFT(CDNS_DPN_CONFIG_PORT_DAT));

	cdns_sdw_writel(sdw, dpn_config_off, dpn_config);

	return 0;
}

static int cdns_transport_params(struct sdw_bus *bus,
			struct sdw_transport_params *t_params,
			unsigned int bank)
{
	struct cdns_sdw *sdw = bus_to_cdns(bus);
	int dpn_config = 0, dpn_config_off;
	int dpn_samplectrl_off;
	int dpn_offsetctrl = 0, dpn_offsetctrl_off;
	int dpn_hctrl = 0, dpn_hctrl_off;
	int num = t_params->port_num;

	if (bank) {
		dpn_config_off = CDNS_DPN_B1_CONFIG(num);
		dpn_samplectrl_off = CDNS_DPN_B1_SAMPLE_CTRL(num);
		dpn_hctrl_off = CDNS_DPN_B1_HCTRL(num);
		dpn_offsetctrl_off = CDNS_DPN_B1_OFFSET_CTRL(num);
	} else {
		dpn_config_off = CDNS_DPN_B0_CONFIG(num);
		dpn_samplectrl_off = CDNS_DPN_B0_SAMPLE_CTRL(num);
		dpn_hctrl_off = CDNS_DPN_B0_HCTRL(num);
		dpn_offsetctrl_off = CDNS_DPN_B0_OFFSET_CTRL(num);
	}

	dpn_config = cdns_sdw_readl(sdw, dpn_config_off);

	dpn_config |= (t_params->blk_grp_ctrl << SDW_REG_SHIFT(CDNS_DPN_CONFIG_BGC));
	dpn_config |= (t_params->blk_pkg_mode << SDW_REG_SHIFT(CDNS_DPN_CONFIG_BPM));

	cdns_sdw_writel(sdw, dpn_config_off, dpn_config);

	dpn_offsetctrl |= (t_params->offset1 << SDW_REG_SHIFT(CDNS_DPN_OFFSET_CTRL_1));
	dpn_offsetctrl |= (t_params->offset2 << SDW_REG_SHIFT(CDNS_DPN_OFFSET_CTRL_2));

	cdns_sdw_writel(sdw, dpn_offsetctrl_off,  dpn_offsetctrl);

	dpn_hctrl |= (t_params->hstart << SDW_REG_SHIFT(CDNS_DPN_HCTRL_HSTART));
	dpn_hctrl |= (t_params->hstop << SDW_REG_SHIFT(CDNS_DPN_HCTRL_HSTOP));
	dpn_hctrl |= (t_params->lane_ctrl << SDW_REG_SHIFT(CDNS_DPN_HCTRL_LCTRL));

	cdns_sdw_writel(sdw, dpn_hctrl_off, dpn_hctrl);

	cdns_sdw_writel(sdw, dpn_samplectrl_off,
					(t_params->sample_interval - 1));

	return 0;
}

static int cdns_port_enable(struct sdw_bus *bus,
			struct sdw_enable_ch *enable_ch, unsigned int bank)
{
	struct cdns_sdw *sdw = bus_to_cdns(bus);
	int dpn_chnen_off, ch_mask;

	if (bank)
		dpn_chnen_off = CDNS_DPN_B1_CH_EN(enable_ch->num);
	else
		dpn_chnen_off = CDNS_DPN_B0_CH_EN(enable_ch->num);

	ch_mask = enable_ch->ch_mask * enable_ch->enable;

	cdns_sdw_writel(sdw, dpn_chnen_off, ch_mask);

	return 0;
}

static const struct sdw_master_ops cdns_ops = {
	.read_prop = sdw_master_read_prop,
	.xfer_msg = cdns_xfer_msg,
	.xfer_msg_async = cdns_xfer_msg_async,
	.set_ssp_interval = cdns_ssp_interval,
	.set_bus_conf = cdns_bus_conf,
	.pre_bank_switch = cdns_pre_bank_switch,
	.post_bank_switch = cdns_post_bank_switch,
};

static const struct sdw_master_port_ops cdns_port_ops = {
	.dpn_set_port_params = cdns_port_params,
	.dpn_set_port_transport_params = cdns_transport_params,
	.dpn_port_prep = NULL,
	.dpn_port_enable_ch = cdns_port_enable,
};

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
	sdw->bus.ops = &cdns_ops;
	sdw->bus.port_ops = &cdns_port_ops;

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
	struct sdw_ishim *shim;
	const struct sdw_ishim_ops *ops;
	int ret;

	sdw = dev_get_drvdata(dev);
	shim = sdw->res->shim;
	ops = sdw->res->ops;

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

	if (shim) {
		/* invoke shim for pd */
		ops->link_power_down(shim, sdw->bus.link_id);

		/* invoke shim for wake enable */
		ops->wake(shim, sdw->bus.link_id, true);
	}

	return 0;
}

static int cdns_sdw_resume(struct device *dev)
{
	struct cdns_sdw *sdw;
	struct sdw_ishim *shim;
	const struct sdw_ishim_ops *ops;
	volatile u32 status;
	int ret;

	sdw = dev_get_drvdata(dev);
	shim = sdw->res->shim;
	ops = sdw->res->ops;

	/* check resume status */
	status = cdns_sdw_readl(sdw, CDNS_MCP_STAT) & CDNS_MCP_STAT_CLK_STOP;
	if (!status) {
		dev_info(dev, "Clock is already running\n");
		return 0;
	}

	/* invoke shim for wake disable */
	if (shim)
		ops->wake(shim, sdw->bus.link_id, false);

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
