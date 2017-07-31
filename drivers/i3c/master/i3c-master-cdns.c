/*
 * Copyright (C) 2017 Cadence Design Systems Inc.
 *
 * Author: Boris Brezillon <boris.brezillon@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/clk.h>
#include <linux/i3c/master.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define DEV_ID				0x0
#define DEV_ID_VID(id)			(((id) & GENMASK(31, 20)) >> 20)
#define DEV_ID_PID(id)			(((id) & GENMASK(19, 8)) >> 8)
#define DEV_ID_REV_MAJOR(id)		(((id) & GENMASK(7, 4)) >> 4)
#define DEV_ID_REV_MINOR(id)		((id) & GENMASK(3, 0))

#define CONF_STATUS			0x4
#define CONF_STATUS_HAS_FIFOS		BIT(26)
#define CONF_STATUS_GPO_NUM(s)		(((s) & GENMASK(25, 18)) >> 18)
#define CONF_STATUS_GPI_NUM(s)		(((s) & GENMASK(17, 10)) >> 10)
#define CONF_STATUS_DEVS_NUM(s)		(((s) & GENMASK(6, 3)) >> 3)
#define DEV_TYPE_MAIN_MASTER		0
#define DEV_TYPE_SECONDARY_MASTER	1
#define DEV_TYPE_SLAVE			2
#define CONF_STATUS_DEV_TYPE(s)		((s) & GENMASK(2, 0))

#define DEVS_CTRL			0x8
#define DEVS_CTRL_DEV_CLR_ALL		GENMASK(31, 16)
#define DEVS_CTRL_DEV_CLR(dev)		BIT(16 + (dev))
#define DEVS_CTRL_DEV_ACTIVE(dev)	BIT(dev)

#define CTRL				0x10
#define CTRL_DEV_EN			BIT(31)
#define CTRL_HALT_EN			BIT(30)
#define CTRL_HJ_DISEC			BIT(8)
#define CTRL_MST_ACK			BIT(7)
#define CTRL_HJ_ACK			BIT(6)
#define CTRL_HJ_INIT			BIT(5)
#define CTRL_MST_INIT			BIT(4)
#define CTRL_AHDR_OPT			BIT(3)
#define CTRL_PURE_BUS_MODE		0
#define CTRL_MIXED_FAST_BUS_MODE	2
#define CTRL_MIXED_SLOW_BUS_MODE	3
#define CTRL_BUS_MODE_MASK		GENMASK(1, 0)

#define PRESCL_CTRL0			0x14
#define PRESCL_CTRL0_I2C(x)		((x) << 16)
#define PRESCL_CTRL0_I3C(x)		(x)
#define PRESCL_CTRL0_MAX		GENMASK(15, 0)

#define PRESCL_CTRL1			0x18
#define PRESCL_CTRL1_PP_LOW(x)		((x) << 8)
#define PRESCL_CTRL1_OD_LOW(x)		(x)

#define MST_IER				0x20
#define MST_IDR				0x24
#define MST_IMR				0x28
#define MST_ICR				0x2c
#define MST_ISR				0x30
#define MST_INT_RX_THR			BIT(24)
#define MST_INT_TX_THR			BIT(23)
#define MST_INT_IBI_THR			BIT(22)
#define MST_INT_CMD_THR			BIT(21)
#define MST_INT_RX_UNF			BIT(20)
#define MST_INT_TX_OVF			BIT(19)
#define MST_INT_IBI_UNF			BIT(18)
#define MST_INT_CMD_OVF			BIT(17)
#define MST_INT_CMD_EMPTY		BIT(16)
#define MST_INT_MR_DONE			BIT(11)
#define MST_INT_IBI_FAIL		BIT(10)
#define MST_INT_SDR_FAIL		BIT(9)
#define MST_INT_DDR_FAIL		BIT(8)
#define MST_INT_HJ_REQ			BIT(7)
#define MST_INT_MR_REQ			BIT(6)
#define MST_INT_IBI_REQ			BIT(5)
#define MST_INT_BUS_DISCR		BIT(4)
#define MST_INT_INVALID_DA		BIT(3)
#define MST_INT_RD_ABORT		BIT(2)
#define MST_INT_NACK			BIT(1)
#define MST_INT_COMP			BIT(0)

#define MST_STATUS0			0x34
#define MST_STATUS0_IDLE		BIT(31)
#define MST_STATUS0_HALTED		BIT(30)
#define MST_STATUS0_MASTER_MODE		BIT(29)
#define MST_STATUS0_IMM_COMP		BIT(28)
#define MST_STATUS0_DDR_ERR_ID(s)	((s) & GENMASK(27, 25) >> 25)
#define MST_STATUS0_DAA_COMP		BIT(24)
#define MST_STATUS0_IBI_FIFO_FULL	BIT(23)
#define MST_STATUS0_RX_FIFO_FULL	BIT(22)
#define MST_STATUS0_XFER_BYTES(s)	((s) & GENMASK(21, 10) >> 10)
#define MST_STATUS0_DEV_ADDR(s)		((s) & GENMASK(9, 0))

#define SIR_STATUS			0x3c
#define SIR_STATUS_DEV(d)		BIT(d)

#define SLV_IER				0x40
#define SLV_IDR				0x44
#define SLV_IMR				0x48
#define SLV_ICR				0x4c
#define SLV_ISR				0x50
#define SLV_INT_TM			BIT(20)
#define SLV_INT_ERROR			BIT(19)
#define SLV_INT_EVENT_UP		BIT(18)
#define SLV_INT_HJ_DONE			BIT(17)
#define SLV_INT_MR_DONE			BIT(16)
#define SLV_INT_SDR_FAIL		BIT(14)
#define SLV_INT_DDR_FAIL		BIT(13)
#define SLV_INT_M_RD_ABORT		BIT(12)
#define SLV_INT_DDR_RX_THR		BIT(11)
#define SLV_INT_DDR_TX_THR		BIT(10)
#define SLV_INT_SDR_RX_THR		BIT(9)
#define SLV_INT_SDR_TX_THR		BIT(8)
#define SLV_INT_DDR_RX_UNF		BIT(7)
#define SLV_INT_DDR_TX_OVF		BIT(6)
#define SLV_INT_SDR_RX_UNF		BIT(5)
#define SLV_INT_SDR_TX_OVF		BIT(4)
#define SLV_INT_DDR_RD_COMP		BIT(3)
#define SLV_INT_DDR_WR_COMP		BIT(2)
#define SLV_INT_SDR_RD_COMP		BIT(1)
#define SLV_INT_SDR_WR_COMP		BIT(0)

#define SLV_STATUS0			0x54
#define SLV_STATUS0_REG_ADDR(s)		(((s) & GENMASK(23, 16)) >> 16)
#define SLV_STATUS0_XFRD_BYTES(s)	((s) & GENMASK(15, 0))

#define SLV_STATUS1			0x58
#define SLV_STATUS1_AS(s)		(((s) & GENMASK(21, 20)) >> 20)
#define SLV_STATUS1_VEN_TM		BIT(19)
#define SLV_STATUS1_HJ_DIS		BIT(18)
#define SLV_STATUS1_MR_DIS		BIT(17)
#define SLV_STATUS1_PROT_ERR		BIT(16)
#define SLV_STATUS1_DDR_RX_FULL		BIT(7)
#define SLV_STATUS1_DDR_TX_FULL		BIT(6)
#define SLV_STATUS1_DDR_RX_EMPTY	BIT(5)
#define SLV_STATUS1_DDR_TX_EMPTY	BIT(4)
#define SLV_STATUS1_SDR_RX_FULL		BIT(3)
#define SLV_STATUS1_SDR_TX_FULL		BIT(2)
#define SLV_STATUS1_SDR_RX_EMPTY	BIT(1)
#define SLV_STATUS1_SDR_TX_EMPTY	BIT(0)

#define CMD0_FIFO			0x60
#define CMD0_FIFO_IS_DDR		BIT(31)
#define CMD0_FIFO_IS_CCC		BIT(30)
#define CMD0_FIFO_BCH			BIT(29)
#define XMIT_BURST_STATIC_SUBADDR	0
#define XMIT_SINGLE_INC_SUBADDR		1
#define XMIT_SINGLE_STATIC_SUBADDR	2
#define XMIT_BURST_WITHOUT_SUBADDR	3
#define CMD0_FIFO_PRIV_XMIT_MODE(m)	((m) << 27)
#define CMD0_FIFO_SBCA			BIT(26)
#define CMD0_FIFO_RSBC			BIT(25)
#define CMD0_FIFO_IS_10B		BIT(24)
#define CMD0_FIFO_PL_LEN(l)		((l) << 12)
#define CMD0_FIFO_PL_LEN_MAX		4095
#define CMD0_FIFO_DEV_ADDR(a)		((a) << 1)
#define CMD0_FIFO_RNW			BIT(0)

#define CMD1_FIFO			0x64
#define CMD1_FIFO_CSRADDR(a)		(a)
#define CMD1_FIFO_CCC(id)		(id)

#define TX_FIFO				0x68

#define IMD_CMD0			0x70
#define IMD_CMD0_PL_LEN(l)		((l) << 12)
#define IMD_CMD0_DEV_ADDR(a)		((a) << 1)
#define IMD_CMD0_RNW			BIT(0)

#define IMD_CMD1			0x74
#define IMD_CMD1_CCC(id)		(id)

#define IMD_DATA			0x78
#define RX_FIFO				0x80
#define IBI_DATA_FIFO			0x84
#define SLV_DDR_TX_FIFO			0x88
#define SLV_DDR_RX_FIFO			0x8c

#define CMD_IBI_THR_CTRL		0x90
#define IBI_THR(t)			((t) << 8)
#define CMD_THR(t)			(t)

#define TX_RX_THR_CTRL			0x94
#define RX_THR(t)			((t) << 16)
#define TX_THR(t)			(t)

#define SLV_DDR_TX_RX_THR_CTRL		0x98
#define SLV_DDR_RX_THR(t)		((t) << 16)
#define SLV_DDR_TX_THR(t)		(t)

#define FLUSH_CTRL			0x9c
#define FLUSH_SLV_DDR_RX_FIFO		BIT(22)
#define FLUSH_SLV_DDR_TX_FIFO		BIT(21)
#define FLUSH_IMM_FIFO			BIT(20)
#define FLUSH_IBI_FIFO			BIT(19)
#define FLUSH_RX_FIFO			BIT(18)
#define FLUSH_TX_FIFO			BIT(17)
#define FLUSH_CMD_FIFO			BIT(16)

#define DEV_ID_RR0(d)			(0xa0 + ((d) * 0x10))
#define DEV_ID_RR0_LVR_EXT_ADDR		BIT(11)
#define DEV_ID_RR0_HDR_CAP		BIT(10)
#define DEV_ID_RR0_IS_I3C		BIT(9)
#define DEV_ID_RR0_SET_DEV_ADDR(a)	(((a) & GENMASK(6, 0)) |	\
					 (((a) & GENMASK(9, 7)) << 6))
#define DEV_ID_RR0_GET_DEV_ADDR(x)	((((x) >> 1) & GENMASK(6, 0)) |	\
					 (((x) >> 6) & GENMASK(9, 7)))

#define DEV_ID_RR1(d)			(0xa4 + ((d) * 0x10))
#define DEV_ID_RR1_PID_MSB(pid)		(pid)

#define DEV_ID_RR2(d)			(0xa8 + ((d) * 0x10))
#define DEV_ID_RR2_PID_LSB(pid)		((pid) << 16)
#define DEV_ID_RR2_BCR(bcr)		((bcr) << 8)
#define DEV_ID_RR2_DCR(dcr)		(dcr)
#define DEV_ID_RR2_LVR(lvr)		(lvr)

#define SIR_MAP(x)			(0x160 + ((x) * 4))
#define SIR_MAP_DEV_REG(d)		SIR_MAP((d) / 2)
#define SIR_MAP_DEV_SHIFT(d, fs)	((fs) + (((d) % 2) ? 16 : 0))
#define SIR_MAP_DEV_MASK(d)		(GENMASK(15, 0) << (((d) % 2) ? 16 : 0))
#define DEV_ROLE_SLAVE			0
#define DEV_ROLE_MASTER			1
#define SIR_MAP_DEV_ROLE(d, role)	((role) << SIR_MAP_DEV_SHIFT(d, 14))
#define SIR_MAP_DEV_SLOW(d)		BIT(SIR_MAP_DEV_SHIFT(d, 13))
#define SIR_MAP_DEV_PL(d, l)		((l) << SIR_MAP_DEV_SHIFT(d, 8))
#define SIR_MAP_PL_MAX			GENMASK(4, 0)
#define SIR_MAP_DEV_DA(d, a)		((a) << SIR_MAP_DEV_SHIFT(d, 1))
#define SIR_MAP_DEV_ACK_RESP(d)		BIT(SIR_MAP_DEV_SHIFT(d, 0))

#define GPIR_WORD(x)			(0x180 + ((x) * 4))
#define GPI_REG(val, id)		\
	(((val) >> (((id) % 4) * 8)) & GENMASK(7, 0))

#define GPOR_WORD(x)			(0x200 + ((x) * 4))
#define GPO_REG(val, id)		\
	(((val) >> (((id) % 4) * 8)) & GENMASK(7, 0))

struct cdns_i3c_cmd {
	struct list_head node;
	u32 cmd0;
	u32 cmd1;
	struct {
		void *in;
		const void *out;
	} data;
	u32 dataptr;
	u32 datalen;
	struct completion *comp;
};

struct cdns_i3c_master_caps {
	u32 cmdfifodepth;
	u32 txfifodepth;
	u32 rxfifodepth;
};

struct cdns_i3c_master {
	struct i3c_master_controller base;
	struct mutex lock;
	unsigned long free_dev_slots;
	void __iomem *regs;
	struct clk *sysclk;
	struct clk *pclk;
	struct completion comp;
	struct cdns_i3c_master_caps caps;
};

static inline struct cdns_i3c_master *
to_cdns_i3c_master(struct i3c_master_controller *master)
{
	return container_of(master, struct cdns_i3c_master, base);
}

static void cdns_i3c_master_wr_to_tx_fifo(struct cdns_i3c_master *master,
					  const u8 *bytes, int nbytes)
{
	int i, j;

	for (i = 0; i < nbytes; i += 4) {
		u32 data = 0;

		for (j = 0; j < 4 && (i + j) < nbytes; j++)
			data |= (u32)bytes[i + j] << (j * 8);

		writel(data, master->regs + TX_FIFO);
	}
}

static void cdns_i3c_master_drain_rx_fifo(struct cdns_i3c_master *master)
{
	int i;

	for (i = 0; i < master->caps.rxfifodepth; i++) {
		readl(master->regs + RX_FIFO);
		if (readl(master->regs + MST_ISR) & MST_INT_RX_UNF) {
			writel(MST_INT_RX_UNF, master->regs + MST_ICR);
			break;
		}
	}
}

static void cdns_i3c_master_rd_from_rx_fifo(struct cdns_i3c_master *master,
					    u8 *bytes, int nbytes)
{
	u32 status0;
	int i, j;

	status0 = readl(master->regs + MST_STATUS0);

	if (nbytes > MST_STATUS0_XFER_BYTES(status0))
		nbytes = MST_STATUS0_XFER_BYTES(status0);

	for (i = 0; i < nbytes; i += 4) {
		u32 data;

		data = readl(master->regs + RX_FIFO);

		for (j = 0; j < 4 && (i + j) < nbytes; j++)
			bytes[i + j] = data >> (j * 8);
	}
}

static bool cdns_i3c_master_supports_ccc_cmd(struct i3c_master_controller *m,
					     const struct i3c_ccc_cmd *cmd)
{
	if (cmd->ndests > 1)
		return false;

	switch (cmd->id) {
	case I3C_CCC_ENEC(true):
	case I3C_CCC_ENEC(false):
	case I3C_CCC_DISEC(true):
	case I3C_CCC_DISEC(false):
	case I3C_CCC_ENTAS(0, true):
	case I3C_CCC_ENTAS(0, false):
	case I3C_CCC_RSTDAA(true):
	case I3C_CCC_RSTDAA(false):
	case I3C_CCC_ENTDAA:
	case I3C_CCC_SETMWL(true):
	case I3C_CCC_SETMWL(false):
	case I3C_CCC_SETMRL(true):
	case I3C_CCC_SETMRL(false):
	case I3C_CCC_DEFSLVS:
	case I3C_CCC_ENTHDR(0):
	case I3C_CCC_SETDASA:
	case I3C_CCC_SETNEWDA:
	case I3C_CCC_GETMWL:
	case I3C_CCC_GETMRL:
	case I3C_CCC_GETPID:
	case I3C_CCC_GETBCR:
	case I3C_CCC_GETDCR:
	case I3C_CCC_GETSTATUS:
	case I3C_CCC_GETACCMST:
	case I3C_CCC_GETMXDS:
	case I3C_CCC_GETHDRCAP:
		return true;
	default:
		break;
	}

	return false;
}

static void cdns_i3c_master_init_irqs(struct cdns_i3c_master *master,
				      u32 irqs)
{
	writel(irqs, master->regs + MST_ICR);
	writel(0xffffffff, master->regs + MST_ICR);
	reinit_completion(&master->comp);
}

static u32 cdns_i3c_master_wait_for_irqs(struct cdns_i3c_master *master,
					 u32 irqs)
{
	u32 ret;

	writel(irqs, master->regs + MST_IER);
	wait_for_completion_timeout(&master->comp, msecs_to_jiffies(1000));
	writel(irqs, master->regs + MST_IDR);

	ret = readl(master->regs + MST_ISR) & irqs;

	return ret;
}

static int cdns_i3c_master_send_ccc_cmd(struct i3c_master_controller *m,
					struct i3c_ccc_cmd *cmd)
{
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	u32 cmd0, isr, irqs;

	mutex_lock(&master->lock);
	cmd0 = CMD0_FIFO_IS_CCC | CMD0_FIFO_PL_LEN(cmd->dests[0].payload.len);
	if (cmd->id & I3C_CCC_DIRECT)
		cmd0 |= CMD0_FIFO_DEV_ADDR(cmd->dests[0].addr);

	if (cmd->rnw)
		cmd0 |= CMD0_FIFO_RNW;

	if (!cmd->rnw)
		cdns_i3c_master_wr_to_tx_fifo(master,
					      cmd->dests[0].payload.data,
					      cmd->dests[0].payload.len);

	irqs = MST_INT_COMP | (cmd->id != I3C_CCC_ENTDAA ? MST_INT_NACK : 0);
	cdns_i3c_master_init_irqs(master, irqs);
	writel(CMD1_FIFO_CCC(cmd->id), master->regs + CMD1_FIFO);
	writel(cmd0, master->regs + CMD0_FIFO);
	isr = cdns_i3c_master_wait_for_irqs(master, irqs);

	if (cmd->rnw) {
		int nbytes = cmd->dests[0].payload.len;
		u32 status0 = readl(master->regs + MST_STATUS0);

		if (nbytes > MST_STATUS0_XFER_BYTES(status0))
			nbytes = MST_STATUS0_XFER_BYTES(status0);

		cdns_i3c_master_rd_from_rx_fifo(master,
						cmd->dests[0].payload.data,
						nbytes);
		cmd->dests[0].payload.len = nbytes;
	}
	mutex_unlock(&master->lock);

	/*
	 * MST_INT_NACK is not an error when doing DAA, it just means "no i3c
	 * devices on the bus".
	 */
	if (isr & MST_INT_NACK)
		return -EIO;
	else if (!(isr & MST_INT_COMP))
		return -ETIMEDOUT;

	return 0;
}

static int cdns_i3c_master_priv_xfers(struct i3c_master_controller *m,
				      const struct i3c_priv_xfer *xfers,
				      int nxfers)
{
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	int tnxfers = 0, tntx = 0, tnrx = 0, j = 0, i, ret = 0;

	for (i = 0; i < nxfers; i++) {
		if (xfers[i].len > CMD0_FIFO_PL_LEN_MAX)
			return -ENOTSUPP;
	}

	if (!nxfers)
		return 0;

	/*
	 * First make sure that all transactions (block of transfers separated
	 * by a STOP marker) fit in the FIFOs.
	 */
	for (i = 0; i < nxfers; i++) {
		tnxfers++;

		if (xfers[i].flags & I3C_PRIV_XFER_READ)
			tnrx += DIV_ROUND_UP(xfers[i].len, 4);
		else
			tntx += DIV_ROUND_UP(xfers[i].len, 4);

		if (!(xfers[i].flags & I3C_PRIV_XFER_STOP))
			continue;

		if (tnxfers > master->caps.cmdfifodepth ||
		    tnrx > master->caps.rxfifodepth ||
		    tntx > master->caps.txfifodepth)
			return -ENOTSUPP;

		tnxfers = 0;
		tntx = 0;
		tnrx = 0;
	}

	mutex_lock(&master->lock);

	cdns_i3c_master_init_irqs(master, MST_INT_NACK | MST_INT_COMP);

	/*
	 * FIXME: The IP does not support stalling the output message queue
	 * while we are queuing I3C commands, and we have no way to tell the
	 * I3C master whether we want a Repeated Start (Sr) or a Stop (S)
	 * between two transfers. Instead, the engine decides by itself when Sr
	 * should be used based on the next command in the queue.
	 * The problem is, we are not guaranteed to queue the second message
	 * before the master has finished transmitting the first one, and the
	 * engine might see an empty FIFO when it tries to figure what kind of
	 * transition should be used, thus generating a S when we expected a
	 * Sr.
	 *
	 * To guarantee atomicity on this transfer queuing operation, we
	 * disable the master, then queue things and finally re-enable it, but
	 * this means we have a short period of time during which we can miss
	 * IBI/HJ events.
	 *
	 * This should hopefully be fixed with the next version of this IP.
	 */
	writel(readl(master->regs + CTRL) & ~CTRL_DEV_EN, master->regs + CTRL);

	for (i = 0; i < nxfers; i++) {
		u32 cmd0, isr;

		cmd0 = CMD0_FIFO_DEV_ADDR(xfers[i].addr) |
		       CMD0_FIFO_PL_LEN(xfers[i].len) |
		       CMD0_FIFO_PRIV_XMIT_MODE(XMIT_BURST_WITHOUT_SUBADDR);

		if (xfers[i].flags & I3C_PRIV_XFER_READ)
			cmd0 |= CMD0_FIFO_RNW;
		else
			cdns_i3c_master_wr_to_tx_fifo(master,
						      xfers[i].data.out,
						      xfers[i].len);

		if (!(xfers[i].flags & I3C_PRIV_XFER_STOP) || i == nxfers - 1)
			cmd0 |= CMD0_FIFO_RSBC;

		if (!i || (xfers[i - 1].flags & I3C_PRIV_XFER_STOP))
			cmd0 |= CMD0_FIFO_BCH;

		writel_relaxed(0, master->regs + CMD1_FIFO);
		writel_relaxed(cmd0, master->regs + CMD0_FIFO);

		if (!(xfers[i].flags & I3C_PRIV_XFER_STOP) && i < nxfers - 1)
			continue;

		writel(readl(master->regs + CTRL) | CTRL_DEV_EN,
		       master->regs + CTRL);

		isr = cdns_i3c_master_wait_for_irqs(master,
						    MST_INT_NACK |
						    MST_INT_COMP);
		if (isr != MST_INT_COMP) {
			cdns_i3c_master_drain_rx_fifo(master);
			if (isr & MST_INT_NACK)
				ret = -EIO;
			else
				ret = -ETIMEDOUT;

			break;
		}

		for (; j <= i; j++) {
			if (xfers[j].flags & I3C_PRIV_XFER_READ)
				cdns_i3c_master_rd_from_rx_fifo(master,
							xfers[j].data.in,
							xfers[j].len);
		}

		cdns_i3c_master_init_irqs(master, MST_INT_NACK | MST_INT_COMP);
		writel(readl(master->regs + CTRL) & ~CTRL_DEV_EN,
		       master->regs + CTRL);
	}

	writel(readl(master->regs + CTRL) | CTRL_DEV_EN, master->regs + CTRL);

	mutex_unlock(&master->lock);

	return ret;
}

#define I3C_DDR_FIRST_DATA_WORD_PREAMBLE	0x2
#define I3C_DDR_DATA_WORD_PREAMBLE		0x3

#define I3C_DDR_PREAMBLE(p)			((p) << 18)

static u32 prepare_ddr_word(u16 payload)
{
	u32 ret;
	u16 pb;

	ret = (u32)payload << 2;

	/* Calculate parity. */
	pb = (payload >> 15) ^ (payload >> 13) ^ (payload >> 11) ^
	     (payload >> 9) ^ (payload >> 7) ^ (payload >> 5) ^
	     (payload >> 3) ^ (payload >> 1);
	ret |= (pb & 1) << 1;
	pb = (payload >> 14) ^ (payload >> 12) ^ (payload >> 10) ^
	     (payload >> 8) ^ (payload >> 6) ^ (payload >> 4) ^
	     (payload >> 2) ^ payload ^ 1;
	ret |= (pb & 1);

	return ret;
}

static u32 prepare_ddr_data_word(u16 data, bool first)
{
	return prepare_ddr_word(data) | I3C_DDR_PREAMBLE(first ? 2 : 3);
}

#define I3C_DDR_READ_CMD	BIT(15)

static u32 prepare_ddr_cmd_word(u16 cmd)
{
	return prepare_ddr_word(cmd) | I3C_DDR_PREAMBLE(1);
}

static u32 prepare_ddr_crc_word(u8 crc5)
{
	return (((u32)crc5 & 0x1f) << 9) | (0xc << 14) |
	       I3C_DDR_PREAMBLE(1);
}

static u8 update_crc5(u8 crc5, u16 word)
{
	u8 crc0;
	int i;

	/*
	 * crc0 = next_data_bit ^ crc[4]
	 *                1         2            3       4
	 * crc[4:0] = { crc[3:2], crc[1]^crc0, crc[0], crc0 }
	 */
	for (i = 0; i < 16; ++i) {
		crc0 = ((word >> (15 - i)) ^ (crc5 >> 4)) & 0x1;
		crc5 = ((crc5 << 1) & (0x18 | 0x2)) |
		       (((crc5 >> 1) ^ crc0) << 2) | crc0;
	}

	return crc5 & 0x1F;
}

static int cdns_i3c_master_send_hdr_cmd(struct i3c_master_controller *m,
					const struct i3c_hdr_cmd *cmds,
					int ncmds)
{
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	int ret, i, ntxwords = 1, nrxwords = 0, pl_len = 1, ncmdwords = 2;
	u16 cmdword, datain;
	u32 checkword, word;
	u32 isr;
	u8 crc5;

	if (ncmds < 1)
		return 0;

	if (ncmds > 1 || cmds[0].ndatawords > CMD0_FIFO_PL_LEN_MAX)
		return -ENOTSUPP;

	if (cmds[0].mode != I3C_HDR_DDR)
		return -ENOTSUPP;

	cmdword = ((u16)cmds[0].code << 8) | (cmds[0].addr << 1);
	if (cmdword & I3C_DDR_READ_CMD)
		nrxwords += cmds[0].ndatawords + 1;
	else
		ntxwords += cmds[0].ndatawords + 1;

	if (ntxwords > master->caps.txfifodepth ||
	    nrxwords > master->caps.rxfifodepth ||
	    ncmdwords > master->caps.cmdfifodepth)
		return -ENOTSUPP;

	if (cmdword & I3C_DDR_READ_CMD) {
		u16 pb;

		pb = (cmdword >> 14) ^ (cmdword >> 12) ^ (cmdword >> 10) ^
		     (cmdword >> 8) ^ (cmdword >> 6) ^ (cmdword >> 4) ^
		     (cmdword >> 2);

		if (pb & 1)
			cmdword |= BIT(0);
	}

	mutex_lock(&master->lock);

	writel(prepare_ddr_cmd_word(cmdword),
	       master->regs + TX_FIFO);

	crc5 = update_crc5(0x1f, cmdword);

	if (!(cmdword & I3C_DDR_READ_CMD)) {
		for (i = 0; i < cmds[0].ndatawords; i++) {
			crc5 = update_crc5(crc5, cmds[0].data.out[i]);
			writel(prepare_ddr_data_word(cmds[0].data.out[i], !i),
			       master->regs + TX_FIFO);
		}

		writel(prepare_ddr_crc_word(crc5), master->regs + TX_FIFO);
		pl_len += 1 + cmds[0].ndatawords;
	}

	cdns_i3c_master_init_irqs(master,
				  MST_INT_NACK | MST_INT_COMP |
				  MST_INT_DDR_FAIL);

	/*
	 * FIXME: The IP does not support stalling the output message queue
	 * while we are queuing I3C HDR commands, and we have no way to tell
	 * the I3C master whether we want an HDR Restart or an HDR Exit
	 * between two HDR commands. Instead, the engine decides by itself when
	 * HDR Restart should be used based on the next command in the queue.
	 * The problem is, we are not guaranteed to queue the second message
	 * before the master has finished transmitting the first one, and the
	 * engine might see an empty FIFO when it tries to figure what kind of
	 * transition should be used, thus generating an HDR Exit when we
	 * expected an HDR Restart.
	 *
	 * To guarantee atomicity on this command queuing operation, we disable
	 * the master, then queue things and finally re-enable it, but this
	 * means we have a short period of time during which we can miss IBI/HJ
	 * events.
	 *
	 * This should hopefully be fixed with the next version of this IP.
	 */
	writel(readl(master->regs + CTRL) & ~CTRL_DEV_EN, master->regs + CTRL);

	/* Queue ENTHDR command */
	writel(CMD1_FIFO_CCC(I3C_CCC_ENTHDR(0)),
	       master->regs + CMD1_FIFO);
	writel(CMD0_FIFO_IS_CCC, master->regs + CMD0_FIFO);

	writel(0, master->regs + CMD1_FIFO);
	writel(CMD0_FIFO_IS_DDR | CMD0_FIFO_PL_LEN(pl_len) |
	       (cmdword & I3C_DDR_READ_CMD ? CMD0_FIFO_RNW : 0) |
	       CMD0_FIFO_DEV_ADDR(cmds[0].addr),
	       master->regs + CMD0_FIFO);

	writel(readl(master->regs + CTRL) | CTRL_DEV_EN, master->regs + CTRL);
	isr = cdns_i3c_master_wait_for_irqs(master,
					    MST_INT_NACK |
					    MST_INT_COMP |
					    MST_INT_DDR_FAIL);
	if (isr != MST_INT_COMP) {
		if (!isr)
			ret = -ETIMEDOUT;
		else
			ret = -EIO;

		goto err_drain_fifo;
	}

	if (!(cmdword & I3C_DDR_READ_CMD))
		return 0;

	for (i = 0; i < cmds[0].ndatawords; i++) {
		word = readl(master->regs + RX_FIFO);
		datain = (word >> 2) & GENMASK(15, 0);
		checkword = prepare_ddr_data_word(datain, !i);
		word &= GENMASK(19, 0);
		if (checkword != word) {
			ret = -EIO;
			goto err_drain_fifo;
		}

		crc5 = update_crc5(crc5, datain);
		cmds[0].data.in[i] = datain;
	}

	word = readl(master->regs + RX_FIFO);
	word &= GENMASK(19, 7);
	datain = (word >> 2) & GENMASK(15, 0);
	checkword = prepare_ddr_crc_word(crc5);
	if (checkword != word) {
		ret = -EIO;
		goto err_drain_fifo;
	}

	mutex_unlock(&master->lock);

	return 0;

err_drain_fifo:
	cdns_i3c_master_drain_rx_fifo(master);
	mutex_unlock(&master->lock);

	return ret;
}

static int cdns_i3c_master_i2c_xfers(struct i3c_master_controller *m,
				     const struct i2c_msg *xfers, int nxfers)
{
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	int i, ret = 0;

	for (i = 0; i < nxfers; i++) {
		if (xfers[i].len > CMD0_FIFO_PL_LEN_MAX)
			return -ENOTSUPP;
	}

	mutex_lock(&master->lock);

	for (i = 0; i < nxfers; i++) {
		u32 cmd0, isr;

		cmd0 = CMD0_FIFO_DEV_ADDR(xfers[i].addr) |
		       CMD0_FIFO_PL_LEN(xfers[i].len) |
		       CMD0_FIFO_PRIV_XMIT_MODE(XMIT_BURST_WITHOUT_SUBADDR);

		if (xfers[i].flags & I2C_M_TEN)
			cmd0 |= CMD0_FIFO_IS_10B;

		if (xfers[i].flags & I2C_M_RD)
			cmd0 |= CMD0_FIFO_RNW;
		else
			cdns_i3c_master_wr_to_tx_fifo(master, xfers[i].buf,
						      xfers[i].len);

		cdns_i3c_master_init_irqs(master, MST_INT_NACK | MST_INT_COMP);
		writel(0, master->regs + CMD1_FIFO);
		writel(cmd0, master->regs + CMD0_FIFO);
		isr = cdns_i3c_master_wait_for_irqs(master,
						    MST_INT_NACK |
						    MST_INT_COMP);

		if (xfers[i].flags & I2C_M_RD) {
			if (isr == MST_INT_COMP)
				cdns_i3c_master_rd_from_rx_fifo(master,
								xfers[i].buf,
								xfers[i].len);
			else
				cdns_i3c_master_drain_rx_fifo(master);
		}

		if (isr & MST_INT_NACK) {
			ret = -EIO;
			break;
		} else if (!(isr & MST_INT_COMP)) {
			ret = -ETIMEDOUT;
			break;
		}
	}

	mutex_unlock(&master->lock);

	return ret;
}

struct cdns_i3c_i2c_dev_data {
	int id;
};

static u32 prepare_rr0_dev_address(u32 addr)
{
	u32 ret = (addr << 1) & 0xff;

	/* RR0[7:1] = addr[6:0] */
	ret |= (addr & GENMASK(6, 0)) << 1;

	/* RR0[15:13] = addr[9:7] */
	ret |= (addr & GENMASK(9, 7)) << 6;

	/* RR0[0] = ~XOR(addr[6:0]) */
	if (!(hweight8(addr & 0x7f) & 1))
		ret |= 1;

	return ret;
}

static int cdns_i3c_master_attach_i3c_dev(struct cdns_i3c_master *master,
					  struct i3c_device *dev)
{
	struct cdns_i3c_i2c_dev_data *data;
	u32 val;

	if (!master->free_dev_slots)
		return -ENOMEM;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->id = ffs(master->free_dev_slots) - 1;
	clear_bit(data->id, &master->free_dev_slots);
	i3c_device_set_master_data(dev, data);

	if (dev->info.dyn_addr)
		val = prepare_rr0_dev_address(dev->info.dyn_addr) |
		      DEV_ID_RR0_IS_I3C;
	else
		val = prepare_rr0_dev_address(dev->info.static_addr);

	if (dev->info.dcr & I3C_BCR_HDR_CAP)
		val |= DEV_ID_RR0_HDR_CAP;

	writel(val, master->regs + DEV_ID_RR0(data->id));
	writel(DEV_ID_RR1_PID_MSB(dev->info.pid),
	       master->regs + DEV_ID_RR1(data->id));
	writel(DEV_ID_RR2_DCR(dev->info.dcr) | DEV_ID_RR2_BCR(dev->info.bcr) |
	       DEV_ID_RR2_PID_LSB(dev->info.pid),
	       master->regs + DEV_ID_RR2(data->id));
	writel(readl(master->regs + DEVS_CTRL) |
	       DEVS_CTRL_DEV_ACTIVE(data->id), master->regs + DEVS_CTRL);

	return 0;
}

static void cdns_i3c_master_detach_i3c_dev(struct cdns_i3c_master *master,
					   struct i3c_device *dev)
{
	struct cdns_i3c_i2c_dev_data *data = i3c_device_get_master_data(dev);

	if (!data)
		return;

	set_bit(data->id, &master->free_dev_slots);
	writel(readl(master->regs + DEVS_CTRL) |
	       DEVS_CTRL_DEV_CLR(data->id), master->regs + DEVS_CTRL);

	i3c_device_set_master_data(dev, NULL);
	kfree(data);
}

static int cdns_i3c_master_attach_i2c_dev(struct cdns_i3c_master *master,
					  struct i2c_device *dev)
{
	struct cdns_i3c_i2c_dev_data *data;

	if (!master->free_dev_slots)
		return -ENOMEM;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->id = ffs(master->free_dev_slots) - 1;
	clear_bit(data->id, &master->free_dev_slots);
	i2c_device_set_master_data(dev, data);

	writel(prepare_rr0_dev_address(dev->info.addr) |
	       (dev->info.flags & I2C_CLIENT_TEN ? DEV_ID_RR0_LVR_EXT_ADDR : 0),
	       master->regs + DEV_ID_RR0(data->id));
	writel(dev->lvr, master->regs + DEV_ID_RR2(data->id));
	writel(readl(master->regs + DEVS_CTRL) |
	       DEVS_CTRL_DEV_ACTIVE(data->id), master->regs + DEVS_CTRL);

	return 0;
}

static void cdns_i3c_master_detach_i2c_dev(struct cdns_i3c_master *master,
					   struct i2c_device *dev)
{
	struct cdns_i3c_i2c_dev_data *data = i2c_device_get_master_data(dev);

	if (!data)
		return;

	set_bit(data->id, &master->free_dev_slots);
	writel(readl(master->regs + DEVS_CTRL) |
	       DEVS_CTRL_DEV_CLR(data->id),
	       master->regs + DEVS_CTRL);

	i2c_device_set_master_data(dev, NULL);
	kfree(data);
}

static int cdns_i3c_master_disable(struct cdns_i3c_master *master)
{
	u32 status;

	writel(0, master->regs + CTRL);

	return readl_poll_timeout(master->regs + MST_STATUS0, status,
				  status & MST_STATUS0_IDLE, 10, 1000000);
}

static void cdns_i3c_master_bus_cleanup(struct i3c_master_controller *m)
{
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	struct i2c_device *i2cdev;
	struct i3c_device *i3cdev;

	cdns_i3c_master_disable(master);

	i3c_bus_for_each_i2cdev(m->bus, i2cdev)
		cdns_i3c_master_detach_i2c_dev(master, i2cdev);

	i3c_bus_for_each_i3cdev(m->bus, i3cdev)
		cdns_i3c_master_detach_i3c_dev(master, i3cdev);
}

static void cdns_i3c_master_dev_rr_to_info(struct cdns_i3c_master *master,
					   unsigned int slot,
					   struct i3c_device_info *info)
{
	u32 rr;

	memset(info, 0, sizeof(info));
	rr = readl(master->regs + DEV_ID_RR0(slot));
	info->dyn_addr = DEV_ID_RR0_GET_DEV_ADDR(rr);
	rr = readl(master->regs + DEV_ID_RR2(slot));
	info->dcr = rr;
	info->bcr = rr >> 8;
	info->pid = rr >> 16;
	info->pid |= (u64)readl(master->regs + DEV_ID_RR1(slot)) << 16;
}

static int cdns_i3c_master_bus_init(struct i3c_master_controller *m)
{
	unsigned long pres_step, sysclk_rate, max_i2cfreq, i3c_scl_lim = 0;
	struct cdns_i3c_master *master = to_cdns_i3c_master(m);
	u32 ctrl, prescl0, prescl1, pres, low;
	struct i3c_device_info info = { };
	struct i3c_ccc_events events;
	struct i2c_device *i2cdev;
	struct i3c_device *i3cdev;
	int ret, slot, ncycles;
	u8 last_addr = 0;
	u32 status, devs;

	switch (m->bus->mode) {
	case I3C_BUS_MODE_PURE:
		ctrl = CTRL_PURE_BUS_MODE;
		break;

	case I3C_BUS_MODE_MIXED_FAST:
		ctrl = CTRL_MIXED_FAST_BUS_MODE;
		break;

	case I3C_BUS_MODE_MIXED_SLOW:
		ctrl = CTRL_MIXED_SLOW_BUS_MODE;
		break;

	default:
		return -EINVAL;
	}

	sysclk_rate = clk_get_rate(master->sysclk);
	if (!sysclk_rate)
		return -EINVAL;

	pres = DIV_ROUND_UP(sysclk_rate, (m->bus->scl_rate.i3c * 4)) - 1;
	if (pres > PRESCL_CTRL0_MAX)
		return -ERANGE;

	m->bus->scl_rate.i3c = sysclk_rate / ((pres + 1) * 4);

	prescl0 = PRESCL_CTRL0_I3C(pres);

	low = ((I3C_BUS_TLOW_OD_MIN_NS * sysclk_rate) / (pres + 1)) - 2;
	prescl1 = PRESCL_CTRL1_OD_LOW(low);

	max_i2cfreq = m->bus->scl_rate.i2c;

	pres = (sysclk_rate / (max_i2cfreq * 5)) - 1;
	if (pres > PRESCL_CTRL0_MAX)
		return -ERANGE;

	m->bus->scl_rate.i2c = sysclk_rate / ((pres + 1) * 5);

	prescl0 |= PRESCL_CTRL0_I2C(pres);

	writel(DEVS_CTRL_DEV_CLR_ALL, master->regs + DEVS_CTRL);

	i3c_bus_for_each_i2cdev(m->bus, i2cdev) {
		ret = cdns_i3c_master_attach_i2c_dev(master, i2cdev);
		if (ret)
			goto err_detach_devs;
	}

	writel(prescl0, master->regs + PRESCL_CTRL0);

	/* Calculate OD and PP low. */
	pres_step = 1000000000 / (m->bus->scl_rate.i3c * 4);
	ncycles = DIV_ROUND_UP(I3C_BUS_TLOW_OD_MIN_NS, pres_step) - 2;
	if (ncycles < 0)
		ncycles = 0;
	prescl1 = PRESCL_CTRL1_OD_LOW(ncycles);
	writel(prescl1, master->regs + PRESCL_CTRL1);

	i3c_bus_for_each_i3cdev(m->bus, i3cdev) {
		ret = cdns_i3c_master_attach_i3c_dev(master, i3cdev);
		if (ret)
			goto err_detach_devs;
	}

	/* Get an address for the master. */
	ret = i3c_master_get_free_addr(m, 0);
	if (ret < 0)
		goto err_detach_devs;

	writel(prepare_rr0_dev_address(ret) | DEV_ID_RR0_IS_I3C,
	       master->regs + DEV_ID_RR0(0));

	cdns_i3c_master_dev_rr_to_info(master, 0, &info);
	if (info.bcr & I3C_BCR_HDR_CAP)
		info.hdr_cap = I3C_CCC_HDR_MODE(I3C_HDR_DDR);

	ret = i3c_master_set_info(&master->base, &info);
	if (ret)
		goto err_detach_devs;

	/* Prepare RR slots before lauching DAA. */
	for (slot = find_next_bit(&master->free_dev_slots, BITS_PER_LONG, 1);
	     slot < BITS_PER_LONG;
	     slot = find_next_bit(&master->free_dev_slots,
				  BITS_PER_LONG, slot + 1)) {
		ret = i3c_master_get_free_addr(m, last_addr + 1);
		if (ret < 0)
			goto err_disable_master;

		last_addr = ret;
		writel(prepare_rr0_dev_address(last_addr) | DEV_ID_RR0_IS_I3C,
		       master->regs + DEV_ID_RR0(slot));
		writel(0, master->regs + DEV_ID_RR1(slot));
		writel(0, master->regs + DEV_ID_RR2(slot));
	}

	writel(ctrl | CTRL_DEV_EN, master->regs + CTRL);

	/*
	 * Reset all dynamic addresses on the bus, because we don't know what
	 * happened before this point (the bootloader may have assigned dynamic
	 * addresses that we're not aware of).
	 */
	ret = i3c_master_rstdaa_locked(m, I3C_BROADCAST_ADDR);
	if (ret)
		goto err_disable_master;

	/* Disable all slave events (interrupts) before starting DAA. */
	events.events = I3C_CCC_EVENT_SIR | I3C_CCC_EVENT_MR |
			I3C_CCC_EVENT_HJ;
	ret = i3c_master_disec_locked(m, I3C_BROADCAST_ADDR, &events);
	if (ret)
		goto err_disable_master;

	ret = i3c_master_entdaa_locked(m);
	if (ret)
		goto err_disable_master;

	status = readl(master->regs + MST_STATUS0);

	/* No devices discovered, bail out. */
	if (!(status & MST_STATUS0_DAA_COMP))
		return 0;

	/* Now add discovered devices to the bus. */
	devs = readl(master->regs + DEVS_CTRL);
	for (slot = find_next_bit(&master->free_dev_slots, BITS_PER_LONG, 1);
	     slot < BITS_PER_LONG;
	     slot = find_next_bit(&master->free_dev_slots,
				  BITS_PER_LONG, slot + 1)) {
		struct cdns_i3c_i2c_dev_data *data;
		u32 sircfg, rr, max_fscl = 0;
		u8 addr;

		if (!(devs & DEVS_CTRL_DEV_ACTIVE(slot)))
			continue;

		data = kzalloc(sizeof(*data), GFP_KERNEL);
		if (!data)
			goto err_disable_master;

		data->id = slot;
		rr = readl(master->regs + DEV_ID_RR0(slot));
		addr = DEV_ID_RR0_GET_DEV_ADDR(rr);
		i3cdev = i3c_master_add_i3c_dev_locked(m, addr);
		if (IS_ERR(i3cdev)) {
			ret = PTR_ERR(i3cdev);
			goto err_disable_master;
		}

		i3c_device_get_info(i3cdev, &info);
		clear_bit(data->id, &master->free_dev_slots);
		i3c_device_set_master_data(i3cdev, data);

		max_fscl = max(I3C_CCC_MAX_SDR_FSCL(info.max_read_ds),
			       I3C_CCC_MAX_SDR_FSCL(info.max_write_ds));
		switch (max_fscl) {
		case I3C_SDR_DR_FSCL_8MHZ:
			max_fscl = 8000000;
			break;
		case I3C_SDR_DR_FSCL_6MHZ:
			max_fscl = 6000000;
			break;
		case I3C_SDR_DR_FSCL_4MHZ:
			max_fscl = 4000000;
			break;
		case I3C_SDR_DR_FSCL_2MHZ:
			max_fscl = 2000000;
			break;
		case I3C_SDR_DR_FSCL_MAX:
		default:
			max_fscl = 0;
			break;
		}

		if (max_fscl && (max_fscl < i3c_scl_lim || !i3c_scl_lim))
			i3c_scl_lim = max_fscl;

		if (!(info.bcr & I3C_BCR_IBI_REQ_CAP))
			continue;

		if ((info.bcr & I3C_BCR_IBI_PAYLOAD) &&
		    (!info.max_ibi_len ||
		     info.max_ibi_len > SIR_MAP_PL_MAX)) {
			ret = -ENOTSUPP;
			goto err_disable_master;
		}

		sircfg = readl(master->regs + SIR_MAP_DEV_REG(slot));
		sircfg &= ~SIR_MAP_DEV_MASK(slot);
		sircfg |= SIR_MAP_DEV_ROLE(slot, info.bcr >> 6) |
			  SIR_MAP_DEV_DA(slot, info.dyn_addr) |
			  SIR_MAP_DEV_PL(slot, info.max_ibi_len);

		if (info.bcr & I3C_BCR_MAX_DATA_SPEED_LIM)
			sircfg |= SIR_MAP_DEV_SLOW(slot);

		/* Do not ack IBI requests until explicitly requested. */
		writel(sircfg, master->regs + SIR_MAP_DEV_REG(slot));
	}

	ret = i3c_master_defslvs_locked(m);
	if (ret)
		goto err_disable_master;

	/* Configure PP_LOW to meet I3C slave limitations. */
	if (i3c_scl_lim && i3c_scl_lim < m->bus->scl_rate.i3c) {
		unsigned long i3c_lim_period;

		i3c_lim_period = DIV_ROUND_UP(1000000000, i3c_scl_lim);
		ncycles = DIV_ROUND_UP(i3c_lim_period, pres_step) - 4;
		if (ncycles < 0)
			ncycles = 0;
		prescl1 |= PRESCL_CTRL1_PP_LOW(ncycles);

		/* Disable I3C master before updating PRESCL_CTRL1. */
		writel(ctrl, master->regs + CTRL);
		ret = readl_poll_timeout(master->regs + MST_STATUS0, status,
					 status & MST_STATUS0_IDLE, 1,
					 1000000);
		if (ret)
			goto err_disable_master;

		writel(prescl1, master->regs + PRESCL_CTRL1);
		writel(ctrl | CTRL_DEV_EN, master->regs + CTRL);
	}

	return 0;

err_disable_master:
	cdns_i3c_master_disable(master);

err_detach_devs:
	cdns_i3c_master_bus_cleanup(m);

	return ret;
}

static const struct i3c_master_controller_ops cdns_i3c_master_ops = {
	.bus_init = cdns_i3c_master_bus_init,
	.bus_cleanup = cdns_i3c_master_bus_cleanup,
	.supports_ccc_cmd = cdns_i3c_master_supports_ccc_cmd,
	.send_ccc_cmd = cdns_i3c_master_send_ccc_cmd,
	.send_hdr_cmds = cdns_i3c_master_send_hdr_cmd,
	.priv_xfers = cdns_i3c_master_priv_xfers,
	.i2c_xfers = cdns_i3c_master_i2c_xfers,
};

static irqreturn_t cdns_i3c_master_interrupt(int irq, void *data)
{
	struct cdns_i3c_master *master = data;
	u32 status;

	status = readl(master->regs + MST_ISR) & readl(master->regs + MST_IMR);
	if (!status)
		return IRQ_NONE;

	writel(status, master->regs + MST_IDR);
	complete(&master->comp);

	return IRQ_HANDLED;
}

static int cdns_i3c_master_probe(struct platform_device *pdev)
{
	struct cdns_i3c_master *master;
	struct resource *res;
	int ret, irq;
	u32 val;

	master = devm_kzalloc(&pdev->dev, sizeof(*master), GFP_KERNEL);
	if (!master)
		return -ENOMEM;

	master->caps.cmdfifodepth = 8;
	master->caps.rxfifodepth = 16;
	master->caps.txfifodepth = 16;

	init_completion(&master->comp);
	mutex_init(&master->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	master->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(master->regs))
		return PTR_ERR(master->regs);

	master->pclk = devm_clk_get(&pdev->dev, "pclk");
	if (IS_ERR(master->pclk))
		return PTR_ERR(master->pclk);

	master->sysclk = devm_clk_get(&pdev->dev, "sysclk");
	if (IS_ERR(master->pclk))
		return PTR_ERR(master->pclk);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = clk_prepare_enable(master->pclk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(master->sysclk);
	if (ret)
		goto err_disable_pclk;

	writel(0xffffffff, master->regs + MST_IDR);
	writel(0xffffffff, master->regs + SLV_IDR);
	ret = devm_request_irq(&pdev->dev, irq, cdns_i3c_master_interrupt, 0,
			       dev_name(&pdev->dev), master);
	if (ret)
		goto err_disable_sysclk;

	platform_set_drvdata(pdev, master);

	val = readl(master->regs + CONF_STATUS);

	/* Device ID0 is reserved to describe this master. */
	master->free_dev_slots = GENMASK(CONF_STATUS_DEVS_NUM(val), 1);

	ret = i3c_master_register(&master->base, &pdev->dev,
				  &cdns_i3c_master_ops, false);
	if (ret)
		goto err_disable_sysclk;

err_disable_sysclk:
	clk_disable_unprepare(master->sysclk);

err_disable_pclk:
	clk_disable_unprepare(master->pclk);

	return ret;
}

static int cdns_i3c_master_remove(struct platform_device *pdev)
{
	struct cdns_i3c_master *master = platform_get_drvdata(pdev);
	int ret;

	ret = i3c_master_unregister(&master->base);
	if (ret)
		return ret;

	clk_disable_unprepare(master->sysclk);
	clk_disable_unprepare(master->pclk);

	return 0;
}

static const struct of_device_id cdns_i3c_master_of_ids[] = {
	{ .compatible = "cdns,i3c-master" },
};

static struct platform_driver cdns_i3c_master = {
	.probe = cdns_i3c_master_probe,
	.remove = cdns_i3c_master_remove,
	.driver = {
		.name = "cdns-i3c-master",
		.of_match_table = cdns_i3c_master_of_ids,
	},
};
module_platform_driver(cdns_i3c_master);
