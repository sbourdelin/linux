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

/* Configuration Registers */
#define TYPE1_CRC_OFFSET	0x0
#define TYPE1_FAR_OFFSET	0x1
#define TYPE1_FDRI_OFFSET	0x2
#define TYPE1_FDRO_OFFSET	0x3
#define TYPE1_CMD_OFFSET	0x4
#define TYPE1_CTRL0_OFFSET	0x5
#define TYPE1_MASK_OFFSET	0x6
#define TYPE1_STAT_OFFSET	0x7
#define TYPE1_LOUT_OFFSET	0x8
#define TYPE1_COR0_OFFSET	0x9
#define TYPE1_MFWR_OFFSET	0xa
#define TYPE1_CBC_OFFSET	0xb
#define TYPE1_IDCODE_OFFSET	0xc
#define TYPE1_AXSS_OFFSET	0xd
#define TYPE1_COR1_OFFSET	0xe
#define TYPE1_WBSTR_OFFSET	0x10
#define TYPE1_TIMER_OFFSET	0x11
#define TYPE1_BOOTSTS_OFFSET	0x16
#define TYPE1_CTRL1_OFFSET	0x18
#define TYPE1_BSPI_OFFSET	0x1f

/* Masks for Configuration registers */
#define RCFG_CMD_MASK		0x00000004
#define START_CMD_MASK		0x00000005
#define RCRC_CMD_MASK		0x00000007
#define SHUTDOWN_CMD_MASK	0x0000000B
#define DESYNC_WORD_MASK	0x0000000D
#define BUSWIDTH_SYNCWORD_MASK	0x000000BB
#define NOOP_WORD_MASK		0x20000000
#define BUSWIDTH_DETECT_MASK	0x11220044
#define SYNC_WORD_MASK		0xAA995566
#define DUMMY_WORD_MASK		0xFFFFFFFF

#define TYPE1_HDR_SHIFT		29
#define TYPE1_REG_SHIFT		13
#define TYPE1_OP_SHIFT		27
#define TYPE1_OPCODE_NOOP	0
#define TYPE1_OPCODE_READ	1
#define TYPE1_OPCODE_WRITE	2

#define CFGREG_SRCDMA_SIZE	0x40

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
	 * Generating the Type 1 packet header which involves sifting of Type1
	 * Header Mask, Register value and the OpCode which is 01 in this case
	 * as only read operation is to be carried out and then performing OR
	 * operation with the Word Length.
	 */
	return (((1 << TYPE1_HDR_SHIFT) |
		(reg << TYPE1_REG_SHIFT) |
		(opcode << TYPE1_OP_SHIFT)) | size);

}

static int zynq_fpga_getconfigreg(struct fpga_manager *mgr, u8 reg)
{
	struct zynq_fpga_priv *priv;
	int ret = 0, cmdindex, src_offset;
	int *srcbuf, *dstbuf;
	dma_addr_t src_dma_addr, dst_dma_addr;
	u32 status, intr_status;

	priv = mgr->priv;

	srcbuf = dma_alloc_coherent(mgr->dev.parent, CFGREG_SRCDMA_SIZE,
				    &src_dma_addr, GFP_KERNEL);
	if (!srcbuf)
		return -ENOMEM;

	dstbuf = dma_alloc_coherent(mgr->dev.parent, sizeof(dstbuf),
				    &dst_dma_addr, GFP_KERNEL);
	if (!dstbuf)
		goto free_srcbuf;

	cmdindex = 0;
	srcbuf[cmdindex++] = DUMMY_WORD_MASK;
	srcbuf[cmdindex++] = BUSWIDTH_SYNCWORD_MASK;
	srcbuf[cmdindex++] = BUSWIDTH_DETECT_MASK;
	srcbuf[cmdindex++] = DUMMY_WORD_MASK;
	srcbuf[cmdindex++] = SYNC_WORD_MASK;
	srcbuf[cmdindex++] = NOOP_WORD_MASK;
	srcbuf[cmdindex++] = zynq_type1_pkt(reg, TYPE1_OPCODE_READ, 1);
	srcbuf[cmdindex++] = NOOP_WORD_MASK;
	srcbuf[cmdindex++] = NOOP_WORD_MASK;

	ret = zynq_fpga_poll_timeout(priv, STATUS_OFFSET, status,
				     status & STATUS_PCFG_INIT_MASK,
				     INIT_POLL_DELAY,
				     INIT_POLL_TIMEOUT);
	if (ret) {
		dev_err(&mgr->dev, "Timeout waiting for PCFG_INIT\n");
		goto free_dstbuf;
	}

	intr_status = zynq_fpga_read(priv, INT_STS_OFFSET);
	zynq_fpga_write(priv, INT_STS_OFFSET, IXR_ALL_MASK);

	zynq_fpga_dma_xfer(priv, src_dma_addr, cmdindex,
			   DMA_INVALID_ADDRESS, 0);
	ret = zynq_fpga_poll_timeout(priv, INT_STS_OFFSET, status,
				     status & IXR_D_P_DONE_MASK,
				     INIT_POLL_DELAY,
				     INIT_POLL_TIMEOUT);
	if (ret) {
		dev_err(&mgr->dev, "Timeout waiting for D_P_DONE\n");
		goto free_dstbuf;
	}
	zynq_fpga_set_irq(priv, intr_status);
	zynq_fpga_dma_xfer(priv, DMA_INVALID_ADDRESS, 0, dst_dma_addr, 1);
	ret = zynq_fpga_poll_timeout(priv, INT_STS_OFFSET, status,
				     status & IXR_DMA_DONE_MASK,
				     INIT_POLL_DELAY,
				     INIT_POLL_TIMEOUT);
	if (ret) {
		dev_err(&mgr->dev, "Timeout waiting for DMA DONE\n");
		goto free_dstbuf;
	}
	ret = zynq_fpga_poll_timeout(priv, INT_STS_OFFSET, status,
				     status & IXR_D_P_DONE_MASK,
				     INIT_POLL_DELAY,
				     INIT_POLL_TIMEOUT);
	if (ret) {
		dev_err(&mgr->dev, "Timeout waiting for D_P_DONE\n");
		goto free_dstbuf;
	}
	src_offset = cmdindex * 4;
	cmdindex = 0;
	srcbuf[cmdindex++] = zynq_type1_pkt(TYPE1_CMD_OFFSET,
					    TYPE1_OPCODE_WRITE, 1);
	srcbuf[cmdindex++] = DESYNC_WORD_MASK;
	srcbuf[cmdindex++] = NOOP_WORD_MASK;
	srcbuf[cmdindex++] = NOOP_WORD_MASK;
	zynq_fpga_dma_xfer(priv, src_dma_addr + src_offset, cmdindex,
			   DMA_INVALID_ADDRESS, 0);
	ret = zynq_fpga_poll_timeout(priv, INT_STS_OFFSET, status,
				     status & IXR_DMA_DONE_MASK,
				     INIT_POLL_DELAY,
				     INIT_POLL_TIMEOUT);
	if (ret) {
		dev_err(&mgr->dev, "Timeout waiting for DMA DONE\n");
		goto free_dstbuf;
	}
	ret = zynq_fpga_poll_timeout(priv, INT_STS_OFFSET, status,
				     status & IXR_D_P_DONE_MASK,
				     INIT_POLL_DELAY,
				     INIT_POLL_TIMEOUT);
	if (ret) {
		dev_err(&mgr->dev, "Timeout waiting for D_P_DONE\n");
		goto free_dstbuf;
	}

	ret = *dstbuf;

free_dstbuf:
	dma_free_coherent(mgr->dev.parent, sizeof(dstbuf), dstbuf,
			  dst_dma_addr);
free_srcbuf:
	dma_free_coherent(mgr->dev.parent, CFGREG_SRCDMA_SIZE, srcbuf,
			  src_dma_addr);

	return ret;
}

static int zynq_fpga_ops_read(struct fpga_manager *mgr, struct seq_file *s)
{
	struct zynq_fpga_priv *priv;
	int err;

	priv = mgr->priv;

	err = clk_enable(priv->clk);
	if (err)
		return err;

	seq_puts(s, "zynq FPGA Configuration register contents are\n");
	seq_printf(s, "CRC --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_CRC_OFFSET)));
	seq_printf(s, "FAR --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_FAR_OFFSET)));
	seq_printf(s, "FDRI --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_FDRI_OFFSET)));
	seq_printf(s, "FDRO --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_FDRO_OFFSET)));
	seq_printf(s, "CMD --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_CMD_OFFSET)));
	seq_printf(s, "CTRL0 --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_CTRL0_OFFSET)));
	seq_printf(s, "MASK --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_MASK_OFFSET)));
	seq_printf(s, "STAT --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_STAT_OFFSET)));
	seq_printf(s, "LOUT --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_LOUT_OFFSET)));
	seq_printf(s, "COR0 --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_COR0_OFFSET)));
	seq_printf(s, "MFWR --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_MFWR_OFFSET)));
	seq_printf(s, "CBC --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_CBC_OFFSET)));
	seq_printf(s, "IDCODE --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_IDCODE_OFFSET)));
	seq_printf(s, "AXSS --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_AXSS_OFFSET)));
	seq_printf(s, "COR1 --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_COR1_OFFSET)));
	seq_printf(s, "WBSTR --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_WBSTR_OFFSET)));
	seq_printf(s, "TIMER --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_TIMER_OFFSET)));
	seq_printf(s, "BOOTSTS --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_BOOTSTS_OFFSET)));
	seq_printf(s, "CTRL1 --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_CTRL1_OFFSET)));
	seq_printf(s, "BSPI --> \t %x \t\r\n",
		   (zynq_fpga_getconfigreg(mgr, TYPE1_BSPI_OFFSET)));

	clk_disable(priv->clk);

	return 0;
}

static const struct fpga_manager_ops zynq_fpga_ops = {
	.initial_header_size = 128,
	.state = zynq_fpga_ops_state,
	.write_init = zynq_fpga_ops_write_init,
	.write_sg = zynq_fpga_ops_write,
	.write_complete = zynq_fpga_ops_write_complete,
	.read = zynq_fpga_ops_read,
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

	return 0;
}

static int zynq_fpga_remove(struct platform_device *pdev)
{
	struct zynq_fpga_priv *priv;
	struct fpga_manager *mgr;

	mgr = platform_get_drvdata(pdev);
	priv = mgr->priv;

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
