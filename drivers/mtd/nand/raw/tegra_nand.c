// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Stefan Agner <stefan@agner.ch>
 * Copyright (C) 2014-2015 Lucas Stach <dev@lynxeye.de>
 * Copyright (C) 2012 Avionic Design GmbH
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/rawnand.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#define CMD					0x00
#define   CMD_GO				(1 << 31)
#define   CMD_CLE				(1 << 30)
#define   CMD_ALE				(1 << 29)
#define   CMD_PIO				(1 << 28)
#define   CMD_TX				(1 << 27)
#define   CMD_RX				(1 << 26)
#define   CMD_SEC_CMD				(1 << 25)
#define   CMD_AFT_DAT				(1 << 24)
#define   CMD_TRANS_SIZE(x)			(((x - 1) & 0xf) << 20)
#define   CMD_A_VALID				(1 << 19)
#define   CMD_B_VALID				(1 << 18)
#define   CMD_RD_STATUS_CHK			(1 << 17)
#define   CMD_RBSY_CHK				(1 << 16)
#define   CMD_CE(x)				(1 << (8 + ((x) & 0x7)))
#define   CMD_CLE_SIZE(x)			(((x - 1) & 0x3) << 4)
#define   CMD_ALE_SIZE(x)			(((x - 1) & 0xf) << 0)

#define STATUS					0x04

#define ISR					0x08
#define   ISR_CORRFAIL_ERR			(1 << 24)
#define   ISR_UND				(1 << 7)
#define   ISR_OVR				(1 << 6)
#define   ISR_CMD_DONE				(1 << 5)
#define   ISR_ECC_ERR				(1 << 4)

#define IER					0x0c
#define   IER_ERR_TRIG_VAL(x)			(((x) & 0xf) << 16)
#define   IER_UND				(1 << 7)
#define   IER_OVR				(1 << 6)
#define   IER_CMD_DONE				(1 << 5)
#define   IER_ECC_ERR				(1 << 4)
#define   IER_GIE				(1 << 0)

#define CFG					0x10
#define   CFG_HW_ECC				(1 << 31)
#define   CFG_ECC_SEL				(1 << 30)
#define   CFG_ERR_COR				(1 << 29)
#define   CFG_PIPE_EN				(1 << 28)
#define   CFG_TVAL_4				(0 << 24)
#define   CFG_TVAL_6				(1 << 24)
#define   CFG_TVAL_8				(2 << 24)
#define   CFG_SKIP_SPARE			(1 << 23)
#define   CFG_BUS_WIDTH_8			(0 << 21)
#define   CFG_BUS_WIDTH_16			(1 << 21)
#define   CFG_COM_BSY				(1 << 20)
#define   CFG_PS_256				(0 << 16)
#define   CFG_PS_512				(1 << 16)
#define   CFG_PS_1024				(2 << 16)
#define   CFG_PS_2048				(3 << 16)
#define   CFG_PS_4096				(4 << 16)
#define   CFG_SKIP_SPARE_SIZE_4			(0 << 14)
#define   CFG_SKIP_SPARE_SIZE_8			(1 << 14)
#define   CFG_SKIP_SPARE_SIZE_12		(2 << 14)
#define   CFG_SKIP_SPARE_SIZE_16		(3 << 14)
#define   CFG_TAG_BYTE_SIZE(x)			((x) & 0xff)

#define TIMING_1				0x14
#define   TIMING_TRP_RESP(x)			(((x) & 0xf) << 28)
#define   TIMING_TWB(x)				(((x) & 0xf) << 24)
#define   TIMING_TCR_TAR_TRR(x)			(((x) & 0xf) << 20)
#define   TIMING_TWHR(x)			(((x) & 0xf) << 16)
#define   TIMING_TCS(x)				(((x) & 0x3) << 14)
#define   TIMING_TWH(x)				(((x) & 0x3) << 12)
#define   TIMING_TWP(x)				(((x) & 0xf) <<  8)
#define   TIMING_TRH(x)				(((x) & 0xf) <<  4)
#define   TIMING_TRP(x)				(((x) & 0xf) <<  0)

#define RESP					0x18

#define TIMING_2				0x1c
#define   TIMING_TADL(x)			((x) & 0xf)

#define CMD_1					0x20
#define CMD_2					0x24
#define ADDR_1					0x28
#define ADDR_2					0x2c

#define DMA_CTRL				0x30
#define   DMA_CTRL_GO				(1 << 31)
#define   DMA_CTRL_IN				(0 << 30)
#define   DMA_CTRL_OUT				(1 << 30)
#define   DMA_CTRL_PERF_EN			(1 << 29)
#define   DMA_CTRL_IE_DONE			(1 << 28)
#define   DMA_CTRL_REUSE			(1 << 27)
#define   DMA_CTRL_BURST_1			(2 << 24)
#define   DMA_CTRL_BURST_4			(3 << 24)
#define   DMA_CTRL_BURST_8			(4 << 24)
#define   DMA_CTRL_BURST_16			(5 << 24)
#define   DMA_CTRL_IS_DONE			(1 << 20)
#define   DMA_CTRL_EN_A				(1 <<  2)
#define   DMA_CTRL_EN_B				(1 <<  1)

#define DMA_CFG_A				0x34
#define DMA_CFG_B				0x38

#define FIFO_CTRL				0x3c
#define   FIFO_CTRL_CLR_ALL			(1 << 3)

#define DATA_PTR				0x40
#define TAG_PTR					0x44
#define ECC_PTR					0x48

#define DEC_STATUS				0x4c
#define   DEC_STATUS_A_ECC_FAIL			(1 << 1)
#define   DEC_STATUS_ERR_COUNT_MASK		0x00ff0000
#define   DEC_STATUS_ERR_COUNT_SHIFT		16

#define HWSTATUS_CMD				0x50
#define HWSTATUS_MASK				0x54
#define   HWSTATUS_RDSTATUS_MASK(x)		(((x) & 0xff) << 24)
#define   HWSTATUS_RDSTATUS_VALUE(x)		(((x) & 0xff) << 16)
#define   HWSTATUS_RBSY_MASK(x)			(((x) & 0xff) << 8)
#define   HWSTATUS_RBSY_VALUE(x)		(((x) & 0xff) << 0)

#define BCH_CONFIG				0xcc
#define   BCH_ENABLE				(1 << 0)
#define   BCH_TVAL_4				(0 << 4)
#define   BCH_TVAL_8				(1 << 4)
#define   BCH_TVAL_14				(2 << 4)
#define   BCH_TVAL_16				(3 << 4)

#define DEC_STAT_RESULT				0xd0
#define DEC_STAT_BUF				0xd4
#define   DEC_STAT_BUF_FAIL_SEC_FLAG_MASK	0xff000000
#define   DEC_STAT_BUF_FAIL_SEC_FLAG_SHIFT	24
#define   DEC_STAT_BUF_CORR_SEC_FLAG_MASK	0x00ff0000
#define   DEC_STAT_BUF_CORR_SEC_FLAG_SHIFT	16
#define   DEC_STAT_BUF_MAX_CORR_CNT_MASK	0x00001f00
#define   DEC_STAT_BUF_MAX_CORR_CNT_SHIFT	8

#define SKIP_SPARE_BYTES	4
#define BITS_PER_STEP_RS	18
#define BITS_PER_STEP_BCH	13

struct tegra_nand_controller {
	struct nand_hw_control controller;
	void __iomem *regs;
	struct clk *clk;
	struct device *dev;
	struct completion command_complete;
	struct completion dma_complete;
	bool last_read_error;
	int cur_chip;
	struct nand_chip *chip;
};

struct tegra_nand_chip {
	struct nand_chip chip;
	struct gpio_desc *wp_gpio;
};

static inline struct tegra_nand_controller *to_tegra_ctrl(
						struct nand_hw_control *hw_ctrl)
{
	return container_of(hw_ctrl, struct tegra_nand_controller, controller);
}

static int tegra_nand_ooblayout_rs_ecc(struct mtd_info *mtd, int section,
				       struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	int bytes_per_step = (BITS_PER_STEP_RS * chip->ecc.strength) / 8;

	if (section > 0)
		return -ERANGE;

	oobregion->offset = SKIP_SPARE_BYTES;
	oobregion->length = round_up(bytes_per_step * chip->ecc.steps, 4);

	return 0;
}

static int tegra_nand_ooblayout_rs_free(struct mtd_info *mtd, int section,
					struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	int bytes_per_step = DIV_ROUND_UP(BITS_PER_STEP_RS * chip->ecc.strength, 8);

	if (section > 0)
		return -ERANGE;

	oobregion->offset = SKIP_SPARE_BYTES +
			    round_up(bytes_per_step * chip->ecc.steps, 4);
	oobregion->length = mtd->oobsize - oobregion->offset;

	return 0;
}

static const struct mtd_ooblayout_ops tegra_nand_oob_rs_ops = {
	.ecc = tegra_nand_ooblayout_rs_ecc,
	.free = tegra_nand_ooblayout_rs_free,
};

static int tegra_nand_ooblayout_bch_ecc(struct mtd_info *mtd, int section,
				       struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	int bytes_per_step = DIV_ROUND_UP(BITS_PER_STEP_BCH * chip->ecc.strength, 8);

	if (section > 0)
		return -ERANGE;

	oobregion->offset = SKIP_SPARE_BYTES;
	oobregion->length = round_up(bytes_per_step * chip->ecc.steps, 4);

	return 0;
}

static int tegra_nand_ooblayout_bch_free(struct mtd_info *mtd, int section,
					struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	int bytes_per_step = (BITS_PER_STEP_BCH * chip->ecc.strength) / 8;

	if (section > 0)
		return -ERANGE;

	oobregion->offset = SKIP_SPARE_BYTES +
			    round_up(bytes_per_step * chip->ecc.steps, 4);
	oobregion->length = mtd->oobsize - oobregion->offset;

	return 0;
}

/*
 * Layout with tag bytes is
 *
 * --------------------------------------------------------------------------
 * | main area                       | skip bytes | tag bytes | parity | .. |
 * --------------------------------------------------------------------------
 *
 * If not tag bytes are written, parity moves right after skip bytes!
 */
static const struct mtd_ooblayout_ops tegra_nand_oob_bch_ops = {
	.ecc = tegra_nand_ooblayout_bch_ecc,
	.free = tegra_nand_ooblayout_bch_free,
};

static irqreturn_t tegra_nand_irq(int irq, void *data)
{
	struct tegra_nand_controller *ctrl = data;
	u32 isr, dma;

	isr = readl_relaxed(ctrl->regs + ISR);
	dma = readl_relaxed(ctrl->regs + DMA_CTRL);
	dev_dbg(ctrl->dev, "isr %08x\n", isr);

	if (!isr && !(dma & DMA_CTRL_IS_DONE))
		return IRQ_NONE;

	if (isr & ISR_CORRFAIL_ERR)
		ctrl->last_read_error = true;

	if (isr & ISR_CMD_DONE)
		complete(&ctrl->command_complete);

	if (isr & ISR_UND)
		dev_dbg(ctrl->dev, "FIFO underrun\n");

	if (isr & ISR_OVR)
		dev_dbg(ctrl->dev, "FIFO overrun\n");

	/* handle DMA interrupts */
	if (dma & DMA_CTRL_IS_DONE) {
		writel(dma, ctrl->regs + DMA_CTRL);
		complete(&ctrl->dma_complete);
	}

	/* clear interrupts */
	writel(isr, ctrl->regs + ISR);

	return IRQ_HANDLED;
}

static int tegra_nand_cmd(struct nand_chip *chip,
			 const struct nand_subop *subop)
{
	const struct nand_op_instr *instr;
	const struct nand_op_instr *instr_data_in = NULL;
	struct tegra_nand_controller *ctrl = to_tegra_ctrl(chip->controller);
	unsigned int op_id = -1, trfr_in_sz = 0, trfr_out_sz = 0, offset = 0;
	bool first_cmd = true;
	u32 cmd = 0;
	u32 value;

	for (op_id = 0; op_id < subop->ninstrs; op_id++) {
		unsigned int naddrs, i;
		const u8 *addrs;
		u32 addr1 = 0, addr2 = 0;

		instr = &subop->instrs[op_id];

		switch (instr->type) {
		case NAND_OP_CMD_INSTR:
			if (first_cmd) {
				cmd |= CMD_CLE;
				writel(instr->ctx.cmd.opcode, ctrl->regs + CMD_1);
			} else {
				cmd |= CMD_SEC_CMD;
				writel(instr->ctx.cmd.opcode, ctrl->regs + CMD_2);
			}
			first_cmd = false;
			break;
		case NAND_OP_ADDR_INSTR:
			offset = nand_subop_get_addr_start_off(subop, op_id);
			naddrs = nand_subop_get_num_addr_cyc(subop, op_id);
			addrs = &instr->ctx.addr.addrs[offset];

			cmd |= CMD_ALE | CMD_ALE_SIZE(naddrs);
			for (i = 0; i < min_t(unsigned int, 4, naddrs); i++)
				addr1 |= *addrs++ << (8 * i);
			naddrs -= i;
			for (i = 0; i < min_t(unsigned int, 4, naddrs); i++)
				addr2 |= *addrs++ << (8 * i);
			writel(addr1, ctrl->regs + ADDR_1);
			writel(addr2, ctrl->regs + ADDR_2);
			break;

		case NAND_OP_DATA_IN_INSTR:
			trfr_in_sz = nand_subop_get_data_len(subop, op_id);
			offset = nand_subop_get_data_start_off(subop, op_id);

			cmd |= CMD_TRANS_SIZE(trfr_in_sz) | CMD_PIO | CMD_RX | CMD_A_VALID;

			instr_data_in = instr;
			break;

		case NAND_OP_DATA_OUT_INSTR:
			trfr_out_sz = nand_subop_get_data_len(subop, op_id);
			offset = nand_subop_get_data_start_off(subop, op_id);
			trfr_out_sz = min_t(size_t, trfr_out_sz, 4);

			cmd |= CMD_TRANS_SIZE(trfr_out_sz) | CMD_PIO | CMD_TX | CMD_A_VALID;

			memcpy(&value, instr->ctx.data.buf.out + offset, trfr_out_sz);
			writel(value, ctrl->regs + RESP);

			break;
		case NAND_OP_WAITRDY_INSTR:
			cmd |= CMD_RBSY_CHK;
			break;

		}
	}


	cmd |= CMD_GO | CMD_CE(ctrl->cur_chip);
	writel(cmd, ctrl->regs + CMD);
	wait_for_completion(&ctrl->command_complete);

	if (instr_data_in) {
		u32 value;
		size_t n = min_t(size_t, trfr_in_sz, 4);

		value = readl(ctrl->regs + RESP);
		memcpy(instr_data_in->ctx.data.buf.in + offset, &value, n);
	}

	return 0;
}

static const struct nand_op_parser tegra_nand_op_parser = NAND_OP_PARSER(
	NAND_OP_PARSER_PATTERN(tegra_nand_cmd,
		NAND_OP_PARSER_PAT_CMD_ELEM(true),
		NAND_OP_PARSER_PAT_ADDR_ELEM(true, 8),
		NAND_OP_PARSER_PAT_CMD_ELEM(true),
		NAND_OP_PARSER_PAT_WAITRDY_ELEM(true)),
	NAND_OP_PARSER_PATTERN(tegra_nand_cmd,
		NAND_OP_PARSER_PAT_DATA_OUT_ELEM(false, 4)),
	NAND_OP_PARSER_PATTERN(tegra_nand_cmd,
		NAND_OP_PARSER_PAT_CMD_ELEM(true),
		NAND_OP_PARSER_PAT_ADDR_ELEM(true, 8),
		NAND_OP_PARSER_PAT_CMD_ELEM(true),
		NAND_OP_PARSER_PAT_WAITRDY_ELEM(true),
		NAND_OP_PARSER_PAT_DATA_IN_ELEM(true, 4)),
	);

static int tegra_nand_exec_op(struct nand_chip *chip,
			     const struct nand_operation *op,
			     bool check_only)
{
	return nand_op_parser_exec_op(chip, &tegra_nand_op_parser, op,
				      check_only);
}
static void tegra_nand_select_chip(struct mtd_info *mtd, int chip_nr)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct tegra_nand_controller *ctrl = to_tegra_ctrl(chip->controller);

	ctrl->cur_chip = chip_nr;
}

static u32 tegra_nand_fill_address(struct tegra_nand_controller *ctrl,
				   struct nand_chip *chip, int page)
{
	/* Lower 16-bits are column, always 0 */
	writel(page << 16, ctrl->regs + ADDR_1);

	if (chip->options & NAND_ROW_ADDR_3) {
		writel(page >> 16, ctrl->regs + ADDR_2);
		return 5;
	}

	return 4;
}

static void tegra_nand_hw_ecc(struct tegra_nand_controller *ctrl,
			      struct nand_chip *chip, bool enable)
{
	u32 value;

	switch (chip->ecc.algo) {
	case NAND_ECC_RS:
		value = readl(ctrl->regs + CFG);
		if (enable)
			value |= CFG_HW_ECC | CFG_ERR_COR;
		else
			value &= ~(CFG_HW_ECC | CFG_ERR_COR);
		writel(value, ctrl->regs + CFG);
		break;
	case NAND_ECC_BCH:
		value = readl(ctrl->regs + BCH_CONFIG);
		if (enable)
			value |= BCH_ENABLE;
		else
			value &= ~BCH_ENABLE;
		writel(value, ctrl->regs + BCH_CONFIG);
		break;
	default:
		dev_err(ctrl->dev, "Unsupported hardware ECC algorithm\n");
		break;
	}
}

static int tegra_nand_read_page(struct mtd_info *mtd, struct nand_chip *chip,
				uint8_t *buf, int oob_required, int page)
{
	struct tegra_nand_controller *ctrl = to_tegra_ctrl(chip->controller);
	dma_addr_t dma_addr;
	u32 value, addrs;
	int ret, dma_len;

	writel(NAND_CMD_READ0, ctrl->regs + CMD_1);
	writel(NAND_CMD_READSTART, ctrl->regs + CMD_2);

	addrs = tegra_nand_fill_address(ctrl, chip, page);

	dma_len = mtd->writesize + (oob_required ? mtd->oobsize : 0);
	dma_addr = dma_map_single(ctrl->dev, buf, dma_len, DMA_FROM_DEVICE);
	ret = dma_mapping_error(ctrl->dev, dma_addr);
	if (ret) {
		dev_err(ctrl->dev, "dma mapping error\n");
		return -EINVAL;
	}

	writel(mtd->writesize - 1, ctrl->regs + DMA_CFG_A);
	writel(dma_addr, ctrl->regs + DATA_PTR);

	if (oob_required) {
		struct mtd_oob_region oobregion;
		dma_addr_t dma_addr_oob = dma_addr + mtd->writesize;

		mtd_ooblayout_free(mtd, 0, &oobregion);

		writel(oobregion.length - 1, ctrl->regs + DMA_CFG_B);
		writel(dma_addr_oob + oobregion.offset, ctrl->regs + TAG_PTR);
	} else {
		writel(0, ctrl->regs + DMA_CFG_B);
		writel(0, ctrl->regs + TAG_PTR);
	}

	value = DMA_CTRL_GO | DMA_CTRL_IN | DMA_CTRL_PERF_EN |
		DMA_CTRL_REUSE | DMA_CTRL_IE_DONE | DMA_CTRL_IS_DONE |
		DMA_CTRL_BURST_16 | DMA_CTRL_EN_A;
	if (oob_required)
		value |= DMA_CTRL_EN_B;
	writel(value, ctrl->regs + DMA_CTRL);

	value = CMD_CLE | CMD_ALE | CMD_ALE_SIZE(addrs) | CMD_SEC_CMD |
		CMD_RBSY_CHK | CMD_GO | CMD_RX | CMD_TRANS_SIZE(9) |
		CMD_A_VALID | CMD_CE(ctrl->cur_chip);
	if (oob_required)
		value |= CMD_B_VALID;
	writel(value, ctrl->regs + CMD);

	wait_for_completion(&ctrl->command_complete);
	wait_for_completion(&ctrl->dma_complete);

	dma_unmap_single(ctrl->dev, dma_addr, dma_len, DMA_FROM_DEVICE);

	return 0;
}

static int tegra_nand_read_page_hwecc(struct mtd_info *mtd,
				      struct nand_chip *chip,
				      uint8_t *buf, int oob_required, int page)
{
	struct tegra_nand_controller *ctrl = to_tegra_ctrl(chip->controller);
	u32 value;
	int ret;

	tegra_nand_hw_ecc(ctrl, chip, true);
	ret = tegra_nand_read_page(mtd, chip, buf, oob_required, page);
	tegra_nand_hw_ecc(ctrl, chip, false);
	if (ret)
		return ret;

	/* If no correctable or un-correctable errors occured we can return 0 */
	if (!ctrl->last_read_error)
		return 0;

	/*
	 * Correctable or un-correctable errors did occure. NAND dec status
	 * contains information for all ECC selections
	 */
	ctrl->last_read_error = false;
	value = readl(ctrl->regs + DEC_STAT_BUF);

	if (value & DEC_STAT_BUF_FAIL_SEC_FLAG_MASK) {
		/*
		 * The ECC isn't smart enough to figure out if a page is
		 * completely erased and flags an error in this case. So we
		 * check the read data here to figure out if it's a legitimate
		 * error or a false positive.
		 */
		int i, ret;
		int flips_threshold = chip->ecc.strength / 2;
		int max_bitflips = 0;

		for (i = 0; i < chip->ecc.steps; i++) {
			u8 *data = buf + (chip->ecc.size * i);

			ret = nand_check_erased_ecc_chunk(data, chip->ecc.size,
							  NULL, 0,
							  NULL, 0,
							  flips_threshold);
			if (ret < 0)
				mtd->ecc_stats.failed++;
			else
				max_bitflips = max(ret, max_bitflips);
		}

		return max_bitflips;
	} else {
		int max_corr_cnt, corr_sec_flag;

		corr_sec_flag = (value & DEC_STAT_BUF_CORR_SEC_FLAG_MASK) >>
				DEC_STAT_BUF_CORR_SEC_FLAG_SHIFT;
		max_corr_cnt = (value & DEC_STAT_BUF_MAX_CORR_CNT_MASK) >>
			       DEC_STAT_BUF_MAX_CORR_CNT_SHIFT;

		/*
		 * The value returned in the register is the maximum of
		 * bitflips encountered in any of the ECC regions. As there is
		 * no way to get the number of bitflips in a specific regions
		 * we are not able to deliver correct stats but instead
		 * overestimate the number of corrected bitflips by assuming
		 * that all regions where errors have been corrected
		 * encountered the maximum number of bitflips.
		 */
		mtd->ecc_stats.corrected += max_corr_cnt * hweight8(corr_sec_flag);

		return max_corr_cnt;
	}

}

static int tegra_nand_write_page(struct mtd_info *mtd, struct nand_chip *chip,
				 const uint8_t *buf, int oob_required, int page)
{
	struct tegra_nand_controller *ctrl = to_tegra_ctrl(chip->controller);
	dma_addr_t dma_addr;
	u32 value, addrs;
	int ret, dma_len;

	writel(NAND_CMD_SEQIN, ctrl->regs + CMD_1);
	writel(NAND_CMD_PAGEPROG, ctrl->regs + CMD_2);

	addrs = tegra_nand_fill_address(ctrl, chip, page);

	dma_len = mtd->writesize + (oob_required ? mtd->oobsize : 0);
	dma_addr = dma_map_single(ctrl->dev, (void *)buf, dma_len, DMA_TO_DEVICE);
	ret = dma_mapping_error(ctrl->dev, dma_addr);
	if (ret) {
		dev_err(ctrl->dev, "dma mapping error\n");
		return -EINVAL;
	}

	writel(mtd->writesize - 1, ctrl->regs + DMA_CFG_A);
	writel(dma_addr, ctrl->regs + DATA_PTR);

	if (oob_required) {
		struct mtd_oob_region oobregion;
		dma_addr_t dma_addr_oob = dma_addr + mtd->writesize;

		mtd_ooblayout_free(mtd, 0, &oobregion);

		writel(oobregion.length - 1, ctrl->regs + DMA_CFG_B);
		writel(dma_addr_oob + oobregion.offset, ctrl->regs + TAG_PTR);
	} else {
		writel(0, ctrl->regs + DMA_CFG_B);
		writel(0, ctrl->regs + TAG_PTR);
	}

	value = DMA_CTRL_GO | DMA_CTRL_OUT | DMA_CTRL_PERF_EN |
		DMA_CTRL_IE_DONE | DMA_CTRL_IS_DONE |
		DMA_CTRL_BURST_16 | DMA_CTRL_EN_A;
	if (oob_required)
		value |= DMA_CTRL_EN_B;
	writel(value, ctrl->regs + DMA_CTRL);

	value = CMD_CLE | CMD_ALE | CMD_ALE_SIZE(addrs) | CMD_SEC_CMD |
		CMD_AFT_DAT | CMD_RBSY_CHK | CMD_GO | CMD_TX | CMD_A_VALID |
		CMD_TRANS_SIZE(9) | CMD_CE(ctrl->cur_chip);
	if (oob_required)
		value |= CMD_B_VALID;
	writel(value, ctrl->regs + CMD);

	wait_for_completion(&ctrl->command_complete);
	wait_for_completion(&ctrl->dma_complete);


	dma_unmap_single(ctrl->dev, dma_addr, dma_len, DMA_TO_DEVICE);

	return 0;
}

static int tegra_nand_write_page_hwecc(struct mtd_info *mtd,
				       struct nand_chip *chip,
				       const uint8_t *buf, int oob_required,
				       int page)
{
	struct tegra_nand_controller *ctrl = to_tegra_ctrl(chip->controller);
	int ret;

	tegra_nand_hw_ecc(ctrl, chip, true);
	ret = tegra_nand_write_page(mtd, chip, buf, oob_required, page);
	tegra_nand_hw_ecc(ctrl, chip, false);

	return ret;
}

static void tegra_nand_setup_timing(struct tegra_nand_controller *ctrl,
				    const struct nand_sdr_timings *timings)
{
	/*
	 * The period (and all other timings in this function) is in ps,
	 * so need to take care here to avoid integer overflows.
	 */
	unsigned int rate = clk_get_rate(ctrl->clk) / 1000000;
	unsigned int period = DIV_ROUND_UP(1000000, rate);
	u32 val, reg = 0;

	val = DIV_ROUND_UP(max3(timings->tAR_min, timings->tRR_min,
				timings->tRC_min), period);
	if (val > 2)
		val -= 3;
	reg |= TIMING_TCR_TAR_TRR(val);

	val = DIV_ROUND_UP(max(max(timings->tCS_min, timings->tCH_min),
				   max(timings->tALS_min, timings->tALH_min)),
			   period);
	if (val > 1)
		val -= 2;
	reg |= TIMING_TCS(val);

	val = DIV_ROUND_UP(max(timings->tRP_min, timings->tREA_max) + 6000,
			   period);
	reg |= TIMING_TRP(val) | TIMING_TRP_RESP(val);

	reg |= TIMING_TWB(DIV_ROUND_UP(timings->tWB_max, period));
	reg |= TIMING_TWHR(DIV_ROUND_UP(timings->tWHR_min, period));
	reg |= TIMING_TWH(DIV_ROUND_UP(timings->tWH_min, period));
	reg |= TIMING_TWP(DIV_ROUND_UP(timings->tWP_min, period));
	reg |= TIMING_TRH(DIV_ROUND_UP(timings->tRHW_min, period));

	writel(reg, ctrl->regs + TIMING_1);

	val = DIV_ROUND_UP(timings->tADL_min, period);
	if (val > 2)
		val -= 3;
	reg = TIMING_TADL(val);

	writel(reg, ctrl->regs + TIMING_2);
}

static int tegra_nand_setup_data_interface(struct mtd_info *mtd, int csline,
					   const struct nand_data_interface *conf)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct tegra_nand_controller *ctrl = to_tegra_ctrl(chip->controller);
	const struct nand_sdr_timings *timings;

	timings = nand_get_sdr_timings(conf);
	if (IS_ERR(timings))
		return PTR_ERR(timings);

	if (csline == NAND_DATA_IFACE_CHECK_ONLY)
		return 0;

	tegra_nand_setup_timing(ctrl, timings);

	return 0;
}

static int tegra_nand_chips_init(struct device *dev,
				 struct tegra_nand_controller *ctrl)
{
	struct device_node *np = dev->of_node;
	struct device_node *np_nand;
	int nchips = of_get_child_count(np);
	struct tegra_nand_chip *nand;
	struct mtd_info *mtd;
	struct nand_chip *chip;
	unsigned long config, bch_config = 0;
	int bits_per_step;
	int err;

	if (nchips != 1) {
		dev_err(dev, "currently only one NAND chip supported\n");
		return -EINVAL;
	}

	np_nand = of_get_next_child(np, NULL);

	nand = devm_kzalloc(dev, sizeof(*nand), GFP_KERNEL);
	if (!nand) {
		dev_err(dev, "could not allocate chip structure\n");
		return -ENOMEM;
	}

	nand->wp_gpio = devm_gpiod_get_optional(dev, "wp", GPIOD_OUT_LOW);

	if (IS_ERR(nand->wp_gpio)) {
		err = PTR_ERR(nand->wp_gpio);
		dev_err(dev, "failed to request WP GPIO: %d\n", err);
		return err;
	}

	chip = &nand->chip;
	chip->controller = &ctrl->controller;
	ctrl->chip = chip;

	mtd = nand_to_mtd(chip);

	mtd->dev.parent = dev;
	mtd->name = "tegra_nand";
	mtd->owner = THIS_MODULE;

	nand_set_flash_node(chip, np_nand);

	chip->options = NAND_NO_SUBPAGE_WRITE | NAND_USE_BOUNCE_BUFFER;
	chip->exec_op = tegra_nand_exec_op;
	chip->select_chip = tegra_nand_select_chip;
	chip->setup_data_interface = tegra_nand_setup_data_interface;

	err = nand_scan_ident(mtd, 1, NULL);
	if (err)
		return err;

	if (chip->bbt_options & NAND_BBT_USE_FLASH)
		chip->bbt_options |= NAND_BBT_NO_OOB;

	chip->ecc.mode = NAND_ECC_HW;
	if (!chip->ecc.size)
		chip->ecc.size = 512;
	if (chip->ecc.size != 512)
		return -EINVAL;

	chip->ecc.read_page = tegra_nand_read_page_hwecc;
	chip->ecc.write_page = tegra_nand_write_page_hwecc;
	/* Not functional for unknown reason...
	chip->ecc.read_page_raw = tegra_nand_read_page;
	chip->ecc.write_page_raw = tegra_nand_write_page;
	*/
	config = readl(ctrl->regs + CFG);
	config |= CFG_PIPE_EN | CFG_SKIP_SPARE | CFG_SKIP_SPARE_SIZE_4;

	if (chip->options & NAND_BUSWIDTH_16)
		config |= CFG_BUS_WIDTH_16;

	switch (chip->ecc.algo) {
	case NAND_ECC_RS:
		bits_per_step = BITS_PER_STEP_RS * chip->ecc.strength;
		mtd_set_ooblayout(mtd, &tegra_nand_oob_rs_ops);
		switch (chip->ecc.strength) {
		case 4:
			config |= CFG_ECC_SEL | CFG_TVAL_4;
			break;
		case 6:
			config |= CFG_ECC_SEL | CFG_TVAL_6;
			break;
		case 8:
			config |= CFG_ECC_SEL | CFG_TVAL_8;
			break;
		default:
			dev_err(dev, "ECC strength %d not supported\n",
				chip->ecc.strength);
			return -EINVAL;
		}
		break;
	case NAND_ECC_BCH:
		bits_per_step = BITS_PER_STEP_BCH * chip->ecc.strength;
		mtd_set_ooblayout(mtd, &tegra_nand_oob_bch_ops);
		switch (chip->ecc.strength) {
		case 4:
			bch_config = BCH_TVAL_4;
			break;
		case 8:
			bch_config = BCH_TVAL_8;
			break;
		case 14:
			bch_config = BCH_TVAL_14;
			break;
		case 16:
			bch_config = BCH_TVAL_16;
			break;
		default:
			dev_err(dev, "ECC strength %d not supported\n",
				chip->ecc.strength);
			return -EINVAL;
		}
		break;
	default:
		dev_err(dev, "ECC algorithm not supported\n");
		return -EINVAL;
	}

	chip->ecc.bytes = DIV_ROUND_UP(bits_per_step, 8);

	switch (mtd->writesize) {
	case 256:
		config |= CFG_PS_256;
		break;
	case 512:
		config |= CFG_PS_512;
		break;
	case 1024:
		config |= CFG_PS_1024;
		break;
	case 2048:
		config |= CFG_PS_2048;
		break;
	case 4096:
		config |= CFG_PS_4096;
		break;
	default:
		dev_err(dev, "unhandled writesize %d\n", mtd->writesize);
		return -ENODEV;
	}

	writel(config, ctrl->regs + CFG);
	writel(bch_config, ctrl->regs + BCH_CONFIG);

	err = nand_scan_tail(mtd);
	if (err)
		return err;

	config |= CFG_TAG_BYTE_SIZE(mtd_ooblayout_count_freebytes(mtd) - 1);
	writel(config, ctrl->regs + CFG);

	err = mtd_device_register(mtd, NULL, 0);
	if (err)
		return err;

	return 0;
}

static int tegra_nand_probe(struct platform_device *pdev)
{
	struct reset_control *rst;
	struct tegra_nand_controller *ctrl;
	struct resource *res;
	unsigned long value;
	int irq, err = 0;

	ctrl = devm_kzalloc(&pdev->dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->dev = &pdev->dev;
	nand_hw_control_init(&ctrl->controller);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ctrl->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(ctrl->regs))
		return PTR_ERR(ctrl->regs);

	rst = devm_reset_control_get(&pdev->dev, "nand");
	if (IS_ERR(rst))
		return PTR_ERR(rst);

	ctrl->clk = devm_clk_get(&pdev->dev, "nand");
	if (IS_ERR(ctrl->clk))
		return PTR_ERR(ctrl->clk);

	err = clk_prepare_enable(ctrl->clk);
	if (err)
		return err;

	reset_control_reset(rst);

	value = HWSTATUS_RDSTATUS_MASK(1) | HWSTATUS_RDSTATUS_VALUE(0) |
		HWSTATUS_RBSY_MASK(NAND_STATUS_READY) |
		HWSTATUS_RBSY_VALUE(NAND_STATUS_READY);
	writel(NAND_CMD_STATUS, ctrl->regs + HWSTATUS_CMD);
	writel(value, ctrl->regs + HWSTATUS_MASK);

	init_completion(&ctrl->command_complete);
	init_completion(&ctrl->dma_complete);

	/* clear interrupts */
	value = readl(ctrl->regs + ISR);
	writel(value, ctrl->regs + ISR);

	irq = platform_get_irq(pdev, 0);
	err = devm_request_irq(&pdev->dev, irq, tegra_nand_irq, 0,
			       dev_name(&pdev->dev), ctrl);
	if (err)
		goto err_disable_clk;

	writel(DMA_CTRL_IS_DONE, ctrl->regs + DMA_CTRL);

	/* enable interrupts */
	value = IER_UND | IER_OVR | IER_CMD_DONE | IER_ECC_ERR | IER_GIE;
	writel(value, ctrl->regs + IER);

	/* reset config */
	writel(0, ctrl->regs + CFG);

	err = tegra_nand_chips_init(ctrl->dev, ctrl);
	if (err)
		goto err_disable_clk;

	platform_set_drvdata(pdev, ctrl);

	return 0;

err_disable_clk:
	clk_disable_unprepare(ctrl->clk);
	return err;
}

static int tegra_nand_remove(struct platform_device *pdev)
{
	struct tegra_nand_controller *ctrl = platform_get_drvdata(pdev);

	nand_release(nand_to_mtd(ctrl->chip));

	clk_disable_unprepare(ctrl->clk);

	return 0;
}

static const struct of_device_id tegra_nand_of_match[] = {
	{ .compatible = "nvidia,tegra20-nand" },
	{ /* sentinel */ }
};

static struct platform_driver tegra_nand_driver = {
	.driver = {
		.name = "tegra-nand",
		.of_match_table = tegra_nand_of_match,
	},
	.probe = tegra_nand_probe,
	.remove = tegra_nand_remove,
};
module_platform_driver(tegra_nand_driver);

MODULE_DESCRIPTION("NVIDIA Tegra NAND driver");
MODULE_AUTHOR("Thierry Reding <thierry.reding@nvidia.com>");
MODULE_AUTHOR("Lucas Stach <dev@lynxeye.de>");
MODULE_AUTHOR("Stefan Agner <stefan@agner.ch>");
MODULE_LICENSE("GPL v2");
MODULE_DEVICE_TABLE(of, tegra_nand_of_match);
