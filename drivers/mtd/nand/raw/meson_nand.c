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
#include <linux/module.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include "clk-regmap.h"

#define NFC_REG_CMD		0x00
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
#define NFC_CMD_RB		(1 << 20)
#define NFC_CMD_IO6		((0xb << 10) | (1 << 18))

#define NFC_RB_USED		(1 << 23)
#define NFC_LARGE_PAGE		(1 << 22)
#define NFC_RW_OPS		(2 << 20)

#define NAND_TWB_TIME_CYCLE	10

#define CMDRWGEN(cmd_dir, ran, bch, short_mode, page_size, pages)	\
	(								\
		(cmd_dir)			|			\
		((ran) << 19)			|			\
		((bch) << 14)			| 			\
		((short_mode) << 13)		|			\
		(((page_size) & 0x7f) << 6)	|			\
		((pages) & 0x3f)					\
	)

#define GENCMDDADDRL(adl, addr)		((adl) | ((addr) & 0xffff))
#define GENCMDDADDRH(adh, addr)		((adh) | (((addr) >> 16) & 0xffff))
#define GENCMDIADDRL(ail, addr)		((ail) | ((addr) & 0xffff))
#define GENCMDIADDRH(aih, addr)		((aih) | (((addr) >> 16) & 0xffff))

#define RB_STA(x)		(1 << (26 + x))

#define ECC_CHECK_RETURN_FF	(-1)

#define NAND_CE0		(0xe << 10)
#define NAND_CE1		(0xd << 10)

#define DMA_BUSY_TIMEOUT	0x100000

#define MAX_CE_NUM		2
#define RAN_ENABLE		1

#define SD_EMMC_CLOCK 		0x00
#define CLK_ALWAYS_ON		BIT(28)
#define CLK_SELECT_NAND		BIT(31)
#define CLK_DIV_MASK		GENMASK(5, 0)
#define CLK_SRC_MASK		GENMASK(7, 6)

#define NFC_CLK_CYCLE		6

/* nand flash controller delay 3 ns */
#define NFC_DEFAULT_DELAY 3000

#define MAX_ECC_INDEX	10

#define MUX_CLK_NUM_PARENTS 2

struct meson_nfc_info_format {
	u16 info_bytes;
	u8 zero_cnt; /* bit0~5 is valid */
	struct ecc_sta {
		u8 eccerr_cnt : 6;
		u8 notused : 1;
		u8 completed : 1;
	} ecc;
	u32 reserved;
};

#define	PER_INFO_BYTE	(sizeof(struct meson_nfc_info_format))

struct meson_nfc_nand_chip {
	struct list_head node;
	struct nand_chip nand;
	/*
	 * Then NAND controller support two oob modes:
	 *  a) 2 user bytes with each ecc page;
	 *  b) 16 user bytes with 1st ecc page and zero user byte
	 *      with the other ecc pages.
	 * when using as mtd mode, the driver prefer to use 2 user bytes mode.
	 */
	int user_mode;
	int rand_mode; /* 0: disable scramble, 1: enable scramble */
	int bch_mode;
	int cs;

	u8 *data_buf;
	u8 *info_buf;
};

/*
 * While booting from NAND, a page0 data is needed to tell ROM boot code
 * to read SPL image, and the ROM boot code need to know which ecc mode
 * is selected and whether scramble is enabled or not, and so on.
 *
 * So when updating SPL image, the driver need to store these informations
 * into the page0, and SPL image will be loadded into next page - the page1.
 */
struct meson_nand_setup {
	u32 d32;
	u16 id;
	u16 max;
};

struct meson_nand_page0 {
	struct meson_nand_setup nand_setup;
	unsigned char page_list[16];
	unsigned short reserved[32];
};

struct meson_nand_ecc {
	int bch;
	int strength;
	int parity;
};

struct meson_nfc_data {
	struct meson_nand_ecc *ecc;
	int ecc_num;
	int bch_mode;
	int short_bch;
};

struct meson_nfc_param {
	int chip_select;
	int rb_select;

	int page_size;
	int oob_size;
	int ecc_size;
	int ecc_bytes;

	int rand_mode;
	int oob_mode;
	int bch_mode;
	int ecc_step;

	int ecc_max;
};

struct meson_nfc {
	struct nand_hw_control controller;
	struct clk *core_clk;
	struct clk *device_clk;

	struct device *dev;
	void __iomem *reg_base;
	struct regmap *reg_clk;

	struct completion completion;
	struct list_head chips;
	struct meson_nfc_data *data;
	struct meson_nfc_param param;
	struct meson_nand_page0 *page0;

	u8 *data_buf;
	u8 *info_buf;
};

enum {
	NFC_ECC_NONE	= 0,
	NFC_ECC_BCH8,		/* bch8 with ecc page size of 512B */
	NFC_ECC_BCH8_1K,	/* bch8 with ecc page size of 1024B */
	NFC_ECC_BCH24_1K,
	NFC_ECC_BCH30_1K,
	NFC_ECC_BCH40_1K,
	NFC_ECC_BCH50_1K,
	NFC_ECC_BCH60_1K,

	/*
	 * Short mode is special only for page 0 when inplement booting
	 * from nand, which means a small size(384 bit / 8 = 48 Byte) of
	 * ecc page is used with a fixed ecc mode. rom code will use short mode
	 * to read page0 for getting nand parameters such as ecc, scramber, etc.
	 *
	 * Example, in GXL SoC, the first page adopt the short mode with
	 * 60bit ecc, while in AXG SoC, it adopt short mode with 8bit ecc.
	 */
	NFC_ECC_BCH_SHORT,
};

enum {
	NFC_USER2_OOB_BYTES		= 2,
	NFC_USER16_OOB_BYTES		= 16,
};

#define MESON_ECC_DATA(b, s, p)				\
	{ .bch = (b),	.strength = (s),	.parity = (p) }

struct meson_nand_ecc meson_gxl_ecc[] = {
	MESON_ECC_DATA(NFC_ECC_NONE, 0, 0),
	MESON_ECC_DATA(NFC_ECC_BCH8, 8, 14),
	MESON_ECC_DATA(NFC_ECC_BCH8_1K, 8, 14),
	MESON_ECC_DATA(NFC_ECC_BCH24_1K, 24, 42),
	MESON_ECC_DATA(NFC_ECC_BCH30_1K, 30, 54),
	MESON_ECC_DATA(NFC_ECC_BCH40_1K, 40, 70),
	MESON_ECC_DATA(NFC_ECC_BCH50_1K, 50, 88),
	MESON_ECC_DATA(NFC_ECC_BCH60_1K, 60, 106),
	MESON_ECC_DATA(NFC_ECC_BCH_SHORT, 0xff, 0xff),
};

struct meson_nand_ecc meson_axg_ecc[] = {
	MESON_ECC_DATA(NFC_ECC_NONE, 0, 0),
	MESON_ECC_DATA(NFC_ECC_BCH8, 8, 14),
	MESON_ECC_DATA(NFC_ECC_BCH8_1K, 8, 14),
	MESON_ECC_DATA(NFC_ECC_BCH_SHORT, 0xff, 0xff),
};

static inline struct meson_nfc_nand_chip *to_meson_nand(struct nand_chip *nand)
{
	return container_of(nand, struct meson_nfc_nand_chip, nand);
}

static int meson_nfc_page0_gen(struct meson_nfc *nfc)
{
	u32 cmd;

	nfc->page0 = devm_kzalloc(nfc->dev,
		sizeof(struct meson_nand_page0), GFP_KERNEL);
	if(!nfc->page0)
		return -ENOMEM;

	cmd = CMDRWGEN(NFC_CMD_N2M, nfc->param.rand_mode,
		nfc->param.bch_mode, 0,
		nfc->param.ecc_size >> 3,
		nfc->param.ecc_step);
	cmd |= NFC_RB_USED | NFC_LARGE_PAGE | NFC_RW_OPS;
	nfc->page0->nand_setup.d32 = cmd;

	return 0;
}

static void meson_nfc_select_chip(struct mtd_info *mtd, int chip)
{
	struct nand_chip *nand = mtd_to_nand(mtd);
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	struct meson_nfc *nfc = nand_get_controller_data(nand);

	if (chip != meson_chip->cs)
		return;

	nfc->param.chip_select = chip ? NAND_CE1 : NAND_CE0;
	nfc->param.rb_select = chip ? NAND_CE1 : NAND_CE0;
	nfc->param.oob_mode =
		(meson_chip->user_mode == NFC_USER2_OOB_BYTES) ? 0 : 1;
	nfc->param.rand_mode = meson_chip->rand_mode;
	nfc->param.bch_mode = meson_chip->bch_mode;

	nfc->param.ecc_step = mtd->writesize / nand->ecc.size;
	nfc->param.ecc_size = nand->ecc.size;
	nfc->param.ecc_bytes =  nand->ecc.bytes;
	nfc->param.page_size = mtd->writesize;
	nfc->param.oob_size =  mtd->oobsize;
	nfc->param.ecc_max = nand->ecc.strength;

	nfc->data_buf = meson_chip->data_buf;
	nfc->info_buf = meson_chip->info_buf;
}

static inline void meson_nfc_cmd_idle(struct meson_nfc *nfc, u32 time)
{
	writel(nfc->param.chip_select | NFC_CMD_IDLE | (time & 0x3ff),
			nfc->reg_base + NFC_REG_CMD);
}

static void meson_nfc_cmd_ctrl(struct mtd_info *mtd,
					int cmd, unsigned int ctrl)
{
	struct meson_nfc *nfc = nand_get_controller_data(mtd_to_nand(mtd));

	if (cmd == NAND_CMD_NONE)
		return;

	cmd = nfc->param.chip_select | (cmd & 0xff);
	cmd |= (ctrl & NAND_CLE) ? NFC_CMD_CLE : NFC_CMD_ALE;

	writel(cmd, nfc->reg_base + NFC_REG_CMD);
}

static inline void meson_nfc_cmd_seed(struct meson_nfc *nfc, u32 seed)
{
	writel(NFC_CMD_SEED | (0xc2 + (seed & 0x7fff)),
			nfc->reg_base + NFC_REG_CMD);
}

static void meson_nfc_cmd_m2n(struct meson_nfc *nfc, int raw)
{
	u32 cmd, pagesize, pages, shortm = 0;
	int bch = nfc->param.bch_mode;
	int len = nfc->param.page_size;

	pagesize = nfc->param.ecc_size;

	if (unlikely(raw)) {
		bch = NAND_ECC_NONE;
		len = nfc->param.page_size + nfc->param.oob_size;
		cmd = NFC_CMD_M2N |
			(len & 0x3fff) | (nfc->param.rand_mode << 19);
		writel(cmd, nfc->reg_base + NFC_REG_CMD);
		return;
	}

	if (unlikely(bch == NFC_ECC_BCH_SHORT)) {
		bch = nfc->data->short_bch;
		pagesize = 384 >> 3;
		pages = len / nfc->param.ecc_size;
		memcpy(nfc->data_buf,
			nfc->page0, sizeof(struct meson_nand_page0));
		shortm = 1;
	} else
		pages = len / nfc->param.ecc_size;

	cmd = CMDRWGEN(NFC_CMD_M2N,
		nfc->param.rand_mode, bch, shortm, pagesize, pages);

	writel(cmd, nfc->reg_base + NFC_REG_CMD);
}

static void meson_nfc_cmd_n2m(struct meson_nfc *nfc, int raw)
{
	u32 cmd, pagesize, pages, shortm = 0;
	int bch = nfc->param.bch_mode;
	int len = nfc->param.page_size;

	pagesize = nfc->param.ecc_size;

	if (unlikely(raw)) {
		bch = NAND_ECC_NONE;
		len = nfc->param.page_size + nfc->param.oob_size;
		cmd = (len & 0x3fff) | (nfc->param.rand_mode << 19) |
			NFC_CMD_N2M;
		writel(cmd, nfc->reg_base + NFC_REG_CMD);
		return;
	}

	if (unlikely(bch == NFC_ECC_BCH_SHORT)) {
		bch = nfc->data->short_bch;
		pagesize = 384 >> 3;
		pages = len / nfc->param.ecc_size;
		shortm = 1;
	} else
		pages = len / nfc->param.ecc_size;

	cmd = CMDRWGEN(NFC_CMD_N2M,
		nfc->param.rand_mode, bch, shortm, pagesize, pages);

	writel(cmd, nfc->reg_base + NFC_REG_CMD);
}

static int meson_nfc_wait_cmd_finish(struct meson_nfc *nfc,
					unsigned int timeout_ms)
{
	u32 cmd_size = 0;
	int ret;

	/* wait cmd fifo is empty */
	ret = readl_poll_timeout(nfc->reg_base + NFC_REG_CMD,
				cmd_size,
				!((cmd_size >> 22) & 0x1f),
				10, timeout_ms * 1000);
	if (ret)
		dev_err(nfc->dev, "wait for empty cmd FIFO time out\n");

	return ret;
}

static int meson_nfc_wait_dma_finish(struct meson_nfc *nfc)
{
	meson_nfc_cmd_idle(nfc, 0);
	meson_nfc_cmd_idle(nfc, 0);

	return meson_nfc_wait_cmd_finish(nfc, DMA_BUSY_TIMEOUT);
}

static inline struct meson_nfc_info_format *nfc_info_ptr(struct meson_nfc *nfc,
	int index)
{
	return (struct meson_nfc_info_format *) &nfc->info_buf[index * 8];
}

static u8 *meson_nfc_oob_ptr(struct meson_nfc *nfc, int i)
{
	int x, len;
	int ecc_bytes = nfc->param.ecc_bytes, temp = nfc->param.ecc_size;

	x = i ? 16 : 0;
	len = (nfc->param.oob_mode) ? (temp * (i + 1) + ecc_bytes * i + x) :
				(temp * (i + 1) + (ecc_bytes + 2) * i);

	return nfc->data_buf + len;
}

static u8 *meson_nfc_data_ptr(struct meson_nfc *nfc, int i)
{
	int len, x;
	int temp = nfc->param.ecc_size + nfc->param.ecc_bytes;

	x = i ? 16 : 0;
	len = nfc->param.oob_mode ? (temp * i + x) : (temp + 2) * i;

	return nfc->data_buf + len;
}

static void meson_nfc_prase_data_oob(struct meson_nfc *nfc, u8 *buf, u8 *oob)
{
	int i, oob_len = 0;
	u8 *dsrc, *osrc;

	for (i = 0; i < nfc->param.ecc_step; i++) {
		if (buf) {
			dsrc = meson_nfc_data_ptr(nfc, i);
			memcpy(buf, dsrc, nfc->param.ecc_size);
			buf += nfc->param.ecc_size;
		}

		if (nfc->param.oob_mode)
			oob_len = (i) ? nfc->param.ecc_bytes :
						nfc->param.ecc_bytes + 16;
		else
			oob_len = nfc->param.ecc_bytes + 2;

		osrc = meson_nfc_oob_ptr(nfc, i);
		memcpy(oob, osrc, oob_len);
		oob += oob_len;
	}
}

static void meson_nfc_format_data_oob(struct meson_nfc *nfc,
						const u8 *buf, u8 *oob)
{
	int i, oob_len = 0;
	u8 *dsrc, *osrc;

	for (i = 0; i < nfc->param.ecc_step; i++) {
		if (buf) {
			dsrc = meson_nfc_data_ptr(nfc, i);
			memcpy(dsrc, buf, nfc->param.ecc_size);
			buf += nfc->param.ecc_size;
		}

		if (nfc->param.oob_mode)
			oob_len = i ? nfc->param.ecc_bytes :
						nfc->param.ecc_bytes + 16;
		else
			oob_len = nfc->param.ecc_bytes + 2;

		osrc = meson_nfc_oob_ptr(nfc, i);
		memcpy(osrc, oob, oob_len);
		oob += oob_len;
	}
}

static int meson_nfc_queue_rb(struct meson_nfc *nfc)
{
	u32 cmd, cfg;
	int ret = 0;

	init_completion(&nfc->completion);

	cfg = readl(nfc->reg_base + NFC_REG_CFG);
	cfg |= (1 << 21);
	writel(cfg, nfc->reg_base + NFC_REG_CFG);

	meson_nfc_cmd_idle(nfc, NAND_TWB_TIME_CYCLE);
	cmd = nfc->param.chip_select | NFC_CMD_CLE | (NAND_CMD_STATUS & 0xff);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);
	meson_nfc_cmd_idle(nfc, NAND_TWB_TIME_CYCLE);

	cmd = NFC_CMD_RB | NFC_CMD_IO6 | (1 << 16) | (0x18 & 0x1f);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);
	meson_nfc_cmd_idle(nfc, 2);

	ret = wait_for_completion_timeout(&nfc->completion,
					msecs_to_jiffies(1000));
	if (ret == 0) {
		dev_err(nfc->dev, "wait nand irq timeout\n");
		ret = -1;
	}

	return ret;
}

static void meson_nfc_set_user_byte(struct mtd_info *mtd,
					struct nand_chip *chip, u8 *oob_buf)
{
	struct meson_nfc *nfc = nand_get_controller_data(chip);
	struct meson_nfc_info_format *info;
	int i, count;

	if (nfc->param.oob_mode) {
		memcpy(nfc->info_buf, oob_buf, 16);
		return;
	}

	for (i = 0, count = 0; i < chip->ecc.steps; i++, count += 2) {
		info = nfc_info_ptr(nfc, i);
		info->info_bytes =
			oob_buf[count] | (oob_buf[count + 1] << 8);
	}
}

static void meson_nfc_get_user_byte(struct mtd_info *mtd,
					struct nand_chip *chip, u8 *oob_buf)
{
	struct meson_nfc *nfc = nand_get_controller_data(chip);
	struct meson_nfc_info_format *info;
	int i, count;

	if (nfc->param.oob_mode) {
		memcpy(oob_buf,  nfc->info_buf, 16);
		return;
	}

	for (i = 0, count = 0; i < chip->ecc.steps; i++, count += 2) {
		info = nfc_info_ptr(nfc, i);
		oob_buf[count] = info->info_bytes & 0xff;
		oob_buf[count + 1] = (info->info_bytes >> 8) & 0xff;
	}
}

static int meson_nfc_ecc_correct(struct mtd_info *mtd,
						struct nand_chip *chip)
{
	struct meson_nfc *nfc = nand_get_controller_data(chip);
	struct meson_nfc_info_format *info;
	u32 bitflips = 0, i;
	u8 zero_cnt;

	for (i = 0; i < nfc->param.ecc_step; i++) {
		info = nfc_info_ptr(nfc, i);
		if (info->ecc.eccerr_cnt == 0x3f) {
			zero_cnt = info->zero_cnt & 0x3f;
			if (nfc->param.rand_mode
				&& (zero_cnt < nfc->param.ecc_max))
				return ECC_CHECK_RETURN_FF;
			mtd->ecc_stats.failed++;
			continue;
		}
		mtd->ecc_stats.corrected += info->ecc.eccerr_cnt;
		bitflips = max_t(u32, bitflips, info->ecc.eccerr_cnt);
	}

	return bitflips;
}

static inline u8 meson_nfc_read_byte(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct meson_nfc *nfc = nand_get_controller_data(chip);
	u32 cmd;

	cmd = nfc->param.chip_select | NFC_CMD_DRD | 0;
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	meson_nfc_cmd_idle(nfc, 0);
	meson_nfc_cmd_idle(nfc, 0);

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

	meson_nfc_cmd_idle(nfc, NAND_TWB_TIME_CYCLE);

	cmd = nfc->param.chip_select | NFC_CMD_DWR | (byte & 0xff);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	meson_nfc_cmd_idle(nfc, NAND_TWB_TIME_CYCLE);
	meson_nfc_cmd_idle(nfc, 0);

	meson_nfc_wait_cmd_finish(nfc, 1000);
}

static void meson_nfc_write_buf(struct mtd_info *mtd, const u8 *buf, int len)
{
	int i;

	for (i = 0; i < len; i++)
		meson_nfc_write_byte(mtd, buf[i]);
}

static int meson_nfc_write_page_sub(struct mtd_info *mtd,
		struct nand_chip *chip, const u8 *buf, int page, int raw)
{
	struct meson_nfc *nfc = nand_get_controller_data(chip);
	dma_addr_t daddr, iaddr;
	u32 cmd;
	int ret;

	nand_prog_page_begin_op(chip, page, 0, NULL, 0);

	daddr = dma_map_single(nfc->dev, (void *)nfc->data_buf,
			mtd->writesize + mtd->oobsize, DMA_TO_DEVICE);
	ret = dma_mapping_error(nfc->dev, daddr);
	if (ret) {
		dev_err(nfc->dev, "dma mapping error\n");
		return -EINVAL;
	}

	iaddr = dma_map_single(nfc->dev, (void *)nfc->info_buf,
			nfc->param.ecc_step * PER_INFO_BYTE, DMA_TO_DEVICE);
	ret = dma_mapping_error(nfc->dev, iaddr);
	if (ret) {
		dev_err(nfc->dev, "dma mapping error\n");
		return -EINVAL;
	}

	cmd = GENCMDDADDRL(NFC_CMD_ADL, daddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);
	cmd = GENCMDDADDRH(NFC_CMD_ADH, daddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	cmd = GENCMDIADDRL(NFC_CMD_AIL, iaddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);
	cmd = GENCMDIADDRH(NFC_CMD_AIH, iaddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	meson_nfc_cmd_seed(nfc, page);

	meson_nfc_cmd_m2n(nfc, raw);

	ret = meson_nfc_wait_dma_finish(nfc);

	dma_unmap_single(nfc->dev, daddr,
			mtd->writesize + mtd->oobsize, DMA_TO_DEVICE);
	dma_unmap_single(nfc->dev, iaddr,
			nfc->param.ecc_step * PER_INFO_BYTE, DMA_TO_DEVICE);

	return nand_prog_page_end_op(chip);
}

static int meson_nfc_write_page_raw(struct mtd_info *mtd,
	struct nand_chip *chip, const u8 *buf, int oob_required, int page)
{
	struct meson_nfc *nfc = nand_get_controller_data(chip);
	u8 *oob_buf = chip->oob_poi;

	meson_nfc_format_data_oob(nfc, buf, oob_buf);

	return meson_nfc_write_page_sub(mtd, chip, nfc->data_buf, page, 1);
}

static int meson_nfc_write_page_hwecc(struct mtd_info *mtd,
				    struct nand_chip *chip, const u8 *buf,
				    int oob_required, int page)
{
	struct meson_nfc *nfc = nand_get_controller_data(chip);
	u8 *oob_buf = chip->oob_poi;

	memcpy(nfc->data_buf, buf, mtd->writesize);
	meson_nfc_set_user_byte(mtd, chip, oob_buf);

	return meson_nfc_write_page_sub(mtd, chip, nfc->data_buf, page, 0);
}

static void meson_nfc_check_ecc_pages_valid(struct meson_nfc *nfc, int raw)
{
	struct meson_nfc_info_format *info;
	int neccpages, i;

	neccpages = raw ? 1 : nfc->param.ecc_step;

	for (i = 0; i < neccpages; i++) {
		info = nfc_info_ptr(nfc, neccpages - 1);
		if (info->ecc.completed == 0)
			dev_err(nfc->dev, "seems eccpage is invalid\n");
	}
}

static int meson_nfc_read_page_sub(struct mtd_info *mtd,
	struct nand_chip *chip, const u8 *buf, int page, int raw)
{
	struct meson_nfc *nfc = nand_get_controller_data(chip);
	dma_addr_t daddr, iaddr;
	u32 cmd;
	int ret;

	nand_read_page_op(chip, page, 0, NULL, 0);

	daddr = dma_map_single(nfc->dev, nfc->data_buf,
			mtd->writesize + mtd->oobsize, DMA_FROM_DEVICE);
	ret = dma_mapping_error(nfc->dev, daddr);
	if (ret) {
		dev_err(nfc->dev, "dma mapping error\n");
		return -EINVAL;
	}

	iaddr = dma_map_single(nfc->dev, nfc->info_buf,
			nfc->param.ecc_step * PER_INFO_BYTE, DMA_FROM_DEVICE);
	ret = dma_mapping_error(nfc->dev, iaddr);
	if (ret) {
		dev_err(nfc->dev, "dma mapping error\n");
		return -EINVAL;
	}

	cmd = GENCMDDADDRL(NFC_CMD_ADL, daddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);
	cmd = GENCMDDADDRH(NFC_CMD_ADH, daddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	cmd = GENCMDIADDRL(NFC_CMD_AIL, iaddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);
	cmd = GENCMDIADDRH(NFC_CMD_AIH, iaddr);
	writel(cmd, nfc->reg_base + NFC_REG_CMD);

	meson_nfc_cmd_seed(nfc, page);

	meson_nfc_cmd_n2m(nfc, raw);

	ret = meson_nfc_wait_dma_finish(nfc);

	meson_nfc_queue_rb(nfc);

	meson_nfc_check_ecc_pages_valid(nfc, raw);

	dma_unmap_single(nfc->dev, daddr,
			mtd->writesize + mtd->oobsize, DMA_FROM_DEVICE);
	dma_unmap_single(nfc->dev, iaddr,
			nfc->param.ecc_step * PER_INFO_BYTE, DMA_FROM_DEVICE);

	return ret;
}

static int meson_nfc_read_page_raw(struct mtd_info *mtd,
	struct nand_chip *chip, u8 *buf, int oob_required, int page)
{
	struct meson_nfc *nfc = nand_get_controller_data(chip);
	u8 *oob_buf = chip->oob_poi;
	int ret;

	ret = meson_nfc_read_page_sub(mtd, chip, nfc->data_buf, page, 1);
	if (ret)
		return ret;

	meson_nfc_prase_data_oob(nfc, buf, oob_buf);

	return 0;
}

static int meson_nfc_read_page_hwecc(struct mtd_info *mtd,
	struct nand_chip *chip, u8 *buf, int oob_required, int page)
{
	struct meson_nfc *nfc = nand_get_controller_data(chip);
	u8 *oob_buf = chip->oob_poi;
	int ret;

	ret = meson_nfc_read_page_sub(mtd, chip, nfc->data_buf, page, 0);
	if (ret)
		return ret;

	meson_nfc_get_user_byte(mtd, chip, oob_buf);

	ret = meson_nfc_ecc_correct(mtd, chip);
	if (ret == ECC_CHECK_RETURN_FF) {
		if (buf)
			memset(buf, 0xff, mtd->writesize);

		memset(oob_buf, 0xff, mtd->oobsize);
		return 0;
	}
	if (buf && (buf != nfc->data_buf))
		memcpy(buf, nfc->data_buf, mtd->writesize);

	return ret;
}

static int meson_nfc_read_oob_raw(struct mtd_info *mtd,
				struct nand_chip *chip, int page)
{
	return meson_nfc_read_page_raw(mtd, chip, NULL, 1, page);
}

static int meson_nfc_read_oob(struct mtd_info *mtd, struct nand_chip *chip,
				int page)
{
	return meson_nfc_read_page_hwecc(mtd, chip, NULL, 1, page);
}

static int meson_ooblayout_ecc(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct meson_nfc *nfc = nand_get_controller_data(chip);
	int free_oob;

	if (section > chip->ecc.steps)
		return -ERANGE;

	free_oob = nfc->param.oob_mode ? 16 : (chip->ecc.steps * 2);
	oobregion->offset = section * chip->ecc.bytes + free_oob;
	oobregion->length = chip->ecc.bytes;

	return 0;
}

static int meson_ooblayout_free(struct mtd_info *mtd, int section,
				   struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct meson_nfc *nfc = nand_get_controller_data(chip);

	if (section > chip->ecc.steps)
		return -ERANGE;

	oobregion->offset = 0;
	oobregion->length = nfc->param.oob_mode ? 16 : (chip->ecc.steps * 2);

	return 0;
}

static const struct mtd_ooblayout_ops meson_ooblayout_ops = {
	.ecc = meson_ooblayout_ecc,
	.free = meson_ooblayout_free,
};

static int meson_nfc_ecc_init(struct device *dev, struct mtd_info *mtd)
{
	struct nand_chip *nand = mtd_to_nand(mtd);
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	struct meson_nfc *nfc = nand_get_controller_data(nand);
	struct meson_nand_ecc *meson_ecc = nfc->data->ecc;
	int num = nfc->data->ecc_num;
	int nsectors, i, bytes;

	/* support only ecc hw mode */
	if (nand->ecc.mode != NAND_ECC_HW) {
		dev_err(dev, "ecc.mode not supported\n");
		return -EINVAL;
	}

	if (!nand->ecc.size || !nand->ecc.strength) {
		/* use datasheet requirements */
		nand->ecc.strength = nand->ecc_strength_ds;
		nand->ecc.size = nand->ecc_step_ds;
	}

	if (nand->ecc.options & NAND_ECC_MAXIMIZE) {
		nand->ecc.size = 1024;
		nsectors = mtd->writesize / nand->ecc.size;

		/* Reserve 2 bytes for each ecc page */
		if (meson_chip->user_mode == NFC_USER2_OOB_BYTES)
			bytes = mtd->oobsize - 2 * nsectors;
		else
			bytes = mtd->oobsize - 16;

		bytes /= nsectors;

		/* and bytes has to be even. */
		if (bytes % 2)
			bytes--;

		nand->ecc.strength = bytes * 8 / fls(8 * nand->ecc.size);
	} else {
		if (nand->ecc.strength > meson_ecc[num - 1].strength) {
			dev_err(dev, "not support ecc strength\n");
			return -EINVAL;
		}
	}

	for (i = 0; i < num; i++) {
		if ((meson_ecc[i].strength == 0xff)
			|| (nand->ecc.strength < meson_ecc[i].strength))
			break;
	}

	if (!i) {
		nand->ecc.strength = 0;
	} else {
		nand->ecc.strength = meson_ecc[i - 1].strength;
		nand->ecc.bytes = meson_ecc[i - 1].parity;
	}

	meson_chip->bch_mode = meson_ecc[i - 1].bch;

	if (nand->ecc.size != 512 && nand->ecc.size != 1024)
		return -EINVAL;

	nsectors = mtd->writesize / nand->ecc.size;
	bytes =(meson_chip->user_mode == NFC_USER2_OOB_BYTES) ? nsectors * 2 : 16;
	if (mtd->oobsize < (nand->ecc.bytes * nsectors + bytes))
		return -EINVAL;

	return 0;
}

static const char * sd_emmc_ext_clk0_parent_names[MUX_CLK_NUM_PARENTS];

static struct clk_regmap sd_emmc_c_ext_clk0_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = SD_EMMC_CLOCK,
		.mask = 0x3,
		.shift = 6,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "sd_emmc_c_nand_clk_mux",
		.ops = &clk_regmap_mux_ops,
		.parent_names = sd_emmc_ext_clk0_parent_names,
		.num_parents = ARRAY_SIZE(sd_emmc_ext_clk0_parent_names),
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap sd_emmc_c_ext_clk0_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = SD_EMMC_CLOCK,
		.shift = 0,
		.width = 6,
		.flags = CLK_DIVIDER_ROUND_CLOSEST | CLK_DIVIDER_ONE_BASED,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "sd_emmc_c_nand_clk_div",
		.ops = &clk_regmap_divider_ops,
		.parent_names = (const char *[]){ "sd_emmc_c_nand_clk_mux" },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static int meson_nfc_clk_init(struct meson_nfc *nfc)
{
	struct clk_regmap *mux = &sd_emmc_c_ext_clk0_sel;
	struct clk_regmap *div = &sd_emmc_c_ext_clk0_div;
	struct clk *clk;
	int i, ret;

	/* request core clock */
	nfc->core_clk = devm_clk_get(nfc->dev, "core");
	if (IS_ERR(nfc->core_clk)) {
		dev_err(nfc->dev, "failed to get core clk\n");
		return PTR_ERR(nfc->core_clk);
	}

	/* init SD_EMMC_CLOCK to sane defaults w/min clock rate */
	regmap_update_bits(nfc->reg_clk, 0,
			CLK_SELECT_NAND | CLK_ALWAYS_ON | CLK_DIV_MASK,
			CLK_SELECT_NAND | CLK_ALWAYS_ON | CLK_DIV_MASK);

	/* get the mux parents */
	for (i = 0; i < MUX_CLK_NUM_PARENTS; i++) {
		char name[16];

		snprintf(name, sizeof(name), "clkin%d", i);
		clk = devm_clk_get(nfc->dev, name);
		if (IS_ERR(clk)) {
			if (clk != ERR_PTR(-EPROBE_DEFER))
				dev_err(nfc->dev, "Missing clock %s\n", name);
			return PTR_ERR(clk);
		}

		sd_emmc_ext_clk0_parent_names[i] = __clk_get_name(clk);
	}

	mux->map = nfc->reg_clk;
	clk = devm_clk_register(nfc->dev, &mux->hw);
	if (WARN_ON(IS_ERR(clk)))
		return PTR_ERR(clk);

	div->map = nfc->reg_clk;
	nfc->device_clk = devm_clk_register(nfc->dev, &div->hw);
	if (WARN_ON(IS_ERR(nfc->device_clk)))
		return PTR_ERR(nfc->device_clk);

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

	return 0;
}

static void meson_nfc_disable_clk(struct meson_nfc *nfc)
{
	clk_disable_unprepare(nfc->device_clk);
	clk_disable_unprepare(nfc->core_clk);
}

static int meson_nfc_buffer_init(struct mtd_info *mtd)
{
	struct nand_chip *nand = mtd_to_nand(mtd);
	struct meson_nfc_nand_chip *meson_chip = to_meson_nand(nand);
	struct meson_nfc *nfc = nand_get_controller_data(nand);
	struct device *dev = nfc->dev;
	int info_bytes, page_bytes;
	int nsectors;

	nsectors = mtd->writesize / nand->ecc.size;
	info_bytes = nsectors * PER_INFO_BYTE;
	page_bytes = mtd->writesize + mtd->oobsize;

	if ((meson_chip->data_buf) && (meson_chip->info_buf))
		return 0;

	meson_chip->data_buf = devm_kzalloc(dev, page_bytes, GFP_KERNEL);
	if (!meson_chip->data_buf)
		return  -ENOMEM;

	meson_chip->info_buf = devm_kzalloc(dev, info_bytes, GFP_KERNEL);
	if (!meson_chip->info_buf)
		return  -ENOMEM;

	return 0;
}

static int meson_nfc_calc_set_timing(struct meson_nfc *nfc,
				int rc_min, int rea_max, int rhoh_min)
{
	int div, bt_min, bt_max, bus_timing;
	int ret;

	div = DIV_ROUND_UP((rc_min / 1000), NFC_CLK_CYCLE);
	ret = clk_set_rate(nfc->device_clk, 1000000000 / div);
	if (ret) {
		dev_err(nfc->dev, "failed to set nand clock rate\n");
		return ret;
	}

	bt_min = (rea_max + NFC_DEFAULT_DELAY) / div;
	bt_max = (NFC_DEFAULT_DELAY + rhoh_min + rc_min / 2) / div;

	bt_min = DIV_ROUND_UP(bt_min, 1000);
	bt_max = DIV_ROUND_UP(bt_max, 1000);

	if (bt_max < bt_min)
		return -EINVAL;

	bus_timing = (bt_min + bt_max) / 2 + 1;

	writel((1 << 21), nfc->reg_base + NFC_REG_CFG);
	writel((NFC_CLK_CYCLE - 1) | (bus_timing << 5),
				nfc->reg_base + NFC_REG_CFG);

	writel((1 << 31), nfc->reg_base + NFC_REG_CMD);

	return 0;
}

static int meson_nfc_setup_data_interface(struct mtd_info *mtd, int csline,
				const struct nand_data_interface *conf)
{
	struct nand_chip *nand = mtd_to_nand(mtd);
	struct meson_nfc *nfc = nand_get_controller_data(nand);
	const struct nand_sdr_timings *timings;

	timings = nand_get_sdr_timings(conf);
	if (IS_ERR(timings))
		return -ENOTSUPP;

	if (csline == NAND_DATA_IFACE_CHECK_ONLY)
		return 0;

	meson_nfc_calc_set_timing(nfc, timings->tRC_min,
				timings->tREA_max, timings->tRHOH_min);

	return 0;
}

static int meson_nfc_get_nand_chip_dts(struct meson_nfc *nfc,
		struct meson_nfc_nand_chip *chip, struct device_node *np)
{
	struct device *dev = nfc->dev;

	if (of_property_read_u32(np, "reg", &chip->cs)) {
		dev_err(dev, "can not get ce number\n");
		return -EINVAL;
	}

	if (chip->cs > MAX_CE_NUM) {
		dev_err(dev, "ce number is beyond\n");
		return -EINVAL;
	}

	if (of_property_read_u32(np, "meson-nand-user-mode", &chip->user_mode)) {
		dev_err(dev, "can not get user oob mode\n");
		return -EINVAL;
	}

	if ((chip->user_mode != NFC_USER2_OOB_BYTES)
			|| (chip->user_mode != NFC_USER16_OOB_BYTES))
		chip->user_mode = NFC_USER2_OOB_BYTES;

	if (of_property_read_u32(np, "meson-nand-ran-mode", &chip->rand_mode)) {
		dev_err(dev, "can not get scramble mode\n");
		return -EINVAL;
	}

	return 0;
}

static int meson_nfc_nand_chip_init(struct device *dev, struct meson_nfc *nfc,
				  struct device_node *np)
{
	struct meson_nfc_nand_chip *chip;
	struct nand_chip *nand;
	struct mtd_info *mtd;
	int ret;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	ret = meson_nfc_get_nand_chip_dts(nfc, chip, np);
	if (ret)
		return ret;

	nand = &chip->nand;
	nand_set_flash_node(nand, np);
	nand_set_controller_data(nand, nfc);

	nand->options |= NAND_USE_BOUNCE_BUFFER;
	nand->select_chip = meson_nfc_select_chip;
	nand->write_byte = meson_nfc_write_byte;
	nand->write_buf = meson_nfc_write_buf;
	nand->read_byte = meson_nfc_read_byte;
	nand->read_buf = meson_nfc_read_buf;
	nand->cmd_ctrl = meson_nfc_cmd_ctrl;
	nand->setup_data_interface = meson_nfc_setup_data_interface;

	nand->chip_delay = 200;
	nand->ecc.mode = NAND_ECC_HW;

	nand->ecc.write_page_raw = meson_nfc_write_page_raw;
	nand->ecc.write_page = meson_nfc_write_page_hwecc;
	nand->ecc.write_oob_raw = nand_write_oob_std;
	nand->ecc.write_oob = nand_write_oob_std;

	nand->ecc.read_page_raw = meson_nfc_read_page_raw;
	nand->ecc.read_page = meson_nfc_read_page_hwecc;
	nand->ecc.read_oob_raw = meson_nfc_read_oob_raw;
	nand->ecc.read_oob = meson_nfc_read_oob;

	mtd = nand_to_mtd(nand);
	mtd->owner = THIS_MODULE;
	mtd->dev.parent = dev;
	mtd->name = devm_kasprintf(nfc->dev, GFP_KERNEL,
				   "%s:nand", dev_name(dev));
	if (!mtd->name) {
		dev_err(nfc->dev, "Failed to allocate mtd->name\n");
		return -ENOMEM;
	}

	mtd_set_ooblayout(mtd, &meson_ooblayout_ops);

	ret = nand_scan_ident(mtd, 1, NULL);
	if (ret) {
		dev_err(dev, "failed to can ident\n");
		return -ENODEV;
	}

	/* store bbt magic in page, cause OOB is not protected */
	if (nand->bbt_options & NAND_BBT_USE_FLASH)
		nand->bbt_options |= NAND_BBT_NO_OOB;

	nand->options |= NAND_NO_SUBPAGE_WRITE;

	ret = meson_nfc_ecc_init(dev, mtd);
	if (ret) {
		dev_err(dev, "failed to ecc init\n");
		return -EINVAL;
	}

	if (nand->options & NAND_BUSWIDTH_16) {
		dev_err(dev, "16bits buswidth not supported");
		return -EINVAL;
	}

	ret = meson_nfc_buffer_init(mtd);
	if (ret)
		return -ENOMEM;

	ret = nand_scan_tail(mtd);
	if (ret)
		return -ENODEV;

	ret = mtd_device_register(mtd, NULL, 0);
	if (ret) {
		dev_err(dev, "failed to register mtd device: %d\n", ret);
		nand_release(mtd);
		return ret;
	}

	list_add_tail(&chip->node, &nfc->chips);

	return 0;
}

static int meson_nfc_nand_chips_init(struct device *dev, struct meson_nfc *nfc)
{
	struct device_node *np = dev->of_node;
	struct device_node *nand_np;
	int ret;

	for_each_child_of_node(np, nand_np) {
		ret = meson_nfc_nand_chip_init(dev, nfc, nand_np);
		if (ret) {
			of_node_put(nand_np);
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
	cfg |= (1 << 21);
	writel(cfg, nfc->reg_base + NFC_REG_CFG);

	complete(&nfc->completion);
	return IRQ_HANDLED;
}

static const struct meson_nfc_data meson_gxl_data = {
	.short_bch	= NFC_ECC_BCH60_1K,
	.ecc		= meson_gxl_ecc,
	.ecc_num	= ARRAY_SIZE(meson_gxl_ecc),
};

static const struct meson_nfc_data meson_axg_data = {
	.short_bch	= NFC_ECC_BCH8_1K,
	.ecc		= meson_axg_ecc,
	.ecc_num	= ARRAY_SIZE(meson_axg_ecc),
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
	const struct of_device_id *of_nfc_id;
	int ret, irq;

	nfc = devm_kzalloc(dev, sizeof(*nfc), GFP_KERNEL);
	if (!nfc)
		return -ENOMEM;

	of_nfc_id = of_match_device(meson_nfc_id_table, &pdev->dev);
	if (!of_nfc_id)
		return -ENODEV;

	nfc->data = (struct meson_nfc_data *)of_nfc_id->data;

	spin_lock_init(&nfc->controller.lock);
	init_waitqueue_head(&nfc->controller.wq);
	INIT_LIST_HEAD(&nfc->chips);

	nfc->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "Failed to nfc reg resource\n");
		return -EINVAL;
	}

	nfc->reg_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(nfc->reg_base)) {
		dev_err(dev, "Failed to lookup nfi reg base\n");
		return PTR_ERR(nfc->reg_base);
	}

	nfc->reg_clk = syscon_regmap_lookup_by_phandle(dev->of_node,
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
		goto err_clk;
	}

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

	meson_nfc_page0_gen(nfc);
	return 0;

err_clk:
	clk_disable_unprepare(nfc->device_clk);
	clk_disable_unprepare(nfc->core_clk);

	return ret;
}

static int meson_nfc_remove(struct platform_device *pdev)
{
	struct meson_nfc *nfc = platform_get_drvdata(pdev);
	struct meson_nfc_nand_chip *chip;

	while (!list_empty(&nfc->chips)) {
		chip = list_first_entry(&nfc->chips, struct meson_nfc_nand_chip,
					node);
		nand_release(nand_to_mtd(&chip->nand));
		list_del(&chip->node);
	}

	meson_nfc_disable_clk(nfc);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver meson_nfc_driver = {
	.probe  = meson_nfc_probe,
	.remove = meson_nfc_remove,
	.driver = {
		.name  = "meson_nand",
		.of_match_table = meson_nfc_id_table,
	},
};

module_platform_driver(meson_nfc_driver);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Liang Yang <liang.yang@amlogic.com>");
MODULE_DESCRIPTION("Amlogic's Meson NAND Flash Controller driver");
