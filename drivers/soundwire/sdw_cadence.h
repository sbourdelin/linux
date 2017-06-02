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
#ifndef __SDW_CADENCE_H
#define __SDW_CADENCE_H

/* Controller Registers */

#define CDNS_MCP_CONFIG				0x0

#define CDNS_MCP_CONFIG_MCMD_RETRY		GENMASK(27, 24)
#define CDNS_MCP_CONFIG_MPREQ_DELAY		GENMASK(20, 16)
#define CDNS_MCP_CONFIG_MMASTER			BIT(7)
#define CDNS_MCP_CONFIG_BUS_REL			BIT(6)
#define CDNS_MCP_CONFIG_SNIFFER			BIT(5)
#define CDNS_MCP_CONFIG_SSPMOD			BIT(4)
#define CDNS_MCP_CONFIG_CMD			BIT(3)
#define CDNS_MCP_CONFIG_OP			GENMASK(2, 0)
#define CDNS_MCP_CONFIG_OP_NORMAL		0

#define CDNS_MCP_CONTROL			0x4

#define CDNS_MCP_CONTROL_RST_DELAY		GENMASK(10, 8)
#define CDNS_MCP_CONTROL_CMD_RST		BIT(7)
#define CDNS_MCP_CONTROL_SOFT_RST		BIT(6)
#define CDNS_MCP_CONTROL_SW_RST			BIT(5)
#define CDNS_MCP_CONTROL_HW_RST			BIT(4)
#define CDNS_MCP_CONTROL_CLK_PAUSE		BIT(3)
#define CDNS_MCP_CONTROL_CLK_STOP		BIT(2)
#define CDNS_MCP_CONTROL_CMD_ACCEPT		BIT(1)
#define CDNS_MCP_CONTROL_BLOCK_WAKEUP		BIT(0)


#define CDNS_MCP_CMDCTRL			0x8
#define CDNS_MCP_SSPSTAT			0xC
#define CDNS_MCP_FRAME_SHAPE			0x10
#define CDNS_MCP_FRAME_SHAPE_INIT		0x14

#define CDNS_MCP_CONFIG_UPDATE			0x18
#define CDNS_MCP_CONFIG_UPDATE_BIT		BIT(0)

#define CDNS_MCP_PHYCTRL			0x1C
#define CDNS_MCP_SSP_CTRL0			0x20
#define CDNS_MCP_SSP_CTRL1			0x28
#define CDNS_MCP_CLK_CTRL0			0x30
#define CDNS_MCP_CLK_CTRL1			0x38

#define CDNS_MCP_STAT				0x40

#define CDNS_MCP_STAT_ACTIVE_BANK		BIT(20)
#define CDNS_MCP_STAT_CLK_STOP			BIT(16)

#define CDNS_MCP_INTSTAT			0x44
#define CDNS_SDW_INTMASK			0x48

#define CDNS_MCP_INT_IRQ			BIT(31)
#define CDNS_MCP_INT_WAKEUP			BIT(16)
#define CDNS_MCP_INT_SLAVE_RSVD			BIT(15)
#define CDNS_MCP_INT_SLAVE_ALERT		BIT(14)
#define CDNS_MCP_INT_SLAVE_ATTACH		BIT(13)
#define CDNS_MCP_INT_SLAVE_NATTACH		BIT(12)
#define CDNS_MCP_INT_SLAVE_MASK			GENMASK(15, 12)
#define CDNS_MCP_INT_DPINT			BIT(11)
#define CDNS_MCP_INT_CTRL_CLASH			BIT(10)
#define CDNS_MCP_INT_DATA_CLASH			BIT(9)
#define CDNS_MCP_INT_CMD_ERR			BIT(7)
#define CDNS_MCP_INT_RX_WL			BIT(2)
#define CDNS_MCP_INT_TXE			BIT(1)

#define CDNS_MCP_INTSET				0x4C

#define CDNS_SDW_SLAVE_STAT			0x50
#define CDNS_MCP_SLAVE_STAT_MASK		BIT(1, 0)

#define CDNS_MCP_SLAVE_INTSTAT0			0x54
#define CDNS_MCP_SLAVE_INTSTAT1			0x58
#define CDNS_MCP_SLAVE_INTSTAT_NPRESENT		BIT(0)
#define CDNS_MCP_SLAVE_INTSTAT_ATTACHED		BIT(1)
#define CDNS_MCP_SLAVE_INTSTAT_ALERT		BIT(2)
#define CDNS_MCP_SLAVE_INTSTAT_RESERVED		BIT(3)
#define CDNS_MCP_SLAVE_STATUS_BITS		GENMASK(3, 0)
#define CDNS_MCP_SLAVE_STATUS_NUM		4

#define CDNS_MCP_SLAVE_INTMASK0			0x5C
#define CDNS_MCP_SLAVE_INTMASK1			0x60

#define CDNS_MCP_SLAVE_INTMASK0_MASK		GENMASK(30, 0)
#define CDNS_MCP_SLAVE_INTMASK1_MASK		GENMASK(16, 0)

#define CDNS_MCP_PORT_INTSTAT			0x64
#define CDNS_MCP_PDI_STAT			0x6C

#define CDNS_MCP_FIFOLEVEL			0x78
#define CDNS_MCP_FIFOSTAT			0x7C
#define CDNS_MCP_RX_FIFO_AVAIL			GENMASK(5, 0)

#define CDNS_MCP_CMD_BASE			0x80
#define CDNS_MCP_RESP_BASE			0x80
#define CDNS_MCP_CMD_LEN			0x20

#define CDNS_MCP_CMD_SSP_TAG			BIT(31)
#define CDNS_MCP_CMD_COMMAND			GENMASK(30, 28)
#define CDNS_MCP_CMD_DEV_ADDR			GENMASK(27, 24)
#define CDNS_MCP_CMD_REG_ADDR_H			GENMASK(23, 16)
#define CDNS_MCP_CMD_REG_ADDR_L			GENMASK(15, 8)
#define CDNS_MCP_CMD_REG_DATA			GENMASK(7, 0)

#define CDNS_MCP_CMD_READ			2
#define CDNS_MCP_CMD_WRITE			3

#define CDNS_MCP_RESP_RDATA			GENMASK(15, 8)
#define CDNS_MCP_RESP_ACK			BIT(0)
#define CDNS_MCP_RESP_NACK			BIT(1)

#define CDNS_DP_SIZE				128

#define CDNS_DPN_B0_CONFIG(n)			(0x100 + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B0_CH_EN(n)			(0x104 + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B0_SAMPLE_CTRL(n)		(0x108 + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B0_OFFSET_CTRL(n)		(0x10C + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B0_HCTRL(n)			(0x110 + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B0_ASYNC_CTRL(n)		(0x114 + CDNS_DP_SIZE * (n))

#define CDNS_DPN_B1_CONFIG(n)			(0x118 + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B1_CH_EN(n)			(0x11C + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B1_SAMPLE_CTRL(n)		(0x120 + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B1_OFFSET_CTRL(n)		(0x124 + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B1_HCTRL(n)			(0x128 + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B1_ASYNC_CTRL(n)		(0x12C + CDNS_DP_SIZE * (n))

#define CDNS_DPN_CONFIG_BPM			BIT(18)
#define CDNS_DPN_CONFIG_BGC			GENMASK(17, 16)
#define CDNS_DPN_CONFIG_WL			GENMASK(12, 8)
#define CDNS_DPN_CONFIG_PORT_DAT		GENMASK(3, 2)
#define CDNS_DPN_CONFIG_PORT_FLOW		GENMASK(1, 0)

#define CDNS_DPN_SAMPLE_CTRL_SI			GENMASK(15, 0)

#define CDNS_DPN_OFFSET_CTRL_1 			GENMASK(7, 0)
#define CDNS_DPN_OFFSET_CTRL_2 			GENMASK(15, 8)

#define CDNS_DPN_HCTRL_HSTOP			GENMASK(3, 0)
#define CDNS_DPN_HCTRL_HSTART			GENMASK(7, 4)
#define CDNS_DPN_HCTRL_LCTRL			GENMASK(10, 8)

#define CDNS_PORTCTRL				0x130
#define CDNS_PORTCTRL_DIRN			BIT(7)
#define CDNS_PORTCTRL_BANK_INVERT		BIT(8)

#define CDNS_PORT_OFFSET			0x80

#define CDNS_PDI_CONFIG(n)			(0x1100 + (n) * 16)

#define CDNS_PDI_CONFIG_SOFT_RESET		BIT(24)
#define CDNS_PDI_CONFIG_CHANNEL			GENMASK(15, 8)
#define CDNS_PDI_CONFIG_PORT			GENMASK(4, 0)

/* Driver defaults */

#define CDNS_DEFAULT_CLK_DIVIDER	0
#define CDNS_DEFAULT_FRAME_SHAPE	0x30
#define CDNS_DEFAULT_SSP_INTERVAL	0x18
#define CDNS_TX_TIMEOUT			2000

#define CDNS_MAX_PORTS			9

#define CDNS_PCM_PDI_OFFSET		0x2
#define CDNS_PDM_PDI_OFFSET		0x6

/**
 * struct cdns_ports: port instance
 *
 * @idx: port index
 * @allocated: is the port allocated
 * @ch: channel count for the port
 * @direction: data port direction of type enum sdw_data_direction
 * @pdi: pdi for port
 */
struct cdns_ports {
	unsigned int idx;
	bool allocated;
	unsigned int ch;
	enum sdw_data_direction direction;
	struct sdw_cdns_pdi *pdi;
};

/**
 * struct sdw_cdns_streams: stream data structure
 *
 * @num_bd: number bidirectional streams
 * @num_in: number of input streams
 * @num_out: number of output streams
 * @bd: bidirectional streams
 * @in: input streams
 * @out: output streams
 */
struct sdw_cdns_streams {
	unsigned int num_bd;
	unsigned int num_in;
	unsigned int num_out;
	struct sdw_cdns_pdi *bd;
	struct sdw_cdns_pdi *in;
	struct sdw_cdns_pdi *out;
};

/**
 * struct cdns_sdw: cadence driver context
 *
 * @instance: instance number
 * @dev: the device
 * @res: link resources
 * @bus: the SoundWire Bus Instance
 * @response_buf: SoundWire response buffer
 * @tx_complete: tx completion
 * @async: async pointer
 * @ports: Data ports
 * @pcm: PCM streams
 * @pdm: PDM streams
 */
struct cdns_sdw {
	int instance;
	struct device *dev;
	struct sdw_ilink_res *res;
	struct sdw_bus bus;

	u32 response_buf[0x80];
	struct completion tx_complete;
	struct sdw_wait *async;

	struct cdns_ports ports[CDNS_MAX_PORTS];
	struct sdw_cdns_streams pcm;
	struct sdw_cdns_streams pdm;
};

#define bus_to_cdns(_bus) container_of(_bus, struct cdns_sdw, bus)

static inline u32 cdns_sdw_readl(struct cdns_sdw *sdw, int offset)
{
	return readl(sdw->res->registers + offset);
}

static inline void cdns_sdw_writel(struct cdns_sdw *sdw, int offset, u32 value)
{
	writel(value, sdw->res->registers + offset);

	return;
}

static inline void cdns_sdw_updatel(struct cdns_sdw *sdw, int offset, u32 mask, u32 val)
{
	u32 tmp;

	tmp = readl(sdw->res->registers + offset);
	tmp = (tmp & ~mask) | val;
	writel(tmp, sdw->res->registers + offset);

	return;
}

static inline u16 cdns_sdw_readw(struct cdns_sdw *sdw, int offset)
{
	return readw(sdw->res->registers + offset);
}

static inline void cdns_sdw_writew(struct cdns_sdw *sdw, int offset, u16 value)
{
	writew(value, sdw->res->registers + offset);
}

static inline int cdns_sdw_port_readl(struct cdns_sdw *sdw,
					int offset, int port_num)
{
	return cdns_sdw_readl(sdw, offset + port_num * CDNS_DP_SIZE);
}

static inline void cdns_sdw_port_writel(struct cdns_sdw *sdw, int offset,
						int port_num, int value)
{
	return cdns_sdw_writel(sdw, offset + port_num * CDNS_DP_SIZE, value);
}

#endif /* __SDW_CADENCE_H */
