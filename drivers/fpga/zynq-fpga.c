/*
 * Copyright (c) 2011-2015 Xilinx Inc.
 * Copyright (c) 2015, National Instruments Corp.
 *
 * FPGA Manager Driver for Xilinx Zynq, heavily based on xdevcfg driver
 * in their vendor tree.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/scatterlist.h>
#include <linux/seq_file.h>

/* Offsets into SLCR regmap */

/* FPGA Software Reset Control */
#define SLCR_FPGA_RST_CTRL_OFFSET	0x240
/* Level Shifters Enable */
#define SLCR_LVL_SHFTR_EN_OFFSET	0x900

/* Constant Definitions */

/* Control Register */
#define CTRL_OFFSET			0x00
/* Lock Register */
#define LOCK_OFFSET			0x04
/* Interrupt Status Register */
#define INT_STS_OFFSET			0x0c
/* Interrupt Mask Register */
#define INT_MASK_OFFSET			0x10
/* Status Register */
#define STATUS_OFFSET			0x14
/* DMA Source Address Register */
#define DMA_SRC_ADDR_OFFSET		0x18
/* DMA Destination Address Reg */
#define DMA_DST_ADDR_OFFSET		0x1c
/* DMA Source Transfer Length */
#define DMA_SRC_LEN_OFFSET		0x20
/* DMA Destination Transfer */
#define DMA_DEST_LEN_OFFSET		0x24
/* Unlock Register */
#define UNLOCK_OFFSET			0x34
/* Misc. Control Register */
#define MCTRL_OFFSET			0x80

/* Control Register Bit definitions */

/* Signal to reset FPGA */
#define CTRL_PCFG_PROG_B_MASK		BIT(30)
/* Enable PCAP for PR */
#define CTRL_PCAP_PR_MASK		BIT(27)
/* Enable PCAP */
#define CTRL_PCAP_MODE_MASK		BIT(26)
/* Lower rate to allow decrypt on the fly */
#define CTRL_PCAP_RATE_EN_MASK		BIT(25)
/* System booted in secure mode */
#define CTRL_SEC_EN_MASK		BIT(7)

/* Miscellaneous Control Register bit definitions */
/* Internal PCAP loopback */
#define MCTRL_PCAP_LPBK_MASK		BIT(4)

/* Status register bit definitions */

/* FPGA init status */
#define STATUS_DMA_Q_F			BIT(31)
#define STATUS_DMA_Q_E			BIT(30)
#define STATUS_PCFG_INIT_MASK		BIT(4)

/* Interrupt Status/Mask Register Bit definitions */
/* DMA command done */
#define IXR_DMA_DONE_MASK		BIT(13)
/* DMA and PCAP cmd done */
#define IXR_D_P_DONE_MASK		BIT(12)
 /* FPGA programmed */
#define IXR_PCFG_DONE_MASK		BIT(2)
#define IXR_ERROR_FLAGS_MASK		0x00F0C860
#define IXR_ALL_MASK			0xF8F7F87F

/* Miscellaneous constant values */

/* Invalid DMA addr */
#define DMA_INVALID_ADDRESS		GENMASK(31, 0)
/* Used to unlock the dev */
#define UNLOCK_MASK			0x757bdf0d
/* Timeout for polling reset bits */
#define INIT_POLL_TIMEOUT		2500000
/* Delay for polling reset bits */
#define INIT_POLL_DELAY			20
/* Signal this is the last DMA transfer, wait for the AXI and PCAP before
 * interrupting
 */
#define DMA_SRC_LAST_TRANSFER		1
/* Timeout for DMA completion */
#define DMA_TIMEOUT_MS			5000

/* Masks for controlling stuff in SLCR */
/* Disable all Level shifters */
#define LVL_SHFTR_DISABLE_ALL_MASK	0x0
/* Enable Level shifters from PS to PL */
#define LVL_SHFTR_ENABLE_PS_TO_PL	0xa
/* Enable Level shifters from PL to PS */
#define LVL_SHFTR_ENABLE_PL_TO_PS	0xf
/* Enable global resets */
#define FPGA_RST_ALL_MASK		0xf
/* Disable global resets */
#define FPGA_RST_NONE_MASK		0x0

/**
 * struct zynq_configreg - Configuration register offsets
 * @reg:	Name of the configuration register.
 * @offset:	Register offset.
 */
struct zynq_configreg {
	char *reg;
	u32 offset;
};

static struct zynq_configreg cfgreg[] = {
	{.reg = "CRC",		.offset = 0},
	{.reg = "FAR",		.offset = 1},
	{.reg = "FDRI",		.offset = 2},
	{.reg = "FDRO",		.offset = 3},
	{.reg = "CMD",		.offset = 4},
	{.reg = "CTRL0",	.offset = 5},
	{.reg = "MASK",		.offset = 6},
	{.reg = "STAT",		.offset = 7},
	{.reg = "LOUT",		.offset = 8},
	{.reg = "COR0",		.offset = 9},
	{.reg = "MFWR",		.offset = 10},
	{.reg = "CBC",		.offset = 11},
	{.reg = "IDCODE",	.offset = 12},
	{.reg = "AXSS",		.offset = 13},
	{.reg = "COR1",		.offset = 14},
	{.reg = "WBSTR",	.offset = 16},
	{.reg = "TIMER",	.offset = 17},
	{.reg = "BOOTSTS",	.offset = 22},
	{.reg = "CTRL1",	.offset = 24},
	{}
};

/* Masks for Configuration registers */
#define FAR_ADDR_MASK		0x00000000
#define RCFG_CMD_MASK		BIT(2)
#define START_CMD_MASK		BIT(2) + BIT(0)
#define RCRC_CMD_MASK		GENMASK(2, 0)
#define SHUTDOWN_CMD_MASK	GENMASK(1, 0) + BIT(3)
#define DESYNC_WORD_MASK	GENMASK(2, 3) + BIT(0)
#define BUSWIDTH_SYNCWORD_MASK	0x000000BB
#define NOOP_WORD_MASK		BIT(29)
#define BUSWIDTH_DETECT_MASK	0x11220044
#define SYNC_WORD_MASK		0xAA995566
#define DUMMY_WORD_MASK		GENMASK(31, 0)

#define TYPE_HDR_SHIFT		29
#define TYPE_REG_SHIFT		13
#define TYPE_OP_SHIFT		27
#define TYPE_OPCODE_NOOP	0
#define TYPE_OPCODE_READ	1
#define TYPE_OPCODE_WRITE	2
#define TYPE_FAR_OFFSET		1
#define TYPE_FDRO_OFFSET	3
#define TYPE_CMD_OFFSET		4

#define READ_STEP5_NOOPS	6
#define READ_STEP9_NOOPS	32

#define READ_DMA_SIZE		0x200
#define DUMMY_FRAMES_SIZE	0x28
#define SLCR_PCAP_FREQ		10000000

struct zynq_fpga_priv {
	int irq;
	struct clk *clk;

	void __iomem *io_base;
	struct regmap *slcr;

	spinlock_t dma_lock;
	unsigned int dma_elm;
	unsigned int dma_nelms;
	struct scatterlist *cur_sg;

	struct completion dma_done;
#ifdef CONFIG_FPGA_MGR_DEBUG_FS
	struct mutex ref_mutex;
	struct dentry *dir;
#endif
	u32 size;
};

static inline void zynq_fpga_write(struct zynq_fpga_priv *priv, u32 offset,
				   u32 val)
{
	writel(val, priv->io_base + offset);
}

static inline u32 zynq_fpga_read(const struct zynq_fpga_priv *priv,
				 u32 offset)
{
	return readl(priv->io_base + offset);
}

#define zynq_fpga_poll_timeout(priv, addr, val, cond, sleep_us, timeout_us) \
	readl_poll_timeout(priv->io_base + addr, val, cond, sleep_us, \
			   timeout_us)

/* Cause the specified irq mask bits to generate IRQs */
static inline void zynq_fpga_set_irq(struct zynq_fpga_priv *priv, u32 enable)
{
	zynq_fpga_write(priv, INT_MASK_OFFSET, ~enable);
}

static void zynq_fpga_dma_xfer(struct zynq_fpga_priv *priv, u32 srcaddr,
			       u32 srclen, u32 dstaddr, u32 dstlen)
{
	zynq_fpga_write(priv, DMA_SRC_ADDR_OFFSET, srcaddr);
	zynq_fpga_write(priv, DMA_DST_ADDR_OFFSET, dstaddr);
	zynq_fpga_write(priv, DMA_SRC_LEN_OFFSET, srclen);
	zynq_fpga_write(priv, DMA_DEST_LEN_OFFSET, dstlen);
}

static int zynq_fpga_wait_fordone(struct zynq_fpga_priv *priv)
{
	u32 status;
	int ret;

	ret = zynq_fpga_poll_timeout(priv, INT_STS_OFFSET, status,
				     status & IXR_D_P_DONE_MASK,
				     INIT_POLL_DELAY,
				     INIT_POLL_TIMEOUT);
	return ret;
}

/* Must be called with dma_lock held */
static void zynq_step_dma(struct zynq_fpga_priv *priv)
{
	u32 addr;
	u32 len;
	bool first;

	first = priv->dma_elm == 0;
	while (priv->cur_sg) {
		/* Feed the DMA queue until it is full. */
		if (zynq_fpga_read(priv, STATUS_OFFSET) & STATUS_DMA_Q_F)
			break;

		addr = sg_dma_address(priv->cur_sg);
		len = sg_dma_len(priv->cur_sg);
		if (priv->dma_elm + 1 == priv->dma_nelms) {
			/* The last transfer waits for the PCAP to finish too,
			 * notice this also changes the irq_mask to ignore
			 * IXR_DMA_DONE_MASK which ensures we do not trigger
			 * the completion too early.
			 */
			addr |= DMA_SRC_LAST_TRANSFER;
			priv->cur_sg = NULL;
		} else {
			priv->cur_sg = sg_next(priv->cur_sg);
			priv->dma_elm++;
		}

		priv->size += len;
		zynq_fpga_write(priv, DMA_SRC_ADDR_OFFSET, addr);
		zynq_fpga_write(priv, DMA_DST_ADDR_OFFSET, DMA_INVALID_ADDRESS);
		zynq_fpga_write(priv, DMA_SRC_LEN_OFFSET, len / 4);
		zynq_fpga_write(priv, DMA_DEST_LEN_OFFSET, 0);
	}

	/* Once the first transfer is queued we can turn on the ISR, future
	 * calls to zynq_step_dma will happen from the ISR context. The
	 * dma_lock spinlock guarentees this handover is done coherently, the
	 * ISR enable is put at the end to avoid another CPU spinning in the
	 * ISR on this lock.
	 */
	if (first && priv->cur_sg) {
		zynq_fpga_set_irq(priv,
				  IXR_DMA_DONE_MASK | IXR_ERROR_FLAGS_MASK);
	} else if (!priv->cur_sg) {
		/* The last transfer changes to DMA & PCAP mode since we do
		 * not want to continue until everything has been flushed into
		 * the PCAP.
		 */
		zynq_fpga_set_irq(priv,
				  IXR_D_P_DONE_MASK | IXR_ERROR_FLAGS_MASK);
	}
}

static irqreturn_t zynq_fpga_isr(int irq, void *data)
{
	struct zynq_fpga_priv *priv = data;
	u32 intr_status;

	/* If anything other than DMA completion is reported stop and hand
	 * control back to zynq_fpga_ops_write, something went wrong,
	 * otherwise progress the DMA.
	 */
	spin_lock(&priv->dma_lock);
	intr_status = zynq_fpga_read(priv, INT_STS_OFFSET);
	if (!(intr_status & IXR_ERROR_FLAGS_MASK) &&
	    (intr_status & IXR_DMA_DONE_MASK) && priv->cur_sg) {
		zynq_fpga_write(priv, INT_STS_OFFSET, IXR_DMA_DONE_MASK);
		zynq_step_dma(priv);
		spin_unlock(&priv->dma_lock);
		return IRQ_HANDLED;
	}
	spin_unlock(&priv->dma_lock);

	zynq_fpga_set_irq(priv, 0);
	complete(&priv->dma_done);

	return IRQ_HANDLED;
}

/* Sanity check the proposed bitstream. It must start with the sync word in
 * the correct byte order, and be dword aligned. The input is a Xilinx .bin
 * file with every 32 bit quantity swapped.
 */
static bool zynq_fpga_has_sync(const u8 *buf, size_t count)
{
	for (; count >= 4; buf += 4, count -= 4)
		if (buf[0] == 0x66 && buf[1] == 0x55 && buf[2] == 0x99 &&
		    buf[3] == 0xaa)
			return true;
	return false;
}

static int zynq_fpga_ops_write_init(struct fpga_manager *mgr,
				    struct fpga_image_info *info,
				    const char *buf, size_t count)
{
	struct zynq_fpga_priv *priv;
	u32 ctrl, status;
	int err;

	priv = mgr->priv;

	err = clk_enable(priv->clk);
	if (err)
		return err;

	/* check if bitstream is encrypted & and system's still secure */
	if (info->flags & FPGA_MGR_ENCRYPTED_BITSTREAM) {
		ctrl = zynq_fpga_read(priv, CTRL_OFFSET);
		if (!(ctrl & CTRL_SEC_EN_MASK)) {
			dev_err(&mgr->dev,
				"System not secure, can't use crypted bitstreams\n");
			err = -EINVAL;
			goto out_err;
		}
	}

	/* don't globally reset PL if we're doing partial reconfig */
	if (!(info->flags & FPGA_MGR_PARTIAL_RECONFIG)) {
		if (!zynq_fpga_has_sync(buf, count)) {
			dev_err(&mgr->dev,
				"Invalid bitstream, could not find a sync word. Bitstream must be a byte swapped .bin file\n");
			err = -EINVAL;
			goto out_err;
		}

		/* assert AXI interface resets */
		regmap_write(priv->slcr, SLCR_FPGA_RST_CTRL_OFFSET,
			     FPGA_RST_ALL_MASK);

		/* disable all level shifters */
		regmap_write(priv->slcr, SLCR_LVL_SHFTR_EN_OFFSET,
			     LVL_SHFTR_DISABLE_ALL_MASK);
		/* enable level shifters from PS to PL */
		regmap_write(priv->slcr, SLCR_LVL_SHFTR_EN_OFFSET,
			     LVL_SHFTR_ENABLE_PS_TO_PL);

		/* create a rising edge on PCFG_INIT. PCFG_INIT follows
		 * PCFG_PROG_B, so we need to poll it after setting PCFG_PROG_B
		 * to make sure the rising edge actually happens.
		 * Note: PCFG_PROG_B is low active, sequence as described in
		 * UG585 v1.10 page 211
		 */
		ctrl = zynq_fpga_read(priv, CTRL_OFFSET);
		ctrl |= CTRL_PCFG_PROG_B_MASK;

		zynq_fpga_write(priv, CTRL_OFFSET, ctrl);

		err = zynq_fpga_poll_timeout(priv, STATUS_OFFSET, status,
					     status & STATUS_PCFG_INIT_MASK,
					     INIT_POLL_DELAY,
					     INIT_POLL_TIMEOUT);
		if (err) {
			dev_err(&mgr->dev, "Timeout waiting for PCFG_INIT\n");
			goto out_err;
		}

		ctrl = zynq_fpga_read(priv, CTRL_OFFSET);
		ctrl &= ~CTRL_PCFG_PROG_B_MASK;

		zynq_fpga_write(priv, CTRL_OFFSET, ctrl);

		err = zynq_fpga_poll_timeout(priv, STATUS_OFFSET, status,
					     !(status & STATUS_PCFG_INIT_MASK),
					     INIT_POLL_DELAY,
					     INIT_POLL_TIMEOUT);
		if (err) {
			dev_err(&mgr->dev, "Timeout waiting for !PCFG_INIT\n");
			goto out_err;
		}

		ctrl = zynq_fpga_read(priv, CTRL_OFFSET);
		ctrl |= CTRL_PCFG_PROG_B_MASK;

		zynq_fpga_write(priv, CTRL_OFFSET, ctrl);

		err = zynq_fpga_poll_timeout(priv, STATUS_OFFSET, status,
					     status & STATUS_PCFG_INIT_MASK,
					     INIT_POLL_DELAY,
					     INIT_POLL_TIMEOUT);
		if (err) {
			dev_err(&mgr->dev, "Timeout waiting for PCFG_INIT\n");
			goto out_err;
		}
	}

	/* set configuration register with following options:
	 * - enable PCAP interface
	 * - set throughput for maximum speed (if bistream not crypted)
	 * - set CPU in user mode
	 */
	ctrl = zynq_fpga_read(priv, CTRL_OFFSET);
	if (info->flags & FPGA_MGR_ENCRYPTED_BITSTREAM)
		zynq_fpga_write(priv, CTRL_OFFSET,
				(CTRL_PCAP_PR_MASK | CTRL_PCAP_MODE_MASK
				 | CTRL_PCAP_RATE_EN_MASK | ctrl));
	else
		zynq_fpga_write(priv, CTRL_OFFSET,
				(CTRL_PCAP_PR_MASK | CTRL_PCAP_MODE_MASK
				 | ctrl));


	/* We expect that the command queue is empty right now. */
	status = zynq_fpga_read(priv, STATUS_OFFSET);
	if ((status & STATUS_DMA_Q_F) ||
	    (status & STATUS_DMA_Q_E) != STATUS_DMA_Q_E) {
		dev_err(&mgr->dev, "DMA command queue not right\n");
		err = -EBUSY;
		goto out_err;
	}

	/* ensure internal PCAP loopback is disabled */
	ctrl = zynq_fpga_read(priv, MCTRL_OFFSET);
	zynq_fpga_write(priv, MCTRL_OFFSET, (~MCTRL_PCAP_LPBK_MASK & ctrl));

	clk_disable(priv->clk);

	return 0;

out_err:
	clk_disable(priv->clk);

	return err;
}

static int zynq_fpga_ops_write(struct fpga_manager *mgr, struct sg_table *sgt)
{
	struct zynq_fpga_priv *priv;
	const char *why;
	int err;
	u32 intr_status;
	unsigned long timeout;
	unsigned long flags;
	struct scatterlist *sg;
	int i;

	priv = mgr->priv;
	priv->size = 0;

	/* The hardware can only DMA multiples of 4 bytes, and it requires the
	 * starting addresses to be aligned to 64 bits (UG585 pg 212).
	 */
	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		if ((sg->offset % 8) || (sg->length % 4)) {
			dev_err(&mgr->dev,
			    "Invalid bitstream, chunks must be aligned\n");
			return -EINVAL;
		}
	}

	priv->dma_nelms =
	    dma_map_sg(mgr->dev.parent, sgt->sgl, sgt->nents, DMA_TO_DEVICE);
	if (priv->dma_nelms == 0) {
		dev_err(&mgr->dev, "Unable to DMA map (TO_DEVICE)\n");
		return -ENOMEM;
	}

	/* enable clock */
	err = clk_enable(priv->clk);
	if (err)
		goto out_free;

	zynq_fpga_write(priv, INT_STS_OFFSET, IXR_ALL_MASK);
	reinit_completion(&priv->dma_done);

	/* zynq_step_dma will turn on interrupts */
	spin_lock_irqsave(&priv->dma_lock, flags);
	priv->dma_elm = 0;
	priv->cur_sg = sgt->sgl;
	zynq_step_dma(priv);
	spin_unlock_irqrestore(&priv->dma_lock, flags);

	timeout = wait_for_completion_timeout(&priv->dma_done,
					      msecs_to_jiffies(DMA_TIMEOUT_MS));

	spin_lock_irqsave(&priv->dma_lock, flags);
	zynq_fpga_set_irq(priv, 0);
	priv->cur_sg = NULL;
	spin_unlock_irqrestore(&priv->dma_lock, flags);

	intr_status = zynq_fpga_read(priv, INT_STS_OFFSET);
	zynq_fpga_write(priv, INT_STS_OFFSET, IXR_ALL_MASK);

	/* There doesn't seem to be a way to force cancel any DMA, so if
	 * something went wrong we are relying on the hardware to have halted
	 * the DMA before we get here, if there was we could use
	 * wait_for_completion_interruptible too.
	 */

	if (intr_status & IXR_ERROR_FLAGS_MASK) {
		why = "DMA reported error";
		err = -EIO;
		goto out_report;
	}

	if (priv->cur_sg ||
	    !((intr_status & IXR_D_P_DONE_MASK) == IXR_D_P_DONE_MASK)) {
		if (timeout == 0)
			why = "DMA timed out";
		else
			why = "DMA did not complete";
		err = -EIO;
		goto out_report;
	}

	err = 0;
	goto out_clk;

out_report:
	dev_err(&mgr->dev,
		"%s: INT_STS:0x%x CTRL:0x%x LOCK:0x%x INT_MASK:0x%x STATUS:0x%x MCTRL:0x%x\n",
		why,
		intr_status,
		zynq_fpga_read(priv, CTRL_OFFSET),
		zynq_fpga_read(priv, LOCK_OFFSET),
		zynq_fpga_read(priv, INT_MASK_OFFSET),
		zynq_fpga_read(priv, STATUS_OFFSET),
		zynq_fpga_read(priv, MCTRL_OFFSET));

out_clk:
	clk_disable(priv->clk);

out_free:
	dma_unmap_sg(mgr->dev.parent, sgt->sgl, sgt->nents, DMA_TO_DEVICE);
	return err;
}

static int zynq_fpga_ops_write_complete(struct fpga_manager *mgr,
					struct fpga_image_info *info)
{
	struct zynq_fpga_priv *priv = mgr->priv;
	int err;
	u32 intr_status;

	err = clk_enable(priv->clk);
	if (err)
		return err;

	err = zynq_fpga_poll_timeout(priv, INT_STS_OFFSET, intr_status,
				     intr_status & IXR_PCFG_DONE_MASK,
				     INIT_POLL_DELAY,
				     INIT_POLL_TIMEOUT);

	clk_disable(priv->clk);

	if (err)
		return err;

	/* for the partial reconfig case we didn't touch the level shifters */
	if (!(info->flags & FPGA_MGR_PARTIAL_RECONFIG)) {
		/* enable level shifters from PL to PS */
		regmap_write(priv->slcr, SLCR_LVL_SHFTR_EN_OFFSET,
			     LVL_SHFTR_ENABLE_PL_TO_PS);

		/* deassert AXI interface resets */
		regmap_write(priv->slcr, SLCR_FPGA_RST_CTRL_OFFSET,
			     FPGA_RST_NONE_MASK);
	}

	return 0;
}

static enum fpga_mgr_states zynq_fpga_ops_state(struct fpga_manager *mgr)
{
	int err;
	u32 intr_status;
	struct zynq_fpga_priv *priv;

	priv = mgr->priv;

	err = clk_enable(priv->clk);
	if (err)
		return FPGA_MGR_STATE_UNKNOWN;

	intr_status = zynq_fpga_read(priv, INT_STS_OFFSET);
	clk_disable(priv->clk);

	if (intr_status & IXR_PCFG_DONE_MASK)
		return FPGA_MGR_STATE_OPERATING;

	return FPGA_MGR_STATE_UNKNOWN;
}

static int zynq_type1_pkt(u8 reg, u8 opcode, u16 size)
{
	/*
	 * Type 1 Packet Header Format
	 * The header section is always a 32-bit word.
	 *
	 * HeaderType | Opcode | Register Address | Reserved | Word Count
	 * [31:29]      [28:27]         [26:13]      [12:11]     [10:0]
	 * --------------------------------------------------------------
	 *   001          xx      RRRRRRRRRxxxxx        RR      xxxxxxxxxxx
	 *
	 * @R: means the bit is not used and reserved for future use.
	 * The reserved bits should be written as 0s.
	 *
	 * Generating the Type 1 packet header which involves shifting of Type1
	 * Header Mask, Register value and the OpCode which is 01 in this case
	 * as only read operation is to be carried out and then performing OR
	 * operation with the Word Length.
	 * For more details refer ug470 Packet Types section Table 5-20.
	 */
	return (((1 << TYPE_HDR_SHIFT) |
		(reg << TYPE_REG_SHIFT) |
		(opcode << TYPE_OP_SHIFT)) | size);

}

static int zynq_type2_pkt(u8 opcode, u32 size)
{
	/*
	 * Type 2 Packet Header Format
	 * The header section is always a 32-bit word.
	 *
	 * HeaderType | Opcode |  Word Count
	 * [31:29]      [28:27]         [26:0]
	 * --------------------------------------------------------------
	 *   010          xx      xxxxxxxxxxxxx
	 *
	 * @R: means the bit is not used and reserved for future use.
	 * The reserved bits should be written as 0s.
	 *
	 * Generating the Type 2 packet header which involves shifting of Type 2
	 * Header Mask, OpCode and then performing OR operation with the Word
	 * Length. For more details refer ug470 Packet Types section
	 * Table 5-22.
	 */
	return (((2 << TYPE_HDR_SHIFT) |
		(opcode << TYPE_OP_SHIFT)) | size);
}

static int zynq_fpga_ops_read_image(struct fpga_manager *mgr,
				    struct seq_file *s)
{
	struct zynq_fpga_priv *priv = mgr->priv;
	int ret = 0, cmdindex, clk_rate;
	u32 intr_status, status, i;
	dma_addr_t dma_addr;
	unsigned int *buf;
	size_t size;

	ret = clk_enable(priv->clk);
	if (ret)
		return ret;

	size = priv->size + READ_DMA_SIZE + DUMMY_FRAMES_SIZE;
	buf = dma_zalloc_coherent(mgr->dev.parent, size,
				 &dma_addr, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto disable_clk;
	}

	seq_puts(s, "Zynq FPGA Configuration data contents are\n");

	/*
	 * There is no h/w flow control for pcap read
	 * to prevent the FIFO from over flowing, reduce
	 * the PCAP operating frequency.
	 */
	clk_rate = clk_get_rate(priv->clk);
	ret = clk_set_rate(priv->clk, SLCR_PCAP_FREQ);
	if (ret) {
		dev_err(&mgr->dev, "Unable to reduce the PCAP freq\n");
		goto free_dmabuf;
	}

	ret = zynq_fpga_poll_timeout(priv, STATUS_OFFSET, status,
				     status & STATUS_PCFG_INIT_MASK,
				     INIT_POLL_DELAY,
				     INIT_POLL_TIMEOUT);
	if (ret) {
		dev_err(&mgr->dev, "Timeout waiting for PCFG_INIT\n");
		goto restore_pcap_clk;
	}

	cmdindex = 0;
	buf[cmdindex++] = DUMMY_WORD_MASK;
	buf[cmdindex++] = BUSWIDTH_SYNCWORD_MASK;
	buf[cmdindex++] = BUSWIDTH_DETECT_MASK;
	buf[cmdindex++] = DUMMY_WORD_MASK;
	buf[cmdindex++] = SYNC_WORD_MASK;
	buf[cmdindex++] = NOOP_WORD_MASK;
	buf[cmdindex++] = zynq_type1_pkt(TYPE_CMD_OFFSET, TYPE_OPCODE_WRITE,
					 1);
	buf[cmdindex++] = SHUTDOWN_CMD_MASK;
	buf[cmdindex++] = NOOP_WORD_MASK;
	buf[cmdindex++] = zynq_type1_pkt(TYPE_CMD_OFFSET, TYPE_OPCODE_WRITE,
					 1);
	buf[cmdindex++] = RCRC_CMD_MASK;
	for (i = 0; i < READ_STEP5_NOOPS; i++)
		buf[cmdindex++] = NOOP_WORD_MASK;
	buf[cmdindex++] = zynq_type1_pkt(TYPE_CMD_OFFSET, TYPE_OPCODE_WRITE,
					 1);
	buf[cmdindex++] = RCFG_CMD_MASK;
	buf[cmdindex++] = NOOP_WORD_MASK;
	buf[cmdindex++] = zynq_type1_pkt(TYPE_FAR_OFFSET, TYPE_OPCODE_WRITE,
					 1);
	buf[cmdindex++] = FAR_ADDR_MASK;
	buf[cmdindex++] = zynq_type1_pkt(TYPE_FDRO_OFFSET, TYPE_OPCODE_READ,
					 0);
	buf[cmdindex++] = zynq_type2_pkt(TYPE_OPCODE_READ, priv->size/4);
	for (i = 0; i < READ_STEP9_NOOPS; i++)
		buf[cmdindex++] = NOOP_WORD_MASK;

	intr_status = zynq_fpga_read(priv, INT_STS_OFFSET);
	zynq_fpga_write(priv, INT_STS_OFFSET, intr_status);

	/* Write to PCAP */
	zynq_fpga_dma_xfer(priv, dma_addr, cmdindex,
			   DMA_INVALID_ADDRESS, 0);
	ret = zynq_fpga_wait_fordone(priv);
	if (ret) {
		dev_err(&mgr->dev, "SRCDMA: Timeout waiting for D_P_DONE\n");
		goto restore_pcap_clk;
	}
	intr_status = zynq_fpga_read(priv, INT_STS_OFFSET);
	zynq_fpga_write(priv, INT_STS_OFFSET, intr_status);

	/* Read From PACP */
	zynq_fpga_dma_xfer(priv, DMA_INVALID_ADDRESS, 0,
			   dma_addr + READ_DMA_SIZE, priv->size/4);
	ret = zynq_fpga_wait_fordone(priv);
	if (ret) {
		dev_err(&mgr->dev, "DSTDMA: Timeout waiting for D_P_DONE\n");
		goto restore_pcap_clk;
	}
	intr_status = zynq_fpga_read(priv, INT_STS_OFFSET);
	zynq_fpga_write(priv, INT_STS_OFFSET, intr_status);

	/* Write to PCAP */
	cmdindex = 0;
	buf[cmdindex++] = NOOP_WORD_MASK;
	buf[cmdindex++] = zynq_type1_pkt(TYPE_CMD_OFFSET,
					 TYPE_OPCODE_WRITE, 1);
	buf[cmdindex++] = START_CMD_MASK;
	buf[cmdindex++] = NOOP_WORD_MASK;

	buf[cmdindex++] = zynq_type1_pkt(TYPE_CMD_OFFSET,
					 TYPE_OPCODE_WRITE, 1);
	buf[cmdindex++] = RCRC_CMD_MASK;
	buf[cmdindex++] = NOOP_WORD_MASK;
	buf[cmdindex++] = zynq_type1_pkt(TYPE_CMD_OFFSET,
					 TYPE_OPCODE_WRITE, 1);
	buf[cmdindex++] = DESYNC_WORD_MASK;
	buf[cmdindex++] = NOOP_WORD_MASK;
	buf[cmdindex++] = NOOP_WORD_MASK;

	zynq_fpga_dma_xfer(priv, dma_addr, cmdindex,
			   DMA_INVALID_ADDRESS, 0);
	ret = zynq_fpga_wait_fordone(priv);
	if (ret) {
		dev_err(&mgr->dev, "SRCDMA1: Timeout waiting for D_P_DONE\n");
		goto restore_pcap_clk;
	}

	seq_write(s, &buf[READ_DMA_SIZE/4], priv->size);

restore_pcap_clk:
	clk_set_rate(priv->clk, clk_rate);
free_dmabuf:
	dma_free_coherent(mgr->dev.parent, size, buf,
			  dma_addr);
disable_clk:
	clk_disable(priv->clk);
	return ret;
}

#ifdef CONFIG_FPGA_MGR_DEBUG_FS
#include <linux/debugfs.h>

static int zynq_fpga_getconfigreg(struct fpga_manager *mgr, u8 reg,
				  dma_addr_t dma_addr, int *buf)
{
	struct zynq_fpga_priv *priv = mgr->priv;
	int ret = 0, cmdindex, src_dmaoffset;
	u32 intr_status, status;

	src_dmaoffset = 0x8;
	cmdindex = 2;
	buf[cmdindex++] = DUMMY_WORD_MASK;
	buf[cmdindex++] = BUSWIDTH_SYNCWORD_MASK;
	buf[cmdindex++] = BUSWIDTH_DETECT_MASK;
	buf[cmdindex++] = DUMMY_WORD_MASK;
	buf[cmdindex++] = SYNC_WORD_MASK;
	buf[cmdindex++] = NOOP_WORD_MASK;
	buf[cmdindex++] = zynq_type1_pkt(reg, TYPE_OPCODE_READ, 1);
	buf[cmdindex++] = NOOP_WORD_MASK;
	buf[cmdindex++] = NOOP_WORD_MASK;

	ret = zynq_fpga_poll_timeout(priv, STATUS_OFFSET, status,
				     status & STATUS_PCFG_INIT_MASK,
				     INIT_POLL_DELAY,
				     INIT_POLL_TIMEOUT);
	if (ret) {
		dev_err(&mgr->dev, "Timeout waiting for PCFG_INIT\n");
		goto out;
	}

	/* Write to PCAP */
	intr_status = zynq_fpga_read(priv, INT_STS_OFFSET);
	zynq_fpga_write(priv, INT_STS_OFFSET, IXR_ALL_MASK);

	zynq_fpga_dma_xfer(priv, dma_addr + src_dmaoffset, cmdindex,
			   DMA_INVALID_ADDRESS, 0);
	ret = zynq_fpga_wait_fordone(priv);
	if (ret) {
		dev_err(&mgr->dev, "SRCDMA: Timeout waiting for D_P_DONE\n");
		goto out;
	}
	zynq_fpga_set_irq(priv, intr_status);

	/* Read from PACP */
	zynq_fpga_dma_xfer(priv, DMA_INVALID_ADDRESS, 0, dma_addr, 1);
	ret = zynq_fpga_wait_fordone(priv);
	if (ret) {
		dev_err(&mgr->dev, "DSTDMA: Timeout waiting for D_P_DONE\n");
		goto out;
	}

	/* Write to PCAP */
	cmdindex = 2;
	buf[cmdindex++] = zynq_type1_pkt(TYPE_CMD_OFFSET,
					 TYPE_OPCODE_WRITE, 1);
	buf[cmdindex++] = DESYNC_WORD_MASK;
	buf[cmdindex++] = NOOP_WORD_MASK;
	buf[cmdindex++] = NOOP_WORD_MASK;
	zynq_fpga_dma_xfer(priv, dma_addr + src_dmaoffset, cmdindex,
			   DMA_INVALID_ADDRESS, 0);
	ret = zynq_fpga_wait_fordone(priv);
	if (ret)
		dev_err(&mgr->dev, "SRCDMA1: Timeout waiting for D_P_DONE\n");
out:
	return ret;
}

static int zynq_fpga_read_cfg_reg(struct seq_file *s, void *data)
{
	struct fpga_manager *mgr = (struct fpga_manager *)s->private;
	struct zynq_fpga_priv *priv = mgr->priv;
	struct zynq_configreg *p = cfgreg;
	dma_addr_t dma_addr;
	unsigned int *buf;
	int ret = 0;

	if (!mutex_trylock(&priv->ref_mutex))
		return -EBUSY;

	if (mgr->state != FPGA_MGR_STATE_OPERATING) {
		ret = -EPERM;
		goto err_unlock;
	}

	ret = clk_enable(priv->clk);
	if (ret)
		goto err_unlock;

	buf = dma_zalloc_coherent(mgr->dev.parent, READ_DMA_SIZE,
				 &dma_addr, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto disable_clk;
	}

	seq_puts(s, "Zynq FPGA Configuration register contents are\n");

	while (p->reg) {
		ret = zynq_fpga_getconfigreg(mgr, p->offset, dma_addr, buf);
		if (ret)
			goto free_dmabuf;
		seq_printf(s, "%s --> \t %x \t\r\n", p->reg, buf[0]);
		p++;
	}

free_dmabuf:
	dma_free_coherent(mgr->dev.parent, READ_DMA_SIZE, buf,
			  dma_addr);
disable_clk:
	clk_disable(priv->clk);
err_unlock:
	mutex_unlock(&priv->ref_mutex);
	return ret;
}

static int zynq_fpga_read_open(struct inode *inode, struct file *file)
{
	return single_open(file, zynq_fpga_read_cfg_reg, inode->i_private);
}

static const struct file_operations zynq_fpga_ops_cfg_reg = {
	.owner = THIS_MODULE,
	.open = zynq_fpga_read_open,
	.read = seq_read,
};
#endif

static const struct fpga_manager_ops zynq_fpga_ops = {
	.initial_header_size = 128,
	.state = zynq_fpga_ops_state,
	.write_init = zynq_fpga_ops_write_init,
	.write_sg = zynq_fpga_ops_write,
	.write_complete = zynq_fpga_ops_write_complete,
	.read = zynq_fpga_ops_read_image,
};

static int zynq_fpga_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zynq_fpga_priv *priv;
	struct resource *res;
	int err;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	spin_lock_init(&priv->dma_lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->io_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->io_base))
		return PTR_ERR(priv->io_base);

	priv->slcr = syscon_regmap_lookup_by_phandle(dev->of_node,
		"syscon");
	if (IS_ERR(priv->slcr)) {
		dev_err(dev, "unable to get zynq-slcr regmap\n");
		return PTR_ERR(priv->slcr);
	}

	init_completion(&priv->dma_done);

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0) {
		dev_err(dev, "No IRQ available\n");
		return priv->irq;
	}

	priv->clk = devm_clk_get(dev, "ref_clk");
	if (IS_ERR(priv->clk)) {
		dev_err(dev, "input clock not found\n");
		return PTR_ERR(priv->clk);
	}

	err = clk_prepare_enable(priv->clk);
	if (err) {
		dev_err(dev, "unable to enable clock\n");
		return err;
	}

	/* unlock the device */
	zynq_fpga_write(priv, UNLOCK_OFFSET, UNLOCK_MASK);

	zynq_fpga_set_irq(priv, 0);
	zynq_fpga_write(priv, INT_STS_OFFSET, IXR_ALL_MASK);
	err = devm_request_irq(dev, priv->irq, zynq_fpga_isr, 0, dev_name(dev),
			       priv);
	if (err) {
		dev_err(dev, "unable to request IRQ\n");
		clk_disable_unprepare(priv->clk);
		return err;
	}

	clk_disable(priv->clk);

	err = fpga_mgr_register(dev, "Xilinx Zynq FPGA Manager",
				&zynq_fpga_ops, priv);
	if (err) {
		dev_err(dev, "unable to register FPGA manager\n");
		clk_unprepare(priv->clk);
		return err;
	}

#ifdef CONFIG_FPGA_MGR_DEBUG_FS
	struct dentry *d;
	struct fpga_manager *mgr;

	mgr = platform_get_drvdata(pdev);
	mutex_init(&priv->ref_mutex);

	d = debugfs_create_dir(pdev->dev.kobj.name, mgr->dir);
	if (!d)
		return err;

	priv->dir = d;
	d = debugfs_create_file("cfg_reg", 0644, priv->dir, mgr,
				&zynq_fpga_ops_cfg_reg);
	if (!d) {
		debugfs_remove_recursive(mgr->dir);
		return err;
	}
#endif

	return 0;
}

static int zynq_fpga_remove(struct platform_device *pdev)
{
	struct zynq_fpga_priv *priv;
	struct fpga_manager *mgr;

	mgr = platform_get_drvdata(pdev);
	priv = mgr->priv;

#ifdef CONFIG_FPGA_MGR_DEBUG_FS
	debugfs_remove_recursive(priv->dir);
#endif
	fpga_mgr_unregister(&pdev->dev);

	clk_unprepare(priv->clk);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id zynq_fpga_of_match[] = {
	{ .compatible = "xlnx,zynq-devcfg-1.0", },
	{},
};

MODULE_DEVICE_TABLE(of, zynq_fpga_of_match);
#endif

static struct platform_driver zynq_fpga_driver = {
	.probe = zynq_fpga_probe,
	.remove = zynq_fpga_remove,
	.driver = {
		.name = "zynq_fpga_manager",
		.of_match_table = of_match_ptr(zynq_fpga_of_match),
	},
};

module_platform_driver(zynq_fpga_driver);

MODULE_AUTHOR("Moritz Fischer <moritz.fischer@ettus.com>");
MODULE_AUTHOR("Michal Simek <michal.simek@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Zynq FPGA Manager");
MODULE_LICENSE("GPL v2");
