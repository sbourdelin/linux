// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Amlogic Meson Nand Flash Controller Driver
 *
 * Copyright (c) 2018 Amlogic, inc.
 * Author: Liang Yang <liang.yang@amlogic.com>
 */

#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/mtd/rawnand.h>
#include <linux/mtd/mtd.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_device.h>

#define NFC_REG_CMD		0x00
#define NFC_CMD_DRD		(0x8 << 14)
#define NFC_CMD_IDLE		(0xc << 14)
#define NFC_CMD_DWR		(0x4 << 14)
#define NFC_CMD_CLE		(0x5 << 14)
#define NFC_CMD_ALE		(0x6 << 14)
#define NFC_CMD_ADL		((0 << 16) | (3 << 20))
#define NFC_CMD_ADH		((1 << 16) | (3 << 20))
#define NFC_CMD_AIL		((2 << 16) | (3 << 20))
#define NFC_CMD_AIH		((3 << 16) | (3 << 20))
#define NFC_CMD_SEED		((8 << 16) | (3 << 20))
#define NFC_CMD_M2N		((0 << 17) | (2 << 20))
#define NFC_CMD_N2M		((1 << 17) | (2 << 20))
#define NFC_CMD_RB		BIT(20)
#define NFC_CMD_IO6		((0xb << 10) | (1 << 18))
#define NFC_CMD_SCRAMBLER_ENABLE	BIT(19)
#define NFC_CMD_RB_INT		BIT(14)

#define NFC_CMD_GET_SIZE(x)	(((x) >> 22) & GENMASK(4, 0))
#define NFC_CMD_RB_TIMEOUT	0x18

#define NFC_REG_CFG		0x04
#define NFC_REG_DADR		0x08
#define NFC_REG_IADR		0x0c
#define NFC_REG_BUF		0x10
#define NFC_REG_INFO		0x14
#define NFC_REG_DC		0x18
#define NFC_REG_ADR		0x1c
#define NFC_REG_DL		0x20
#define NFC_REG_DH		0x24
#define NFC_REG_CADR		0x28
#define NFC_REG_SADR		0x2c
#define NFC_REG_PINS		0x30
#define NFC_REG_VER		0x38

#define NFC_RB_IRQ_EN		BIT(21)
#define NFC_INT_MASK		(3 << 20)

#define CMDRWGEN(cmd_dir, ran, bch, short_mode, page_size, pages)	\
	(								\
		(cmd_dir)			|			\
		((ran) << 19)			|			\
		((bch) << 14)			|			\
		((short_mode) << 13)		|			\
		(((page_size) & 0x7f) << 6)	|			\
		((pages) & 0x3f)					\
	)

#define GENCMDDADDRL(adl, addr)		((adl) | ((addr) & 0xffff))
#define GENCMDDADDRH(adh, addr)		((adh) | (((addr) >> 16) & 0xffff))
#define GENCMDIADDRL(ail, addr)		((ail) | ((addr) & 0xffff))
#define GENCMDIADDRH(aih, addr)		((aih) | (((addr) >> 16) & 0xffff))

#define RB_STA(x)		(1 << (26 + (x)))
#define DMA_DIR(dir)		((dir) ? NFC_CMD_N2M : NFC_CMD_M2N)

#define ECC_CHECK_RETURN_FF	(-1)

#define NAND_CE0		(0xe << 10)
#define NAND_CE1		(0xd << 10)

#define DMA_BUSY_TIMEOUT	0x100000
#define CMD_FIFO_EMPTY_TIMEOUT	1000

#define MAX_CE_NUM		2

/* eMMC clock register, misc control */
#define SD_EMMC_CLOCK		0x00
#define CLK_ALWAYS_ON		BIT(28)
#define CLK_SELECT_NAND		BIT(31)
#define CLK_DIV_MASK		GENMASK(5, 0)

#define NFC_CLK_CYCLE		6

/* nand flash controller delay 3 ns */
#define NFC_DEFAULT_DELAY	3000

#define MAX_ECC_INDEX		10

#define MUX_CLK_NUM_PARENTS	2

#define ROW_ADDER(page, index)	(((page) >> (8 * (index))) & 0xff)
#define MAX_CYCLE_ROW_ADDRS	3
#define MAX_CYCLE_COLUMN_ADDRS	2
#define DIRREAD			1
#define DIRWRITE		0

#define ECC_PARITY_BCH8_512B	14

#define PER_INFO_BYTE		8
#define ECC_GET_PROTECTED_OOB_BYTE(x, y)			\
		((x) >> (8 * (y)) & GENMASK(7, 0))

#define ECC_SET_PROTECTED_OOB_BYTE(x, y, z)			\
	{							\
		(x) &= (~((__le64)(0xff) << (8 * (y))));	\
		(x) |= ((__le64)(z) << (8 * (y)));		\
	}

#define ECC_COMPLETE            BIT(31)
#define ECC_ERR_CNT(x)		(((x) >> 24) & GENMASK(5, 0))
#define ECC_ZERO_CNT(x)		(((x) >> 16) & GENMASK(5, 0))

struct meson_nfc_nand_chip {
	struct list_head node;
	struct nand_chip nand;
	int clk_rate;
	int level1_divider;
	int bus_timing;
	int twb;
	int tadl;

	int bch_mode;
	u8 *data_buf;
	__le64 *info_buf;
	int nsels;
	u8 sels[0];
};

struct meson_nand_ecc {
	int bch;
	int strength;
};

struct meson_nfc_data {
	const struct nand_ecc_caps *ecc_caps;
};

struct meson_nfc_param {
	int chip_select;
	int rb_select;
};

struct nand_rw_cmd {
	int cmd0;
	int col[MAX_CYCLE_COLUMN_ADDRS];
	int row[MAX_CYCLE_ROW_ADDRS];
	int cmd1;
};

struct nand_timing {
	int twb;
	int tadl;
};

struct meson_nfc {
	struct nand_controller controller;
	struct clk *core_clk;
	struct clk *device_clk;
	struct clk *phase_tx;
	struct clk *phase_rx;

	int clk_rate;
	int bus_timing;

	struct device *dev;
	void __iomem *reg_base;
	struct regmap *reg_clk;
	struct completion completion;
	struct list_head chips;
	const struct meson_nfc_data *data;
	struct meson_nfc_param param;
	struct nand_timing timing;
	union {
		int cmd[32];
		struct nand_rw_cmd rw;
	} cmdfifo;

	dma_addr_t data_dma;
	dma_addr_t info_dma;

	unsigned long assigned_cs;
};

enum {
	NFC_ECC_BCH8_1K		= 2,
	NFC_ECC_BCH24_1K,
	NFC_ECC_BCH30_1K,
	NFC_ECC_BCH40_1K,
	NFC_ECC_BCH50_1K,
	NFC_ECC_BCH60_1K,
};

#define MESON_ECC_DATA(b, s)	{ .bch = (b),	.strength = (s)}

static int meson_nand_calc_ecc_bytes(int step_size, int strength)
{
	int ecc_bytes;

	if (step_size == 512 && strength == 8)
		return ECC_PARITY_BCH8_512B;

	ecc_bytes = DIV_ROUND_UP(strength * fls(step_size * 8), 8);
	ecc_bytes = ALIGN(ecc_bytes, 2);

	return ecc_bytes;
}

NAND_ECC_CAPS_SINGLE(meson_gxl_ecc_caps,
		     meson_nand_calc_ecc_bytes, 1024, 8, 24, 30, 40, 50, 60);
NAND_ECC_CAPS_SINGLE(meson_axg_ecc_caps,
		     meson_nand_calc_ecc_bytes, 1024, 8);

static inline
struct meson_nfc_nand_chip *to_meson_nand(struct nand_chip *nand)
{
	return container_of(nand, struct meson_nfc_nand_chip, nand);
}

static void meson_nfc_select_chip(struct nand_chip *nand, int chip)
{
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	struct meson_nfc *nfc = nand_get_controller_data(nand);
	int ret, value;

	if (chip < 0 || WARN_ON_ONCE(chip > MAX_CE_NUM))
		return;

	nfc->param.chip_select = meson_chip->sels[chip] ? NAND_CE1 : NAND_CE0;
	nfc->param.rb_select = nfc->param.chip_select;
	nfc->timing.twb = meson_chip->twb;
	nfc->timing.tadl = meson_chip->tadl;

	if (chip >= 0) {
		if (nfc->clk_rate != meson_chip->clk_rate) {
			ret = clk_set_rate(nfc->device_clk,
					   meson_chip->clk_rate);
			if (ret) {
				dev_err(nfc->dev, "failed to set clock rate\n");
				return;
			}
			nfc->clk_rate = meson_chip->clk_rate;
		}
		if (nfc->bus_timing != meson_chip->bus_timing) {
			value = (NFC_CLK_CYCLE - 1)
				| (meson_chip->bus_timing << 5);
			writel(value, nfc->reg_base + NFC_REG_CFG);
			writel((1 << 31), nfc->reg_base + NFC_REG_CMD);
			nfc->bus_timing =  meson_chip->bus_timing;
		}
	}
}

static inline void meson_nfc_cmd_idle(struct meson_nfc *nfc, u32 time)
{
	writel(nfc->param.chip_select | NFC_CMD_IDLE | (time & 0x3ff),
	       nfc->reg_base + NFC_REG_CMD);
}

static inline void meson_nfc_cmd_seed(struct meson_nfc *nfc, u32 seed)
{
	writel(NFC_CMD_SEED | (0xc2 + (seed & 0x7fff)),
	       nfc->reg_base + NFC_REG_CMD);
}

static void meson_nfc_cmd_access(struct nand_chip *nand, int raw, bool dir)
{
	struct mtd_info *mtd = nand_to_mtd(nand);
	struct meson_nfc *nfc = nand_get_controller_data(mtd_to_nand(mtd));
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	u32 cmd, pagesize, pages;
	int bch = meson_chip->bch_mode;
	int len = mtd->writesize;
	int scramble = (nand->options & NAND_NEED_SCRAMBLING) ? 1 : 0;

	pagesize = nand->ecc.size;

	if (raw) {
		len = mtd->writesize + mtd->oobsize;
		cmd = (len & 0x3fff) | NFC_CMD_SCRAMBLER_ENABLE | DMA_DIR(dir);
		writel(cmd, nfc->reg_base + NFC_REG_CMD);
		return;
	}

	pages = len / nand->ecc.size;

	cmd = CMDRWGEN(DMA_DIR(dir), scramble, bch, 0, pagesize, pages);

	writel(cmd, nfc->reg_base + NFC_REG_CMD);
}

static inline void meson_nfc_drain_cmd(struct meson_nfc *nfc)
{
	/*
	 * Insert two commands to make sure all valid commands are finished.
	 *
	 * The Nand flash controller is designed as two stages pipleline -
	 *  a) fetch and b) excute.
	 * There might be cases when the driver see command queue is empty,
	 * but the Nand flash controller still has two commands buffered,
	 * one is fetched into NFC request queue (ready to run), and another
	 * is actively executing. So pushing 2 "IDLE" commands guarantees that
	 * the pipeline is emptied.
	 */
	meson_nfc_cmd_idle(nfc, 0);
	meson_nfc_cmd_idle(nfc, 0);
}

static int meson_nfc_wait_cmd_finish(struct meson_nfc *nfc,
				     unsigned int timeout_ms)
{
	u32 cmd_size = 0;
	int ret;

	/* wait cmd fifo is empty */
	ret = readl_poll_timeout(nfc->reg_base + NFC_REG_CMD, cmd_size,
				 !NFC_CMD_GET_SIZE(cmd_size),
				 10, timeout_ms * 1000);
	if (ret)
		dev_err(nfc->dev, "wait for empty cmd FIFO time out\n");

	return ret;
}

static int meson_nfc_wait_dma_finish(struct meson_nfc *nfc)
{
	meson_nfc_drain_cmd(nfc);

	return meson_nfc_wait_cmd_finish(nfc, DMA_BUSY_TIMEOUT);
}

static inline __le64 nfc_info_ptr(struct nand_chip *nand, int index)
{
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);

	return le64_to_cpu(meson_chip->info_buf[index]);
}

static u8 *meson_nfc_oob_ptr(struct nand_chip *nand, int i)
{
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	int len;

	len = nand->ecc.size * (i + 1) + (nand->ecc.bytes + 2) * i;

	return meson_chip->data_buf + len;
}

static u8 *meson_nfc_data_ptr(struct nand_chip *nand, int i)
{
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	int len;
	int temp;

	temp = nand->ecc.size + nand->ecc.bytes;
	len = (temp + 2) * i;

	return meson_chip->data_buf + len;
}

static void meson_nfc_get_data_oob(struct nand_chip *nand,
				   u8 *buf, u8 *oobbuf)
{
	int i, oob_len = 0;
	u8 *dsrc, *osrc;

	oob_len = nand->ecc.bytes + 2;
	for (i = 0; i < nand->ecc.steps; i++) {
		if (buf) {
			dsrc = meson_nfc_data_ptr(nand, i);
			memcpy(buf, dsrc, nand->ecc.size);
			buf += nand->ecc.size;
		}
		osrc = meson_nfc_oob_ptr(nand, i);
		memcpy(oobbuf, osrc, oob_len);
		oobbuf += oob_len;
	}
}

static void meson_nfc_set_data_oob(struct nand_chip *nand,
				   const u8 *buf, u8 *oobbuf)
{
	int i, oob_len = 0;
	u8 *dsrc, *osrc;

	oob_len = nand->ecc.bytes + 2;
	for (i = 0; i < nand->ecc.steps; i++) {
		if (buf) {
			dsrc = meson_nfc_data_ptr(nand, i);
			memcpy(dsrc, buf, nand->ecc.size);
			buf += nand->ecc.size;
		}
		osrc = meson_nfc_oob_ptr(nand, i);
		memcpy(osrc, oobbuf, oob_len);
		oobbuf += oob_len;
	}
}

static int meson_nfc_queue_rb(struct meson_nfc *nfc, int timeout_ms)
{
	u32 cmd, cfg;
	int ret = 0;

	meson_nfc_cmd_idle(nfc, nfc->timing.twb);
	meson_nfc_drain_cmd(nfc);
	meson_nfc_wait_cmd_finish(nfc, CMD_FIFO_EMPTY_TIMEOUT);

	cfg = readl(nfc->reg_base + NFC_REG_CFG);
	cfg |= (1 << 21);
	writel(cfg, nfc->reg_base + NFC_REG_CFG);

	init_completion(&nfc->completion);

	cmd = NFC_CMD_RB | NFC_CMD_RB_INT
		| nfc->param.chip_select | NFC_CMD_RB_TIMEOUT;
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	ret = wait_for_completion_timeout(&nfc->completion,
					  msecs_to_jiffies(timeout_ms));
	if (ret == 0) {
		dev_err(nfc->dev, "wait nand irq timeout\n");
		ret = -1;
	}
	return ret;
}

static void meson_nfc_set_user_byte(struct nand_chip *nand, u8 *oob_buf)
{
	__le64 info;
	int i, count;

	for (i = 0, count = 0; i < nand->ecc.steps; i++, count += 2) {
		info = nfc_info_ptr(nand, i);
		ECC_SET_PROTECTED_OOB_BYTE(info, 0, oob_buf[count]);
		ECC_SET_PROTECTED_OOB_BYTE(info, 1, oob_buf[count + 1]);
	}
}

static void meson_nfc_get_user_byte(struct nand_chip *nand, u8 *oob_buf)
{
	__le64 info;
	int i, count;

	for (i = 0, count = 0; i < nand->ecc.steps; i++, count += 2) {
		info = nfc_info_ptr(nand, i);
		oob_buf[count] = ECC_GET_PROTECTED_OOB_BYTE(info, 0);
		oob_buf[count + 1] = ECC_GET_PROTECTED_OOB_BYTE(info, 1);
	}
}

static int meson_nfc_ecc_correct(struct nand_chip *nand)
{
	struct mtd_info *mtd = nand_to_mtd(nand);
	__le64 info;
	u32 bitflips = 0, i;
	int scramble;
	u8 zero_cnt;

	scramble = (nand->options & NAND_NEED_SCRAMBLING) ? 1 : 0;

	for (i = 0; i < nand->ecc.steps; i++) {
		info = nfc_info_ptr(nand, i);
		if (ECC_ERR_CNT(info) == 0x3f) {
			zero_cnt = ECC_ZERO_CNT(info);
			if (scramble && zero_cnt < nand->ecc.strength)
				return ECC_CHECK_RETURN_FF;
			mtd->ecc_stats.failed++;
			continue;
		}
		mtd->ecc_stats.corrected += ECC_ERR_CNT(info);
		bitflips = max_t(u32, bitflips, ECC_ERR_CNT(info));
	}

	return bitflips;
}

static inline u8 meson_nfc_read_byte(struct mtd_info *mtd)
{
	struct nand_chip *nand = mtd_to_nand(mtd);
	struct meson_nfc *nfc = nand_get_controller_data(nand);
	u32 cmd;

	cmd = nfc->param.chip_select | NFC_CMD_DRD | 0;
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	meson_nfc_drain_cmd(nfc);

	meson_nfc_wait_cmd_finish(nfc, 1000);

	return readb(nfc->reg_base + NFC_REG_BUF);
}

static void meson_nfc_read_buf(struct mtd_info *mtd, u8 *buf, int len)
{
	int i;

	for (i = 0; i < len; i++)
		buf[i] = meson_nfc_read_byte(mtd);
}

static void meson_nfc_write_byte(struct mtd_info *mtd, u8 byte)
{
	struct meson_nfc *nfc = nand_get_controller_data(mtd_to_nand(mtd));
	u32 cmd;

	meson_nfc_cmd_idle(nfc, nfc->timing.twb);

	cmd = nfc->param.chip_select | NFC_CMD_DWR | (byte & 0xff);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	meson_nfc_cmd_idle(nfc, nfc->timing.twb);
	meson_nfc_cmd_idle(nfc, 0);

	meson_nfc_wait_cmd_finish(nfc, CMD_FIFO_EMPTY_TIMEOUT);
}

static void meson_nfc_write_buf(struct mtd_info *mtd,
				const u8 *buf, int len)
{
	int i;

	for (i = 0; i < len; i++)
		meson_nfc_write_byte(mtd, buf[i]);
}

static int meson_nfc_rw_cmd_prepare_and_execute(struct nand_chip *nand,
						int page, bool in)
{
	struct meson_nfc *nfc = nand_get_controller_data(nand);
	const struct nand_sdr_timings *sdr =
		nand_get_sdr_timings(&nand->data_interface);
	int cs = nfc->param.chip_select;
	int i, cmd0, cmd_num;
	int ret = 0;

	cmd0 = in ? NAND_CMD_READ0 : NAND_CMD_SEQIN;
	cmd_num = sizeof(struct nand_rw_cmd) / sizeof(int);
	if (!in)
		cmd_num--;

	nfc->cmdfifo.rw.cmd0 = cs | NFC_CMD_CLE | cmd0;
	for (i = 0; i < MAX_CYCLE_COLUMN_ADDRS; i++)
		nfc->cmdfifo.rw.col[i] = cs | NFC_CMD_ALE | 0;

	for (i = 0; i < MAX_CYCLE_ROW_ADDRS; i++)
		nfc->cmdfifo.rw.row[i] = cs | NFC_CMD_ALE | ROW_ADDER(page, i);

	nfc->cmdfifo.rw.cmd1 = cs | NFC_CMD_CLE | NAND_CMD_READSTART;

	for (i = 0; i < cmd_num; i++)
		writel(nfc->cmdfifo.cmd[i], nfc->reg_base + NFC_REG_CMD);

	if (in)
		meson_nfc_queue_rb(nfc, sdr->tR_max);
	else
		meson_nfc_cmd_idle(nfc, nfc->timing.tadl);

	return ret;
}

static int meson_nfc_write_page_sub(struct nand_chip *nand, const u8 *buf,
				    int page, int raw)
{
	struct mtd_info *mtd = nand_to_mtd(nand);
	const struct nand_sdr_timings *sdr =
		nand_get_sdr_timings(&nand->data_interface);
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	struct meson_nfc *nfc = nand_get_controller_data(nand);
	dma_addr_t daddr, iaddr;
	u32 cmd;
	int ret;

	daddr = dma_map_single(nfc->dev, (void *)meson_chip->data_buf,
			       mtd->writesize + mtd->oobsize,
			       DMA_TO_DEVICE);
	ret = dma_mapping_error(nfc->dev, daddr);
	if (ret) {
		dev_err(nfc->dev, "dma mapping error\n");
		goto err;
	}

	iaddr = dma_map_single(nfc->dev, (void *)meson_chip->info_buf,
			       nand->ecc.steps * PER_INFO_BYTE,
			       DMA_TO_DEVICE);
	ret = dma_mapping_error(nfc->dev, iaddr);
	if (ret) {
		dev_err(nfc->dev, "dma mapping error\n");
		goto err_map_daddr;
	}

	ret = meson_nfc_rw_cmd_prepare_and_execute(nand, page, DIRWRITE);
	if (ret)
		goto err_map_iaddr;

	cmd = GENCMDDADDRL(NFC_CMD_ADL, daddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	cmd = GENCMDDADDRH(NFC_CMD_ADH, daddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	cmd = GENCMDIADDRL(NFC_CMD_AIL, iaddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	cmd = GENCMDIADDRH(NFC_CMD_AIH, iaddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	meson_nfc_cmd_seed(nfc, page);

	meson_nfc_cmd_access(nand, raw, DIRWRITE);

	ret = meson_nfc_wait_dma_finish(nfc);
	cmd = nfc->param.chip_select | NFC_CMD_CLE | NAND_CMD_PAGEPROG;
	writel(cmd, nfc->reg_base + NFC_REG_CMD);
	meson_nfc_queue_rb(nfc, sdr->tPROG_max);

err_map_iaddr:
	dma_unmap_single(nfc->dev, iaddr,
			 nand->ecc.steps * PER_INFO_BYTE, DMA_TO_DEVICE);
err_map_daddr:
	dma_unmap_single(nfc->dev, daddr,
			 mtd->writesize + mtd->oobsize, DMA_TO_DEVICE);
err:
	return ret;
}

static int meson_nfc_write_page_raw(struct nand_chip *nand, const u8 *buf,
				    int oob_required, int page)
{
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	u8 *oob_buf = nand->oob_poi;

	meson_nfc_set_data_oob(nand, buf, oob_buf);

	return meson_nfc_write_page_sub(nand, meson_chip->data_buf, page, 1);
}

static int meson_nfc_write_page_hwecc(struct nand_chip *nand,
				      const u8 *buf, int oob_required, int page)
{
	struct mtd_info *mtd = nand_to_mtd(nand);
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	u8 *oob_buf = nand->oob_poi;

	memcpy(meson_chip->data_buf, buf, mtd->writesize);
	meson_nfc_set_user_byte(nand, oob_buf);

	return meson_nfc_write_page_sub(nand, meson_chip->data_buf, page, 0);
}

static void meson_nfc_check_ecc_pages_valid(struct meson_nfc *nfc,
					    struct nand_chip *nand, int raw)
{
	__le64 info;
	int neccpages, i;

	neccpages = raw ? 1 : nand->ecc.steps;

	for (i = 0; i < neccpages; i++) {
		info = nfc_info_ptr(nand, neccpages - 1);
		if (!(info & ECC_COMPLETE))
			dev_err(nfc->dev, "seems eccpage is invalid\n");
	}
}

static int meson_nfc_read_page_sub(struct nand_chip *nand, const u8 *buf,
				   int page, int raw)
{
	struct mtd_info *mtd = nand_to_mtd(nand);
	struct meson_nfc *nfc = nand_get_controller_data(nand);
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	dma_addr_t daddr, iaddr;
	u32 cmd;
	int ret;

	daddr = dma_map_single(nfc->dev, (void *)meson_chip->data_buf,
			       mtd->writesize + mtd->oobsize, DMA_FROM_DEVICE);
	ret = dma_mapping_error(nfc->dev, daddr);
	if (ret) {
		dev_err(nfc->dev, "dma mapping error\n");
		goto err;
	}

	iaddr = dma_map_single(nfc->dev, (void *)meson_chip->info_buf,
			       nand->ecc.steps * PER_INFO_BYTE,
			       DMA_FROM_DEVICE);
	ret = dma_mapping_error(nfc->dev, iaddr);
	if (ret) {
		dev_err(nfc->dev, "dma mapping error\n");
		goto err_map_daddr;
	}

	ret = meson_nfc_rw_cmd_prepare_and_execute(nand, page, DIRREAD);
	if (ret)
		goto err_map_iaddr;

	cmd = GENCMDDADDRL(NFC_CMD_ADL, daddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	cmd = GENCMDDADDRH(NFC_CMD_ADH, daddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	cmd = GENCMDIADDRL(NFC_CMD_AIL, iaddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	cmd = GENCMDIADDRH(NFC_CMD_AIH, iaddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);
	meson_nfc_cmd_seed(nfc, page);
	meson_nfc_cmd_access(nand, raw, DIRREAD);
	ret = meson_nfc_wait_dma_finish(nfc);
	meson_nfc_check_ecc_pages_valid(nfc, nand, raw);

err_map_iaddr:
	dma_unmap_single(nfc->dev, iaddr,
			 nand->ecc.steps * PER_INFO_BYTE, DMA_FROM_DEVICE);
err_map_daddr:
	dma_unmap_single(nfc->dev, daddr,
			 mtd->writesize + mtd->oobsize, DMA_FROM_DEVICE);
err:

	return ret;
}

static int meson_nfc_read_page_raw(struct nand_chip *nand, u8 *buf,
				   int oob_required, int page)
{
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	u8 *oob_buf = nand->oob_poi;
	int ret;

	ret = meson_nfc_read_page_sub(nand, meson_chip->data_buf, page, 1);
	if (ret)
		return ret;

	meson_nfc_get_data_oob(nand, buf, oob_buf);

	return 0;
}

static int meson_nfc_read_page_hwecc(struct nand_chip *nand, u8 *buf,
				     int oob_required, int page)
{
	struct mtd_info *mtd = nand_to_mtd(nand);
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	u8 *oob_buf = nand->oob_poi;
	int ret;

	ret = meson_nfc_read_page_sub(nand, meson_chip->data_buf, page, 0);
	if (ret)
		return ret;

	meson_nfc_get_user_byte(nand, oob_buf);

	ret = meson_nfc_ecc_correct(nand);
	if (ret == ECC_CHECK_RETURN_FF) {
		if (buf)
			memset(buf, 0xff, mtd->writesize);

		memset(oob_buf, 0xff, mtd->oobsize);
		return 0;
	}

	if (buf && buf != meson_chip->data_buf)
		memcpy(buf, meson_chip->data_buf, mtd->writesize);

	return ret;
}

static int meson_nfc_read_oob_raw(struct nand_chip *nand, int page)
{
	return meson_nfc_read_page_raw(nand, NULL, 1, page);
}

static int meson_nfc_read_oob(struct nand_chip *nand, int page)
{
	return meson_nfc_read_page_hwecc(nand, NULL, 1, page);
}

static int meson_nfc_exec_op(struct nand_chip *nand,
			     const struct nand_operation *op, bool check_only)
{
	struct mtd_info *mtd = nand_to_mtd(nand);
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	struct meson_nfc *nfc = nand_get_controller_data(nand);
	const struct nand_op_instr *instr = NULL;
	int ret = 0, cmd;
	unsigned int op_id;
	int i, delay_idle;

	for (op_id = 0; op_id < op->ninstrs; op_id++) {
		instr = &op->instrs[op_id];
		delay_idle = DIV_ROUND_UP(PSEC_TO_NSEC(instr->delay_ns),
					  meson_chip->level1_divider
					  * NFC_CLK_CYCLE);
		switch (instr->type) {
		case NAND_OP_CMD_INSTR:
			cmd = nfc->param.chip_select | NFC_CMD_CLE;
			cmd |= instr->ctx.cmd.opcode & 0xff;
			writel(cmd, nfc->reg_base + NFC_REG_CMD);
			meson_nfc_cmd_idle(nfc, delay_idle);
			break;

		case NAND_OP_ADDR_INSTR:
			for (i = 0; i < instr->ctx.addr.naddrs; i++) {
				cmd = nfc->param.chip_select | NFC_CMD_ALE;
				cmd |= instr->ctx.addr.addrs[i] & 0xff;
				writel(cmd, nfc->reg_base + NFC_REG_CMD);
			}
			meson_nfc_cmd_idle(nfc, delay_idle);
			break;

		case NAND_OP_DATA_IN_INSTR:
			meson_nfc_read_buf(mtd, instr->ctx.data.buf.in,
					   instr->ctx.data.len);
			break;

		case NAND_OP_DATA_OUT_INSTR:
			meson_nfc_write_buf(mtd, instr->ctx.data.buf.out,
					    instr->ctx.data.len);
			break;

		case NAND_OP_WAITRDY_INSTR:
			meson_nfc_queue_rb(nfc, instr->ctx.waitrdy.timeout_ms);
			meson_nfc_cmd_idle(nfc, delay_idle);
			break;
		}
	}
	return ret;
}

static int meson_ooblayout_ecc(struct mtd_info *mtd, int section,
			       struct mtd_oob_region *oobregion)
{
	struct nand_chip *nand = mtd_to_nand(mtd);

	if (section >= nand->ecc.steps)
		return -ERANGE;

	oobregion->offset =  2 + (section * (2 + nand->ecc.bytes));
	oobregion->length = nand->ecc.bytes;

	return 0;
}

static int meson_ooblayout_free(struct mtd_info *mtd, int section,
				struct mtd_oob_region *oobregion)
{
	struct nand_chip *nand = mtd_to_nand(mtd);

	if (section >= nand->ecc.steps)
		return -ERANGE;

	oobregion->offset = section * (2 + nand->ecc.bytes);
	oobregion->length = 2;

	return 0;
}

static const struct mtd_ooblayout_ops meson_ooblayout_ops = {
	.ecc = meson_ooblayout_ecc,
	.free = meson_ooblayout_free,
};

static int meson_nfc_clk_init(struct meson_nfc *nfc)
{
	int ret, default_clk_rate;

	/* request core clock */
	nfc->core_clk = devm_clk_get(nfc->dev, "core");
	if (IS_ERR(nfc->core_clk)) {
		dev_err(nfc->dev, "failed to get core clk\n");
		return PTR_ERR(nfc->core_clk);
	}

	nfc->device_clk = devm_clk_get(nfc->dev, "device");
	if (IS_ERR(nfc->device_clk)) {
		dev_err(nfc->dev, "failed to get device clk\n");
		return PTR_ERR(nfc->device_clk);
	}

	nfc->phase_tx = devm_clk_get(nfc->dev, "tx");
	if (IS_ERR(nfc->phase_tx)) {
		dev_err(nfc->dev, "failed to get tx clk\n");
		return PTR_ERR(nfc->phase_tx);
	}

	nfc->phase_rx = devm_clk_get(nfc->dev, "rx");
	if (IS_ERR(nfc->phase_rx)) {
		dev_err(nfc->dev, "failed to get rx clk\n");
		return PTR_ERR(nfc->phase_rx);
	}

	/* init SD_EMMC_CLOCK to sane defaults w/min clock rate */
	regmap_update_bits(nfc->reg_clk,
			   0, CLK_SELECT_NAND, CLK_SELECT_NAND);

	ret = clk_prepare_enable(nfc->core_clk);
	if (ret) {
		dev_err(nfc->dev, "failed to enable core clk\n");
		return ret;
	}

	ret = clk_prepare_enable(nfc->device_clk);
	if (ret) {
		dev_err(nfc->dev, "failed to enable device clk\n");
		clk_disable_unprepare(nfc->core_clk);
		return ret;
	}

	ret = clk_prepare_enable(nfc->phase_tx);
	if (ret) {
		dev_err(nfc->dev, "failed to enable tx clk\n");
		clk_disable_unprepare(nfc->core_clk);
		clk_disable_unprepare(nfc->device_clk);
		return ret;
	}

	ret = clk_prepare_enable(nfc->phase_rx);
	if (ret) {
		dev_err(nfc->dev, "failed to enable rx clk\n");
		clk_disable_unprepare(nfc->core_clk);
		clk_disable_unprepare(nfc->device_clk);
		clk_disable_unprepare(nfc->phase_tx);
		return ret;
	}

	default_clk_rate = clk_round_rate(nfc->device_clk, 24000000);
	ret = clk_set_rate(nfc->device_clk, default_clk_rate);
	if (ret)
		return ret;

	return 0;
}

static void meson_nfc_disable_clk(struct meson_nfc *nfc)
{
	clk_disable_unprepare(nfc->phase_rx);
	clk_disable_unprepare(nfc->phase_tx);
	clk_disable_unprepare(nfc->device_clk);
	clk_disable_unprepare(nfc->core_clk);
}

static void meson_nfc_free_buffer(struct nand_chip *nand)
{
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);

	kfree(meson_chip->info_buf);
	kfree(meson_chip->data_buf);
}

static int meson_chip_buffer_init(struct nand_chip *nand)
{
	struct mtd_info *mtd = nand_to_mtd(nand);
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	int page_bytes, info_bytes;
	int nsectors;

	nsectors = mtd->writesize / nand->ecc.size;

	page_bytes =  mtd->writesize + mtd->oobsize;
	info_bytes = nsectors * PER_INFO_BYTE;

	meson_chip->data_buf = kmalloc(page_bytes, GFP_KERNEL);
	if (!meson_chip->data_buf)
		return -ENOMEM;

	meson_chip->info_buf = kmalloc(info_bytes, GFP_KERNEL);
	if (!meson_chip->info_buf) {
		kfree(meson_chip->data_buf);
		return -ENOMEM;
	}

	return 0;
}

static
int meson_nfc_setup_data_interface(struct nand_chip *nand, int csline,
				   const struct nand_data_interface *conf)
{
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	const struct nand_sdr_timings *timings;
	int div, bt_min, bt_max;

	timings = nand_get_sdr_timings(conf);
	if (IS_ERR(timings))
		return -ENOTSUPP;

	if (csline == NAND_DATA_IFACE_CHECK_ONLY)
		return 0;

	div = DIV_ROUND_UP((timings->tRC_min / 1000), NFC_CLK_CYCLE);
	bt_min = (timings->tREA_max + NFC_DEFAULT_DELAY) / div;
	bt_max = (NFC_DEFAULT_DELAY + timings->tRHOH_min
				    + timings->tRC_min / 2) / div;

	meson_chip->twb = DIV_ROUND_UP(PSEC_TO_NSEC(timings->tWB_max),
				       div * NFC_CLK_CYCLE);
	meson_chip->tadl = DIV_ROUND_UP(PSEC_TO_NSEC(timings->tADL_min),
					div * NFC_CLK_CYCLE);

	bt_min = DIV_ROUND_UP(bt_min, 1000);
	bt_max = DIV_ROUND_UP(bt_max, 1000);

	if (bt_max < bt_min)
		return -EINVAL;

	meson_chip->level1_divider = div;
	meson_chip->clk_rate = 1000000000 / meson_chip->level1_divider;
	meson_chip->bus_timing = (bt_min + bt_max) / 2 + 1;

	return 0;
}

static int meson_nand_bch_mode(struct nand_chip *nand)
{
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	struct meson_nand_ecc meson_ecc[] = {
		MESON_ECC_DATA(NFC_ECC_BCH8_1K, 8),
		MESON_ECC_DATA(NFC_ECC_BCH24_1K, 24),
		MESON_ECC_DATA(NFC_ECC_BCH30_1K, 30),
		MESON_ECC_DATA(NFC_ECC_BCH40_1K, 40),
		MESON_ECC_DATA(NFC_ECC_BCH50_1K, 50),
		MESON_ECC_DATA(NFC_ECC_BCH60_1K, 60),
	};
	int i;

	if (nand->ecc.strength > 60 || nand->ecc.strength < 8)
		return -EINVAL;

	for (i = 0; i < sizeof(meson_ecc); i++) {
		if (meson_ecc[i].strength == nand->ecc.strength) {
			meson_chip->bch_mode = meson_ecc[i].bch;
			return 0;
		}
	}

	return -EINVAL;
}

static int meson_nand_attach_chip(struct nand_chip *nand)
{
	struct meson_nfc *nfc = nand_get_controller_data(nand);
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	struct mtd_info *mtd = nand_to_mtd(nand);
	int nsectors = mtd->writesize / 1024;
	int ret;

	if (!mtd->name) {
		mtd->name = devm_kasprintf(nfc->dev, GFP_KERNEL,
					   "%s:nand%d",
					   dev_name(nfc->dev),
					   meson_chip->sels[0]);
		if (!mtd->name)
			return -ENOMEM;
	}

	if (nand->bbt_options & NAND_BBT_USE_FLASH)
		nand->bbt_options |= NAND_BBT_NO_OOB;

	nand->options |= NAND_NO_SUBPAGE_WRITE;

	ret = nand_ecc_choose_conf(nand, nfc->data->ecc_caps,
				   mtd->oobsize - 2 * nsectors);
	if (ret) {
		dev_err(nfc->dev, "failed to ecc init\n");
		return -EINVAL;
	}

	ret = meson_nand_bch_mode(nand);
	if (ret)
		return -EINVAL;

	nand->ecc.mode = NAND_ECC_HW;
	nand->ecc.write_page_raw = meson_nfc_write_page_raw;
	nand->ecc.write_page = meson_nfc_write_page_hwecc;
	nand->ecc.write_oob_raw = nand_write_oob_std;
	nand->ecc.write_oob = nand_write_oob_std;

	nand->ecc.read_page_raw = meson_nfc_read_page_raw;
	nand->ecc.read_page = meson_nfc_read_page_hwecc;
	nand->ecc.read_oob_raw = meson_nfc_read_oob_raw;
	nand->ecc.read_oob = meson_nfc_read_oob;

	if (nand->options & NAND_BUSWIDTH_16) {
		dev_err(nfc->dev, "16bits buswidth not supported");
		return -EINVAL;
	}
	meson_chip_buffer_init(nand);
	if (ret)
		return -ENOMEM;

	return ret;
}

static const struct nand_controller_ops meson_nand_controller_ops = {
	.attach_chip = meson_nand_attach_chip,
};

static int
meson_nfc_nand_chip_init(struct device *dev,
			 struct meson_nfc *nfc, struct device_node *np)
{
	struct meson_nfc_nand_chip *meson_chip;
	struct nand_chip *nand;
	struct mtd_info *mtd;
	int ret, nsels, i;
	u32 tmp;

	if (!of_get_property(np, "reg", &nsels))
		return -EINVAL;

	nsels /= sizeof(u32);
	if (!nsels || nsels > MAX_CE_NUM) {
		dev_err(dev, "invalid reg property size\n");
		return -EINVAL;
	}

	meson_chip = devm_kzalloc(dev,
				  sizeof(*meson_chip) + (nsels * sizeof(u8)),
				  GFP_KERNEL);
	if (!meson_chip)
		return -ENOMEM;

	meson_chip->nsels = nsels;

	for (i = 0; i < nsels; i++) {
		ret = of_property_read_u32_index(np, "reg", i, &tmp);
		if (ret) {
			dev_err(dev, "could not retrieve reg property: %d\n",
				ret);
			return ret;
		}

		if (test_and_set_bit(tmp, &nfc->assigned_cs)) {
			dev_err(dev, "CS %d already assigned\n", tmp);
			return -EINVAL;
		}
	}

	nand = &meson_chip->nand;
	nand->controller = &nfc->controller;
	nand->controller->ops = &meson_nand_controller_ops;
	nand_set_flash_node(nand, np);
	nand_set_controller_data(nand, nfc);

	nand->options |= NAND_USE_BOUNCE_BUFFER;
	nand->select_chip = meson_nfc_select_chip;
	nand->exec_op = meson_nfc_exec_op;
	nand->setup_data_interface = meson_nfc_setup_data_interface;
	mtd = nand_to_mtd(nand);
	mtd->owner = THIS_MODULE;
	mtd->dev.parent = dev;

	ret = nand_scan(nand, nsels);
	if (ret)
		return ret;

	ret = mtd_device_register(mtd, NULL, 0);
	if (ret) {
		dev_err(dev, "failed to register mtd device: %d\n", ret);
		nand_cleanup(nand);
		return ret;
	}

	list_add_tail(&meson_chip->node, &nfc->chips);

	return 0;
}

static int meson_nfc_nand_chip_cleanup(struct meson_nfc *nfc)
{
	struct meson_nfc_nand_chip *meson_chip;
	struct mtd_info *mtd;
	int ret;

	while (!list_empty(&nfc->chips)) {
		meson_chip = list_first_entry(&nfc->chips,
					      struct meson_nfc_nand_chip, node);
		mtd = nand_to_mtd(&meson_chip->nand);
		ret = mtd_device_unregister(mtd);
		if (ret)
			return ret;

		meson_nfc_free_buffer(&meson_chip->nand);
		nand_cleanup(&meson_chip->nand);
		list_del(&meson_chip->node);
	}

	return 0;
}

static int meson_nfc_nand_chips_init(struct device *dev,
				     struct meson_nfc *nfc)
{
	struct device_node *np = dev->of_node;
	struct device_node *nand_np;
	int ret;

	for_each_child_of_node(np, nand_np) {
		ret = meson_nfc_nand_chip_init(dev, nfc, nand_np);
		if (ret) {
			meson_nfc_nand_chip_cleanup(nfc);
			return ret;
		}
	}

	return 0;
}

static irqreturn_t meson_nfc_irq(int irq, void *id)
{
	struct meson_nfc *nfc = id;
	u32 cfg;

	cfg = readl(nfc->reg_base + NFC_REG_CFG);
	if (!(cfg & NFC_RB_IRQ_EN))
		return IRQ_NONE;

	cfg &= ~(NFC_RB_IRQ_EN);
	writel(cfg, nfc->reg_base + NFC_REG_CFG);

	complete(&nfc->completion);
	return IRQ_HANDLED;
}

static const struct meson_nfc_data meson_gxl_data = {
	.ecc_caps = &meson_gxl_ecc_caps,
};

static const struct meson_nfc_data meson_axg_data = {
	.ecc_caps = &meson_axg_ecc_caps,
};

static const struct of_device_id meson_nfc_id_table[] = {
	{
		.compatible = "amlogic,meson-gxl-nfc",
		.data = &meson_gxl_data,
	}, {
		.compatible = "amlogic,meson-axg-nfc",
		.data = &meson_axg_data,
	},
	{}
};
MODULE_DEVICE_TABLE(of, meson_nfc_id_table);

static int meson_nfc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct meson_nfc *nfc;
	struct resource *res;
	int ret, irq;

	nfc = devm_kzalloc(dev, sizeof(*nfc), GFP_KERNEL);
	if (!nfc)
		return -ENOMEM;

	nfc->data = of_device_get_match_data(&pdev->dev);
	if (!nfc->data)
		return -ENODEV;

	nand_controller_init(&nfc->controller);
	INIT_LIST_HEAD(&nfc->chips);

	nfc->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	nfc->reg_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(nfc->reg_base))
		return PTR_ERR(nfc->reg_base);

	nfc->reg_clk =
		syscon_regmap_lookup_by_phandle(dev->of_node,
						"amlogic,mmc-syscon");
	if (IS_ERR(nfc->reg_clk)) {
		dev_err(dev, "Failed to lookup clock base\n");
		return PTR_ERR(nfc->reg_clk);
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev, "no nfi irq resource\n");
		return -EINVAL;
	}

	ret = meson_nfc_clk_init(nfc);
	if (ret) {
		dev_err(dev, "failed to initialize nand clk\n");
		goto err;
	}

	writel(0, nfc->reg_base + NFC_REG_CFG);
	ret = devm_request_irq(dev, irq, meson_nfc_irq, 0, dev_name(dev), nfc);
	if (ret) {
		dev_err(dev, "failed to request nfi irq\n");
		ret = -EINVAL;
		goto err_clk;
	}

	ret = dma_set_mask(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "failed to set dma mask\n");
		goto err_clk;
	}

	platform_set_drvdata(pdev, nfc);

	ret = meson_nfc_nand_chips_init(dev, nfc);
	if (ret) {
		dev_err(dev, "failed to init nand chips\n");
		goto err_clk;
	}

	return 0;

err_clk:
	meson_nfc_disable_clk(nfc);
err:
	return ret;
}

static int meson_nfc_remove(struct platform_device *pdev)
{
	struct meson_nfc *nfc = platform_get_drvdata(pdev);
	int ret;

	ret = meson_nfc_nand_chip_cleanup(nfc);
	if (ret)
		return ret;

	meson_nfc_disable_clk(nfc);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver meson_nfc_driver = {
	.probe  = meson_nfc_probe,
	.remove = meson_nfc_remove,
	.driver = {
		.name  = "meson-nand",
		.of_match_table = meson_nfc_id_table,
	},
};
module_platform_driver(meson_nfc_driver);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Liang Yang <liang.yang@amlogic.com>");
MODULE_DESCRIPTION("Amlogic's Meson NAND Flash Controller driver");
