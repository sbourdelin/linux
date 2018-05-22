/*
 * Copyright (C) 2018 Stefan Agner <stefan@agner.ch>
 * Copyright (C) 2014-2015 Lucas Stach <dev@lynxeye.de>
 * Copyright (C) 2012 Avionic Design GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
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

#define DEC_STAT_RESULT				0xd0
#define DEC_STAT_BUF				0xd4
#define   DEC_STAT_BUF_CORR_SEC_FLAG_MASK	0x00ff0000
#define   DEC_STAT_BUF_CORR_SEC_FLAG_SHIFT	16
#define   DEC_STAT_BUF_MAX_CORR_CNT_MASK	0x00001f00
#define   DEC_STAT_BUF_MAX_CORR_CNT_SHIFT	8

struct tegra_nand {
	void __iomem *regs;
	struct clk *clk;
	struct gpio_desc *wp_gpio;

	struct nand_chip chip;
	struct device *dev;

	struct completion command_complete;
	struct completion dma_complete;
	bool last_read_error;

	dma_addr_t data_dma;
	void *data_buf;
	dma_addr_t oob_dma;
	void *oob_buf;

	int cur_chip;
};

static inline struct tegra_nand *to_tegra_nand(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd_to_nand(mtd);

	return nand_get_controller_data(chip);
}

static int tegra_nand_ooblayout_16_ecc(struct mtd_info *mtd, int section,
				       struct mtd_oob_region *oobregion)
{
	if (section > 0)
		return -ERANGE;

	oobregion->offset = 4;
	oobregion->length = 4;

	return 0;
}

static int tegra_nand_ooblayout_16_free(struct mtd_info *mtd, int section,
					struct mtd_oob_region *oobregion)
{
	if (section > 0)
		return -ERANGE;

	oobregion->offset = 8;
	oobregion->length = 8;

	return 0;
}

static const struct mtd_ooblayout_ops tegra_nand_oob_16_ops = {
	.ecc = tegra_nand_ooblayout_16_ecc,
	.free = tegra_nand_ooblayout_16_free,
};

static int tegra_nand_ooblayout_64_ecc(struct mtd_info *mtd, int section,
				       struct mtd_oob_region *oobregion)
{
	if (section > 0)
		return -ERANGE;

	oobregion->offset = 4;
	oobregion->length = 36;

	return 0;
}

static int tegra_nand_ooblayout_64_free(struct mtd_info *mtd, int section,
					struct mtd_oob_region *oobregion)
{
	if (section > 0)
		return -ERANGE;

	oobregion->offset = 40;
	oobregion->length = 24;

	return 0;
}

static const struct mtd_ooblayout_ops tegra_nand_oob_64_ops = {
	.ecc = tegra_nand_ooblayout_64_ecc,
	.free = tegra_nand_ooblayout_64_free,
};

static int tegra_nand_ooblayout_128_ecc(struct mtd_info *mtd, int section,
				       struct mtd_oob_region *oobregion)
{
	if (section > 0)
		return -ERANGE;

	oobregion->offset = 4;
	oobregion->length = 72;

	return 0;
}

static int tegra_nand_ooblayout_128_free(struct mtd_info *mtd, int section,
					struct mtd_oob_region *oobregion)
{
	if (section > 0)
		return -ERANGE;

	oobregion->offset = 76;
	oobregion->length = 52;

	return 0;
}

static const struct mtd_ooblayout_ops tegra_nand_oob_128_ops = {
	.ecc = tegra_nand_ooblayout_128_ecc,
	.free = tegra_nand_ooblayout_128_free,
};

static int tegra_nand_ooblayout_224_ecc(struct mtd_info *mtd, int section,
				       struct mtd_oob_region *oobregion)
{
	if (section > 0)
		return -ERANGE;

	oobregion->offset = 4;
	oobregion->length = 144;

	return 0;
}

static int tegra_nand_ooblayout_224_free(struct mtd_info *mtd, int section,
					struct mtd_oob_region *oobregion)
{
	if (section > 0)
		return -ERANGE;

	oobregion->offset = 148;
	oobregion->length = 76;

	return 0;
}

static const struct mtd_ooblayout_ops tegra_nand_oob_224_ops = {
	.ecc = tegra_nand_ooblayout_224_ecc,
	.free = tegra_nand_ooblayout_224_free,
};

static irqreturn_t tegra_nand_irq(int irq, void *data)
{
	struct tegra_nand *nand = data;
	u32 isr, dma;

	isr = readl(nand->regs + ISR);
	dma = readl(nand->regs + DMA_CTRL);
	dev_dbg(nand->dev, "isr %08x\n", isr);

	if (!isr && !(dma & DMA_CTRL_IS_DONE))
		return IRQ_NONE;

	if (isr & ISR_CORRFAIL_ERR)
		nand->last_read_error = true;

	if (isr & ISR_CMD_DONE)
		complete(&nand->command_complete);

	if (isr & ISR_UND)
		dev_dbg(nand->dev, "FIFO underrun\n");

	if (isr & ISR_OVR)
		dev_dbg(nand->dev, "FIFO overrun\n");

	/* handle DMA interrupts */
	if (dma & DMA_CTRL_IS_DONE) {
		writel(dma, nand->regs + DMA_CTRL);
		complete(&nand->dma_complete);
	}

	/* clear interrupts */
	writel(isr, nand->regs + ISR);

	return IRQ_HANDLED;
}

static int tegra_nand_cmd(struct nand_chip *chip,
			 const struct nand_subop *subop)
{
	const struct nand_op_instr *instr;
	const struct nand_op_instr *instr_data_in = NULL;
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct tegra_nand *nand = to_tegra_nand(mtd);
	unsigned int op_id = -1, trfr_in_sz = 0, trfr_out_sz = 0, offset = 0;
	bool first_cmd = true;
	bool force8bit;
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
				writel(instr->ctx.cmd.opcode, nand->regs + CMD_1);
			} else {
				cmd |= CMD_SEC_CMD;
				writel(instr->ctx.cmd.opcode, nand->regs + CMD_2);
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
			writel(addr1, nand->regs + ADDR_1);
			writel(addr2, nand->regs + ADDR_2);
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
			writel(value, nand->regs + RESP);

			break;
		case NAND_OP_WAITRDY_INSTR:
			cmd |= CMD_RBSY_CHK;
			break;

		}
	}


	cmd |= CMD_GO | CMD_CE(nand->cur_chip);
	writel(cmd, nand->regs + CMD);
	wait_for_completion(&nand->command_complete);

	if (instr_data_in) {
		u32 value;
		size_t n = min_t(size_t, trfr_in_sz, 4);

		value = readl(nand->regs + RESP);
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
static void tegra_nand_select_chip(struct mtd_info *mtd, int chip)
{
	struct tegra_nand *nand = to_tegra_nand(mtd);

	nand->cur_chip = chip;
}

static u32 tegra_nand_fill_address(struct mtd_info *mtd, struct nand_chip *chip,
				   int page)
{
	struct tegra_nand *nand = to_tegra_nand(mtd);

	/* Lower 16-bits are column, always 0 */
	writel(page << 16, nand->regs + ADDR_1);

	if (chip->options & NAND_ROW_ADDR_3) {
		writel(page >> 16, nand->regs + ADDR_2);
		return 5;
	}

	return 4;
}

static int tegra_nand_read_page(struct mtd_info *mtd, struct nand_chip *chip,
				uint8_t *buf, int oob_required, int page)
{
	struct tegra_nand *nand = to_tegra_nand(mtd);
	u32 value, addrs;

	writel(NAND_CMD_READ0, nand->regs + CMD_1);
	writel(NAND_CMD_READSTART, nand->regs + CMD_2);

	addrs = tegra_nand_fill_address(mtd, chip, page);

	value = readl(nand->regs + CFG);
	value |= CFG_HW_ECC | CFG_ERR_COR;
	writel(value, nand->regs + CFG);

	writel(mtd->writesize - 1, nand->regs + DMA_CFG_A);
	writel(nand->data_dma, nand->regs + DATA_PTR);

	if (oob_required) {
		writel(mtd_ooblayout_count_freebytes(mtd) - 1,
		       nand->regs + DMA_CFG_B);
		writel(nand->oob_dma, nand->regs + TAG_PTR);
	} else {
		writel(0, nand->regs + DMA_CFG_B);
		writel(0, nand->regs + TAG_PTR);
	}

	value = DMA_CTRL_GO | DMA_CTRL_IN | DMA_CTRL_PERF_EN |
		DMA_CTRL_REUSE | DMA_CTRL_IE_DONE | DMA_CTRL_IS_DONE |
		DMA_CTRL_BURST_8 | DMA_CTRL_EN_A;
	if (oob_required)
		value |= DMA_CTRL_EN_B;
	writel(value, nand->regs + DMA_CTRL);

	value = CMD_CLE | CMD_ALE | CMD_ALE_SIZE(addrs) | CMD_SEC_CMD |
		CMD_RBSY_CHK | CMD_GO | CMD_RX | CMD_TRANS_SIZE(9) |
		CMD_A_VALID | CMD_CE(nand->cur_chip);
	if (oob_required)
		value |= CMD_B_VALID;
	writel(value, nand->regs + CMD);

	wait_for_completion(&nand->command_complete);
	wait_for_completion(&nand->dma_complete);

	if (oob_required) {
		struct mtd_oob_region oobregion;

		mtd_ooblayout_free(mtd, 0, &oobregion);
		memcpy(chip->oob_poi, nand->oob_buf + oobregion.offset,
		       mtd_ooblayout_count_freebytes(mtd));
	}
	memcpy(buf, nand->data_buf, mtd->writesize);

	value = readl(nand->regs + CFG);
	value &= ~(CFG_HW_ECC | CFG_ERR_COR);
	writel(value, nand->regs + CFG);

	value = readl(nand->regs + DEC_STATUS);
	if (value & DEC_STATUS_A_ECC_FAIL) {
		/*
		 * The ECC isn't smart enough to figure out if a page is
		 * completely erased and flags an error in this case. So we
		 * check the read data here to figure out if it's a legitimate
		 * error or a false positive.
		 */
		int i, err;
		int flips_threshold = chip->ecc.strength / 2;
		int max_bitflips = 0;

		for (i = 0; i < chip->ecc.steps; i++) {
			u8 *data = buf + (chip->ecc.size * i);
			err = nand_check_erased_ecc_chunk(data, chip->ecc.size,
							  NULL, 0,
							  NULL, 0,
							  flips_threshold);
			if (err < 0)
				return err;

			max_bitflips += max_bitflips;
		}

		return max_bitflips;
	}

	if (nand->last_read_error) {
		int max_corr_cnt, corr_sec_flag;

		value = readl(nand->regs + DEC_STAT_BUF);
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
		nand->last_read_error = false;
		return value;
	}

	return 0;
}

static int tegra_nand_write_page(struct mtd_info *mtd, struct nand_chip *chip,
				 const uint8_t *buf, int oob_required, int page)
{
	struct tegra_nand *nand = to_tegra_nand(mtd);
	u32 value, addrs;

	writel(NAND_CMD_SEQIN, nand->regs + CMD_1);
	writel(NAND_CMD_PAGEPROG, nand->regs + CMD_2);

	addrs = tegra_nand_fill_address(mtd, chip, page);

	value = readl(nand->regs + CFG);
	value |= CFG_HW_ECC | CFG_ERR_COR;
	writel(value, nand->regs + CFG);

	memcpy(nand->data_buf, buf, mtd->writesize);

	writel(mtd->writesize - 1, nand->regs + DMA_CFG_A);
	writel(nand->data_dma, nand->regs + DATA_PTR);

	if (oob_required) {
		struct mtd_oob_region oobregion;

		mtd_ooblayout_free(mtd, 0, &oobregion);
		memcpy(nand->oob_buf, chip->oob_poi + oobregion.offset,
		       mtd_ooblayout_count_freebytes(mtd));
		writel(mtd_ooblayout_count_freebytes(mtd) - 1,
		       nand->regs + DMA_CFG_B);
		writel(nand->oob_dma, nand->regs + TAG_PTR);
	} else {
		writel(0, nand->regs + DMA_CFG_B);
		writel(0, nand->regs + TAG_PTR);
	}

	value = DMA_CTRL_GO | DMA_CTRL_OUT | DMA_CTRL_PERF_EN |
		DMA_CTRL_IE_DONE | DMA_CTRL_IS_DONE |
		DMA_CTRL_BURST_8 | DMA_CTRL_EN_A;
	if (oob_required)
		value |= DMA_CTRL_EN_B;
	writel(value, nand->regs + DMA_CTRL);

	value = CMD_CLE | CMD_ALE | CMD_ALE_SIZE(addrs) | CMD_SEC_CMD |
		CMD_AFT_DAT | CMD_RBSY_CHK | CMD_GO | CMD_TX | CMD_A_VALID |
		CMD_TRANS_SIZE(9) | CMD_CE(nand->cur_chip);
	if (oob_required)
		value |= CMD_B_VALID;
	writel(value, nand->regs + CMD);

	wait_for_completion(&nand->command_complete);
	wait_for_completion(&nand->dma_complete);

	value = readl(nand->regs + CFG);
	value &= ~(CFG_HW_ECC | CFG_ERR_COR);
	writel(value, nand->regs + CFG);

	return 0;
}

static void tegra_nand_setup_timing(struct tegra_nand *nand, int mode)
{
	/*
	 * The period (and all other timings in this function) is in ps,
	 * so need to take care here to avoid integer overflows.
	 */
	unsigned int rate = clk_get_rate(nand->clk) / 1000000;
	unsigned int period = DIV_ROUND_UP(1000000, rate);
	const struct nand_sdr_timings *timings;
	u32 val, reg = 0;

	timings = onfi_async_timing_mode_to_sdr_timings(mode);

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

	writel(reg, nand->regs + TIMING_1);

	val = DIV_ROUND_UP(timings->tADL_min, period);
	if (val > 2)
		val -= 3;
	reg = TIMING_TADL(val);

	writel(reg, nand->regs + TIMING_2);
}

static void tegra_nand_setup_chiptiming(struct tegra_nand *nand)
{
	struct nand_chip *chip = &nand->chip;
	int mode;

	mode = onfi_get_async_timing_mode(chip);
	if (mode == ONFI_TIMING_MODE_UNKNOWN)
		mode = chip->onfi_timing_mode_default;
	else
		mode = fls(mode);

	tegra_nand_setup_timing(nand, mode);
}

static int tegra_nand_probe(struct platform_device *pdev)
{
	struct reset_control *rst;
	struct tegra_nand *nand;
	struct nand_chip *chip;
	struct mtd_info *mtd;
	struct resource *res;
	unsigned long value;
	int irq, err = 0;

	nand = devm_kzalloc(&pdev->dev, sizeof(*nand), GFP_KERNEL);
	if (!nand)
		return -ENOMEM;

	nand->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	nand->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(nand->regs))
		return PTR_ERR(nand->regs);

	irq = platform_get_irq(pdev, 0);
	err = devm_request_irq(&pdev->dev, irq, tegra_nand_irq, 0,
			       dev_name(&pdev->dev), nand);
	if (err)
		return err;

	rst = devm_reset_control_get(&pdev->dev, "nand");
	if (IS_ERR(rst))
		return PTR_ERR(rst);

	nand->clk = devm_clk_get(&pdev->dev, "nand");
	if (IS_ERR(nand->clk))
		return PTR_ERR(nand->clk);

	nand->wp_gpio = gpiod_get_optional(&pdev->dev, "wp-gpios",
					   GPIOD_OUT_HIGH);
	if (IS_ERR(nand->wp_gpio))
		return PTR_ERR(nand->wp_gpio);

	err = clk_prepare_enable(nand->clk);
	if (err)
		return err;

	reset_control_assert(rst);
	udelay(2);
	reset_control_deassert(rst);

	value = HWSTATUS_RDSTATUS_MASK(1) | HWSTATUS_RDSTATUS_VALUE(0) |
		HWSTATUS_RBSY_MASK(NAND_STATUS_READY) |
		HWSTATUS_RBSY_VALUE(NAND_STATUS_READY);
	writel(NAND_CMD_STATUS, nand->regs + HWSTATUS_CMD);
	writel(value, nand->regs + HWSTATUS_MASK);

	init_completion(&nand->command_complete);
	init_completion(&nand->dma_complete);

	/* clear interrupts */
	value = readl(nand->regs + ISR);
	writel(value, nand->regs + ISR);

	writel(DMA_CTRL_IS_DONE, nand->regs + DMA_CTRL);

	/* enable interrupts */
	value = IER_UND | IER_OVR | IER_CMD_DONE | IER_ECC_ERR | IER_GIE;
	writel(value, nand->regs + IER);

	/* reset config */
	writel(0, nand->regs + CFG);

	chip = &nand->chip;
	mtd = nand_to_mtd(chip);

	mtd->dev.parent = &pdev->dev;
	mtd->name = "tegra_nand";
	mtd->owner = THIS_MODULE;

	nand_set_flash_node(chip, pdev->dev.of_node);
	nand_set_controller_data(chip, nand);

	chip->options = NAND_NO_SUBPAGE_WRITE;
	chip->exec_op = tegra_nand_exec_op;
	chip->select_chip = tegra_nand_select_chip;
	tegra_nand_setup_timing(nand, 0);

	err = nand_scan_ident(mtd, 1, NULL);
	if (err)
		goto err_disable_clk;

	if (chip->bbt_options & NAND_BBT_USE_FLASH)
		chip->bbt_options |= NAND_BBT_NO_OOB;

	nand->data_buf = dmam_alloc_coherent(&pdev->dev, mtd->writesize,
					    &nand->data_dma, GFP_KERNEL);
	if (!nand->data_buf) {
		err = -ENOMEM;
		goto err_disable_clk;
	}

	nand->oob_buf = dmam_alloc_coherent(&pdev->dev, mtd->oobsize,
					    &nand->oob_dma, GFP_KERNEL);
	if (!nand->oob_buf) {
		err = -ENOMEM;
		goto err_disable_clk;
	}

	chip->ecc.mode = NAND_ECC_HW;
	chip->ecc.size = 512;
	chip->ecc.read_page = tegra_nand_read_page;
	chip->ecc.write_page = tegra_nand_write_page;

	value = readl(nand->regs + CFG);
	value |= CFG_PIPE_EN | CFG_SKIP_SPARE | CFG_SKIP_SPARE_SIZE_4 |
		 CFG_TAG_BYTE_SIZE(mtd_ooblayout_count_freebytes(mtd) - 1);

	if (chip->options & NAND_BUSWIDTH_16)
		value |= CFG_BUS_WIDTH_16;

	switch (mtd->oobsize) {
	case 16:
		mtd_set_ooblayout(mtd, &tegra_nand_oob_16_ops);
		chip->ecc.strength = 1;
		chip->ecc.bytes = 4;
		break;
	case 64:
		mtd_set_ooblayout(mtd, &tegra_nand_oob_64_ops);
		chip->ecc.strength = 8;
		chip->ecc.bytes = 18;
		value |= CFG_ECC_SEL | CFG_TVAL_8;
		break;
	case 128:
		mtd_set_ooblayout(mtd, &tegra_nand_oob_128_ops);
		chip->ecc.strength = 8;
		chip->ecc.bytes = 18;
		value |= CFG_ECC_SEL | CFG_TVAL_8;
		break;
	case 224:
		mtd_set_ooblayout(mtd, &tegra_nand_oob_224_ops);
		chip->ecc.strength = 8;
		chip->ecc.bytes = 18;
		value |= CFG_ECC_SEL | CFG_TVAL_8;
		break;
	default:
		dev_err(&pdev->dev, "unhandled OOB size %d\n", mtd->oobsize);
		err = -ENODEV;
		goto err_disable_clk;
	}

	switch (mtd->writesize) {
	case 256:
		value |= CFG_PS_256;
		break;
	case 512:
		value |= CFG_PS_512;
		break;
	case 1024:
		value |= CFG_PS_1024;
		break;
	case 2048:
		value |= CFG_PS_2048;
		break;
	case 4096:
		value |= CFG_PS_4096;
		break;
	default:
		dev_err(&pdev->dev, "unhandled writesize %d\n", mtd->writesize);
		err = -ENODEV;
		goto err_disable_clk;
	}

	writel(value, nand->regs + CFG);

	tegra_nand_setup_chiptiming(nand);

	err = nand_scan_tail(mtd);
	if (err)
		goto err_disable_clk;

	err = mtd_device_register(mtd, NULL, 0);
	if (err)
		goto err_cleanup_nand;

	platform_set_drvdata(pdev, nand);

	return 0;

err_cleanup_nand:
	nand_cleanup(chip);
err_disable_clk:
	clk_disable_unprepare(nand->clk);
	return err;
}

static int tegra_nand_remove(struct platform_device *pdev)
{
	struct tegra_nand *nand = platform_get_drvdata(pdev);

	nand_release(nand_to_mtd(&nand->chip));

	clk_disable_unprepare(nand->clk);

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
