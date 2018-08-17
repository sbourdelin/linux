// SPDX-License-Identifier: GPL-2.0
/*
 * Arasan NAND Flash Controller Driver
 *
 * Copyright (C) 2014 - 2017 Xilinx, Inc.
 * Author: Punnaiah Choudary Kalluri <punnaia@xilinx.com>
 * Author: Naga Sureshkumar Relli <nagasure@xilinx.com>
 *
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/mtd/partitions.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#define DRIVER_NAME			"arasan_nand"
#define EVNT_TIMEOUT_MSEC		1000
#define STATUS_TIMEOUT			2000

#define PKT_OFST			0x00
#define MEM_ADDR1_OFST			0x04
#define MEM_ADDR2_OFST			0x08
#define CMD_OFST			0x0C
#define PROG_OFST			0x10
#define INTR_STS_EN_OFST		0x14
#define INTR_SIG_EN_OFST		0x18
#define INTR_STS_OFST			0x1C
#define READY_STS_OFST			0x20
#define DMA_ADDR1_OFST			0x24
#define FLASH_STS_OFST			0x28
#define DATA_PORT_OFST			0x30
#define ECC_OFST			0x34
#define ECC_ERR_CNT_OFST		0x38
#define ECC_SPR_CMD_OFST		0x3C
#define ECC_ERR_CNT_1BIT_OFST		0x40
#define ECC_ERR_CNT_2BIT_OFST		0x44
#define DMA_ADDR0_OFST			0x50
#define DATA_INTERFACE_OFST		0x6C

#define PKT_CNT_SHIFT			12

#define ECC_ENABLE			BIT(31)
#define DMA_EN_MASK			GENMASK(27, 26)
#define DMA_ENABLE			0x2
#define DMA_EN_SHIFT			26
#define REG_PAGE_SIZE_SHIFT		23
#define REG_PAGE_SIZE_512		0
#define REG_PAGE_SIZE_1K		5
#define REG_PAGE_SIZE_2K		1
#define REG_PAGE_SIZE_4K		2
#define REG_PAGE_SIZE_8K		3
#define REG_PAGE_SIZE_16K		4
#define CMD2_SHIFT			8
#define ADDR_CYCLES_SHIFT		28

#define XFER_COMPLETE			BIT(2)
#define READ_READY			BIT(1)
#define WRITE_READY			BIT(0)
#define MBIT_ERROR			BIT(3)

#define PROG_PGRD			BIT(0)
#define PROG_ERASE			BIT(2)
#define PROG_STATUS			BIT(3)
#define PROG_PGPROG			BIT(4)
#define PROG_RDID			BIT(6)
#define PROG_RDPARAM			BIT(7)
#define PROG_RST			BIT(8)
#define PROG_GET_FEATURE		BIT(9)
#define PROG_SET_FEATURE		BIT(10)

#define PG_ADDR_SHIFT			16
#define BCH_MODE_SHIFT			25
#define BCH_EN_SHIFT			27
#define ECC_SIZE_SHIFT			16

#define MEM_ADDR_MASK			GENMASK(7, 0)
#define BCH_MODE_MASK			GENMASK(27, 25)

#define CS_MASK				GENMASK(31, 30)
#define CS_SHIFT			30

#define PAGE_ERR_CNT_MASK		GENMASK(16, 8)
#define PKT_ERR_CNT_MASK		GENMASK(7, 0)

#define NVDDR_MODE			BIT(9)
#define NVDDR_TIMING_MODE_SHIFT		3

#define ONFI_ID_LEN			8
#define TEMP_BUF_SIZE			1024
#define NVDDR_MODE_PACKET_SIZE		8
#define SDR_MODE_PACKET_SIZE		4

#define ONFI_DATA_INTERFACE_NVDDR      BIT(4)
#define EVENT_MASK	(XFER_COMPLETE | READ_READY | WRITE_READY | MBIT_ERROR)

#define SDR_MODE_DEFLT_FREQ		80000000
#define COL_ROW_ADDR(pos, val)			(((val) & 0xFF) << (8 * (pos)))

struct anfc_op {
	s32 cmnds[4];
	u32 type;
	u32 len;
	u32 naddrs;
	u32 col;
	u32 row;
	unsigned int data_instr_idx;
	unsigned int rdy_timeout_ms;
	unsigned int rdy_delay_ns;
	const struct nand_op_instr *data_instr;
};

/**
 * struct anfc_nand_chip - Defines the nand chip related information
 * @node:		used to store NAND chips into a list.
 * @chip:		NAND chip information structure.
 * @bch:		Bch / Hamming mode enable/disable.
 * @bchmode:		Bch mode.
 * @eccval:		Ecc config value.
 * @raddr_cycles:	Row address cycle information.
 * @caddr_cycles:	Column address cycle information.
 * @pktsize:		Packet size for read / write operation.
 * @csnum:		chipselect number to be used.
 * @spktsize:		Packet size in ddr mode for status operation.
 * @inftimeval:		Data interface and timing mode information
 */
struct anfc_nand_chip {
	struct list_head node;
	struct nand_chip chip;
	bool bch;
	u32 bchmode;
	u32 eccval;
	u16 raddr_cycles;
	u16 caddr_cycles;
	u32 pktsize;
	int csnum;
	u32 spktsize;
	u32 inftimeval;
};

/**
 * struct anfc_nand_controller - Defines the Arasan NAND flash controller
 *				 driver instance
 * @controller:		base controller structure.
 * @chips:		list of all nand chips attached to the ctrler.
 * @dev:		Pointer to the device structure.
 * @base:		Virtual address of the NAND flash device.
 * @curr_cmd:		Current command issued.
 * @clk_sys:		Pointer to the system clock.
 * @clk_flash:		Pointer to the flash clock.
 * @dma:		Dma enable/disable.
 * @iswriteoob:		Identifies if oob write operation is required.
 * @buf:		Buffer used for read/write byte operations.
 * @irq:		irq number
 * @bufshift:		Variable used for indexing buffer operation
 * @csnum:		Chip select number currently inuse.
 * @event:		Completion event for nand status events.
 * @status:		Status of the flash device.
 * @prog:		Used to initiate controller operations.
 */
struct anfc_nand_controller {
	struct nand_controller controller;
	struct list_head chips;
	struct device *dev;
	void __iomem *base;
	int curr_cmd;
	struct clk *clk_sys;
	struct clk *clk_flash;
	bool dma;
	bool iswriteoob;
	int irq;
	int csnum;
	struct completion event;
	int status;
	u32 prog;
};

static int anfc_ooblayout_ecc(struct mtd_info *mtd, int section,
			      struct mtd_oob_region *oobregion)
{
	struct nand_chip *nand = mtd_to_nand(mtd);

	if (section >= nand->ecc.steps)
		return -ERANGE;

	if (section)
		return -ERANGE;

	oobregion->length = nand->ecc.total;
	oobregion->offset = mtd->oobsize - oobregion->length;

	return 0;
}

static int anfc_ooblayout_free(struct mtd_info *mtd, int section,
			       struct mtd_oob_region *oobregion)
{
	struct nand_chip *nand = mtd_to_nand(mtd);

	if (section >= nand->ecc.steps)
		return -ERANGE;

	if (section)
		return -ERANGE;

	oobregion->offset = 2;
	oobregion->length = mtd->oobsize - nand->ecc.total - 2;

	return 0;
}

static const struct mtd_ooblayout_ops anfc_ooblayout_ops = {
	.ecc = anfc_ooblayout_ecc,
	.free = anfc_ooblayout_free,
};

static inline struct anfc_nand_chip *to_anfc_nand(struct nand_chip *nand)
{
	return container_of(nand, struct anfc_nand_chip, chip);
}

static inline struct anfc_nand_controller *to_anfc(struct nand_controller *ctrl)
{
	return container_of(ctrl, struct anfc_nand_controller, controller);
}

static u8 anfc_page(u32 pagesize)
{
	switch (pagesize) {
	case 512:
		return REG_PAGE_SIZE_512;
	case 1024:
		return REG_PAGE_SIZE_1K;
	case 2048:
		return REG_PAGE_SIZE_2K;
	case 4096:
		return REG_PAGE_SIZE_4K;
	case 8192:
		return REG_PAGE_SIZE_8K;
	case 16384:
		return REG_PAGE_SIZE_16K;
	default:
		break;
	}

	return 0;
}

static inline void anfc_enable_intrs(struct anfc_nand_controller *nfc, u32 val)
{
	writel(val, nfc->base + INTR_STS_EN_OFST);
	writel(val, nfc->base + INTR_SIG_EN_OFST);
}

static inline void anfc_config_ecc(struct anfc_nand_controller *nfc, bool on)
{
	u32 val;

	val = readl(nfc->base + CMD_OFST);
	if (on)
		val |= ECC_ENABLE;
	else
		val &= ~ECC_ENABLE;
	writel(val, nfc->base + CMD_OFST);
}

static inline void anfc_config_dma(struct anfc_nand_controller *nfc, int on)
{
	u32 val;

	val = readl(nfc->base + CMD_OFST);
	val &= ~DMA_EN_MASK;
	if (on)
		val |= DMA_ENABLE << DMA_EN_SHIFT;
	writel(val, nfc->base + CMD_OFST);
}

static inline int anfc_wait_for_event(struct anfc_nand_controller *nfc)
{
	return wait_for_completion_timeout(&nfc->event,
					msecs_to_jiffies(EVNT_TIMEOUT_MSEC));
}

static inline void anfc_setpktszcnt(struct anfc_nand_controller *nfc,
				    u32 pktsize, u32 pktcount)
{
	writel(pktsize | (pktcount << PKT_CNT_SHIFT), nfc->base + PKT_OFST);
}

static inline void anfc_set_eccsparecmd(struct anfc_nand_controller *nfc,
					struct anfc_nand_chip *achip, u8 cmd1,
					u8 cmd2)
{
	writel(cmd1 | (cmd2 << CMD2_SHIFT) |
	       (achip->caddr_cycles << ADDR_CYCLES_SHIFT),
	       nfc->base + ECC_SPR_CMD_OFST);
}

static void anfc_setpagecoladdr(struct anfc_nand_controller *nfc, u32 page,
				u16 col)
{
	u32 val;

	writel(col | (page << PG_ADDR_SHIFT), nfc->base + MEM_ADDR1_OFST);

	val = readl(nfc->base + MEM_ADDR2_OFST);
	val = (val & ~MEM_ADDR_MASK) |
	      ((page >> PG_ADDR_SHIFT) & MEM_ADDR_MASK);
	writel(val, nfc->base + MEM_ADDR2_OFST);
}

static void anfc_prepare_cmd(struct anfc_nand_controller *nfc, u8 cmd1,
			     u8 cmd2, u8 dmamode,
			     u32 pagesize, u8 addrcycles)
{
	u32 regval;

	regval = cmd1 | (cmd2 << CMD2_SHIFT);
	if (dmamode && nfc->dma)
		regval |= DMA_ENABLE << DMA_EN_SHIFT;
	regval |= addrcycles << ADDR_CYCLES_SHIFT;
	regval |= anfc_page(pagesize) << REG_PAGE_SIZE_SHIFT;
	writel(regval, nfc->base + CMD_OFST);
}

static void anfc_rw_dma_op(struct mtd_info *mtd, uint8_t *buf, int len,
			    bool do_read, u32 prog)
{
	dma_addr_t paddr;
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct anfc_nand_controller *nfc = to_anfc(chip->controller);
	struct anfc_nand_chip *achip = to_anfc_nand(chip);
	u32 eccintr = 0, dir;
	u32 pktsize = len, pktcount = 1;

	if (((nfc->curr_cmd == NAND_CMD_READ0)) ||
	    (nfc->curr_cmd == NAND_CMD_SEQIN && !nfc->iswriteoob)) {
		pktsize = achip->pktsize;
		pktcount = DIV_ROUND_UP(mtd->writesize, pktsize);
	}
	anfc_setpktszcnt(nfc, pktsize, pktcount);

	if (!achip->bch && nfc->curr_cmd == NAND_CMD_READ0)
		eccintr = MBIT_ERROR;

	if (do_read)
		dir = DMA_FROM_DEVICE;
	else
		dir = DMA_TO_DEVICE;

	paddr = dma_map_single(nfc->dev, buf, len, dir);
	if (dma_mapping_error(nfc->dev, paddr)) {
		dev_err(nfc->dev, "Read buffer mapping error");
		return;
	}
	writel(paddr, nfc->base + DMA_ADDR0_OFST);
	writel((paddr >> 32), nfc->base + DMA_ADDR1_OFST);
	anfc_enable_intrs(nfc, (XFER_COMPLETE | eccintr));
	writel(prog, nfc->base + PROG_OFST);
	anfc_wait_for_event(nfc);
	dma_unmap_single(nfc->dev, paddr, len, dir);
}

static void anfc_rw_pio_op(struct mtd_info *mtd, uint8_t *buf, int len,
			    bool do_read, int prog)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct anfc_nand_controller *nfc = to_anfc(chip->controller);
	struct anfc_nand_chip *achip = to_anfc_nand(chip);
	u32 *bufptr = (u32 *)buf;
	u32 cnt = 0, intr = 0;
	u32 pktsize = len, pktcount = 1;

	anfc_config_dma(nfc, 0);

	if (((nfc->curr_cmd == NAND_CMD_READ0)) ||
	    (nfc->curr_cmd == NAND_CMD_SEQIN && !nfc->iswriteoob)) {
		pktsize = achip->pktsize;
		pktcount = DIV_ROUND_UP(mtd->writesize, pktsize);
	}
	anfc_setpktszcnt(nfc, pktsize, pktcount);

	if (!achip->bch && nfc->curr_cmd == NAND_CMD_READ0)
		intr = MBIT_ERROR;

	if (do_read)
		intr |= READ_READY;
	else
		intr |= WRITE_READY;

	anfc_enable_intrs(nfc, intr);
	writel(prog, nfc->base + PROG_OFST);
	while (cnt < pktcount) {

		anfc_wait_for_event(nfc);
		cnt++;
		if (cnt == pktcount)
			anfc_enable_intrs(nfc, XFER_COMPLETE);
		if (do_read)
			ioread32_rep(nfc->base + DATA_PORT_OFST, bufptr,
				     pktsize / 4);
		else
			iowrite32_rep(nfc->base + DATA_PORT_OFST, bufptr,
				      pktsize / 4);
		bufptr += (pktsize / 4);
		if (cnt < pktcount)
			anfc_enable_intrs(nfc, intr);
	}
	anfc_wait_for_event(nfc);
}

static void anfc_read_data_op(struct mtd_info *mtd, uint8_t *buf, int len)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct anfc_nand_controller *nfc = to_anfc(chip->controller);

	if (nfc->dma && !is_vmalloc_addr(buf))
		anfc_rw_dma_op(mtd, buf, len, 1, PROG_PGRD);
	else
		anfc_rw_pio_op(mtd, buf, len, 1, PROG_PGRD);
}

static void anfc_write_data_op(struct mtd_info *mtd, const uint8_t *buf,
			       int len)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct anfc_nand_controller *nfc = to_anfc(chip->controller);

	if (nfc->dma && !is_vmalloc_addr(buf))
		anfc_rw_dma_op(mtd, (char *)buf, len, 0, PROG_PGPROG);
	else
		anfc_rw_pio_op(mtd, (char *)buf, len, 0, PROG_PGPROG);
}

static int anfc_prep_nand_instr(struct mtd_info *mtd, int cmd,
				struct nand_chip *chip,  int col, int page)
{
	u8 addrs[5];

	struct nand_op_instr instrs[] = {
		NAND_OP_CMD(cmd, PSEC_TO_NSEC(1)),
		NAND_OP_ADDR(3, addrs, 0),
	};
	struct nand_operation op = NAND_OPERATION(instrs);

	if (mtd->writesize <= 512) {
		addrs[0] = col;
		if (page != -1) {
			addrs[1] = page;
			addrs[2] = page >> 8;
			instrs[1].ctx.addr.naddrs = 3;
			if (chip->options & NAND_ROW_ADDR_3) {
				addrs[3] = page >> 16;
				instrs[1].ctx.addr.naddrs += 1;
			}
		} else {
			instrs[1].ctx.addr.naddrs = 1;
		}
	} else {
		addrs[0] = col;
		addrs[1] = col >> 8;
		if (page != -1) {
			addrs[2] = page;
			addrs[3] = page >> 8;
			instrs[1].ctx.addr.naddrs = 4;
			if (chip->options & NAND_ROW_ADDR_3) {
				addrs[4] = page >> 16;
				instrs[1].ctx.addr.naddrs += 1;
			}
		} else {
			instrs[1].ctx.addr.naddrs = 2;
		}
	}

	return nand_exec_op(chip, &op);
}

static int anfc_nand_wait(struct mtd_info *mtd, struct nand_chip *chip)
{
	u8 status;
	int ret;
	unsigned long timeo;

	/*
	 * Apply this short delay always to ensure that we do wait tWB in any
	 * case on any machine.
	 */
	ndelay(100);
	timeo = jiffies + msecs_to_jiffies(STATUS_TIMEOUT);
	do {
		ret = nand_status_op(chip, &status);
		if (ret)
			return ret;

		if (status & NAND_STATUS_READY)
			break;
		cond_resched();
	} while (time_before(jiffies, timeo));


	return status;
}

static int anfc_write_oob(struct mtd_info *mtd, struct nand_chip *chip,
			  int page)
{
	struct anfc_nand_controller *nfc = to_anfc(chip->controller);

	nfc->iswriteoob = true;
	anfc_prep_nand_instr(mtd, NAND_CMD_SEQIN, chip, mtd->writesize, page);
	anfc_write_data_op(mtd, chip->oob_poi, mtd->oobsize);
	nfc->iswriteoob = false;

	return 0;
}

static int anfc_read_oob(struct mtd_info *mtd, struct nand_chip *chip,
			  int page)
{
	anfc_prep_nand_instr(mtd, NAND_CMD_READOOB, chip, 0, page);
	anfc_read_data_op(mtd, chip->oob_poi, mtd->oobsize);

	return 0;
}


static int anfc_read_page_hwecc(struct mtd_info *mtd,
				struct nand_chip *chip, uint8_t *buf,
				int oob_required, int page)
{
	u32 val;
	struct anfc_nand_controller *nfc = to_anfc(chip->controller);
	struct anfc_nand_chip *achip = to_anfc_nand(chip);
	u8 *ecc_code = chip->ecc.code_buf;
	u8 *p = buf;
	int eccsize = chip->ecc.size;
	int eccbytes = chip->ecc.bytes;
	int stat = 0, i;
	u32 ret;
	unsigned int max_bitflips = 0;

	ret = nand_read_page_op(chip, page, 0, NULL, 0);
	if (ret)
		return ret;

	anfc_set_eccsparecmd(nfc, achip, NAND_CMD_RNDOUT, NAND_CMD_RNDOUTSTART);
	anfc_config_ecc(nfc, true);
	anfc_read_data_op(mtd, buf, mtd->writesize);

	if (achip->bch) {
		/*
		 * Arasan NAND controller can correct ECC upto 24-bit
		 * Beyond that, it can't detect errors, so no fail count
		 * updated here.
		 */
		val = readl(nfc->base + ECC_ERR_CNT_OFST);
		val = ((val & PAGE_ERR_CNT_MASK) >> 8);
		mtd->ecc_stats.corrected += val;
	} else {
		val = readl(nfc->base + ECC_ERR_CNT_1BIT_OFST);
		mtd->ecc_stats.corrected += val;
		val = readl(nfc->base + ECC_ERR_CNT_2BIT_OFST);
		mtd->ecc_stats.failed += val;
		/* Clear ecc error count register 1Bit, 2Bit */
		writel(0x0, nfc->base + ECC_ERR_CNT_1BIT_OFST);
		writel(0x0, nfc->base + ECC_ERR_CNT_2BIT_OFST);
	}

	if (oob_required)
		chip->ecc.read_oob(mtd, chip, page);

	anfc_config_ecc(nfc, false);

	if (val) {
		if (!oob_required)
			chip->ecc.read_oob(mtd, chip, page);

		mtd_ooblayout_get_eccbytes(mtd, ecc_code, chip->oob_poi, 0,
					   chip->ecc.total);
		for (i = 0; i < chip->ecc.steps; ++i) {
			stat = nand_check_erased_ecc_chunk(p,
							   chip->ecc.size,
							   &ecc_code[i],
							   eccbytes,
							   NULL, 0,
							   chip->ecc.strength);
			if (stat < 0) {
				/*
				 * No fail count updated here, because the data
				 * is already corrected and ecc_stats.corrected
				 * is updated and in case of haming,
				 * ecc_stats.failed is updated.
				 */
				stat = 0;
			} else {
				mtd->ecc_stats.corrected += stat;
			}
			max_bitflips = max_t(unsigned int, max_bitflips, stat);
			p += eccsize;
		}
	}

	return max_bitflips;
}


static int anfc_write_page_hwecc(struct mtd_info *mtd,
				 struct nand_chip *chip, const uint8_t *buf,
				 int oob_required, int page)
{
	int ret;
	struct anfc_nand_controller *nfc = to_anfc(chip->controller);
	struct anfc_nand_chip *achip = to_anfc_nand(chip);
	u8 status;
	u8 *ecc_calc = chip->ecc.calc_buf;

	ret = nand_prog_page_begin_op(chip, page, 0, NULL, 0);
	if (ret)
		return ret;

	anfc_set_eccsparecmd(nfc, achip, NAND_CMD_RNDIN, 0);
	anfc_config_ecc(nfc, true);
	anfc_write_data_op(mtd, buf, mtd->writesize);

	if (oob_required) {
		status = anfc_nand_wait(mtd, chip);
		if (status & NAND_STATUS_FAIL)
			return -EIO;

		anfc_prep_nand_instr(mtd, NAND_CMD_READOOB, chip, 0, page);
		anfc_read_data_op(mtd, ecc_calc, mtd->oobsize);
		ret = mtd_ooblayout_set_eccbytes(mtd, ecc_calc, chip->oob_poi,
						 0, chip->ecc.total);
		if (ret)
			return ret;

		chip->ecc.write_oob(mtd, chip, page);
	}
	status = anfc_nand_wait(mtd, chip);
	if (status & NAND_STATUS_FAIL)
		return -EIO;

	anfc_config_ecc(nfc, false);

	return 0;
}

/**
 * anfc_get_mode_frm_timings - Converts sdr timing values to respective modes
 * @sdr: SDR NAND chip timings structure
 * Arasan NAND controller has Data Interface Register (0x6C)
 * which has timing mode configurations and need to program only the modes but
 * not timings. So this function returns SDR timing mode from SDR timing values
 *
 * Return: 	SDR timing mode on success, a negative error code otherwise.
 */
static int anfc_get_mode_frm_timings(const struct nand_sdr_timings *sdr)
{
	if (sdr->tRC_min <= 20000)
		return 5;
	else if (sdr->tRC_min <= 25000)
		return 4;
	else if (sdr->tRC_min <= 30000)
		return 3;
	else if (sdr->tRC_min <= 35000)
		return 2;
	else if (sdr->tRC_min <= 50000)
		return 1;
	else if (sdr->tRC_min <= 100000)
		return 0;
	else
		return -1;
}

static int anfc_ecc_init(struct mtd_info *mtd,
			 struct nand_ecc_ctrl *ecc, int ecc_mode)
{
	u32 ecc_addr;
	unsigned int bchmode, steps;
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct anfc_nand_chip *achip = to_anfc_nand(chip);

	ecc->write_oob = anfc_write_oob;
	ecc->read_oob = anfc_read_oob;
	ecc->mode = NAND_ECC_HW;
	ecc->read_page = anfc_read_page_hwecc;
	ecc->write_page = anfc_write_page_hwecc;

	mtd_set_ooblayout(mtd, &anfc_ooblayout_ops);

	steps = mtd->writesize / chip->ecc_step_ds;

	switch (chip->ecc_strength_ds) {
	case 12:
		bchmode = 0x1;
		break;
	case 8:
		bchmode = 0x2;
		break;
	case 4:
		bchmode = 0x3;
		break;
	case 24:
		bchmode = 0x4;
		break;
	default:
		bchmode = 0x0;
	}
	if (!bchmode)
		ecc->total = 3 * steps;
	else
		ecc->total =
		     DIV_ROUND_UP(fls(8 * chip->ecc_step_ds) *
			 chip->ecc_strength_ds * steps, 8);

	ecc->strength = chip->ecc_strength_ds;
	ecc->size = chip->ecc_step_ds;
	ecc->bytes = ecc->total / steps;
	ecc->steps = steps;
	achip->bchmode = bchmode;
	achip->bch = achip->bchmode;
	ecc_addr = mtd->writesize + (mtd->oobsize - ecc->total);

	achip->eccval = ecc_addr | (ecc->total << ECC_SIZE_SHIFT) |
			(achip->bch << BCH_EN_SHIFT);

	if (chip->ecc_step_ds >= 1024)
		achip->pktsize = 1024;
	else
		achip->pktsize = 512;

	return 0;
}

/* NAND framework ->exec_op() hooks and related helpers */
static void anfc_parse_instructions(struct nand_chip *chip,
					 const struct nand_subop *subop,
					 struct anfc_op *nfc_op)
{
	const struct nand_op_instr *instr = NULL;
	struct anfc_nand_controller *nfc = to_anfc(chip->controller);
	unsigned int op_id;
	int i = 0;
	const u8 *addrs;

	memset(nfc_op, 0, sizeof(struct anfc_op));

	/*
	 * This is to keep track of status command. some times
	 * core will just request status byte to read. so to make
	 * sure that no command is issued, cmnds[0] is assigned to
	 * NAND_CMD_NONE.
	 */
	nfc_op->cmnds[0] = NAND_CMD_NONE;
	for (op_id = 0; op_id < subop->ninstrs; op_id++) {
		instr = &subop->instrs[op_id];
		switch (instr->type) {
		case NAND_OP_CMD_INSTR:
			nfc_op->type = NAND_OP_CMD_INSTR;
			if (op_id)
				nfc_op->cmnds[1] = instr->ctx.cmd.opcode;
			else
				nfc_op->cmnds[0] = instr->ctx.cmd.opcode;
			nfc->curr_cmd = nfc_op->cmnds[0];
			break;

		case NAND_OP_ADDR_INSTR:
			i = nand_subop_get_addr_start_off(subop, op_id);
			nfc_op->naddrs = nand_subop_get_num_addr_cyc(subop,
								     op_id);
			addrs = &instr->ctx.addr.addrs[i];

			for (; i < nfc_op->naddrs; i++) {
				u8 val = instr->ctx.addr.addrs[i];

				if (nfc_op->cmnds[0] == NAND_CMD_ERASE1)
					nfc_op->row |= COL_ROW_ADDR(i, val);
				else {
					if (i < 2)
						nfc_op->col |= COL_ROW_ADDR(
								i, val);
					else
						nfc_op->row |= COL_ROW_ADDR(
								i - 2, val);
				}
			}
			break;
		case NAND_OP_DATA_IN_INSTR:
			nfc_op->data_instr = instr;
			nfc_op->type = NAND_OP_DATA_IN_INSTR;
			nfc_op->data_instr_idx = op_id;
			break;
		case NAND_OP_DATA_OUT_INSTR:
			nfc_op->data_instr = instr;
			nfc_op->type = NAND_OP_DATA_IN_INSTR;
			nfc_op->data_instr_idx = op_id;
			break;
		case NAND_OP_WAITRDY_INSTR:
			nfc_op->rdy_timeout_ms = instr->ctx.waitrdy.timeout_ms;
			nfc_op->rdy_delay_ns = instr->delay_ns;
			break;
		}
	}
}

/**
 * anfc_data_cpy - Read data from the NAND
 * @nfc: The Controller instance
 * @mtd: mtd info structure
 * @buf: buffer used to store the data
 * @len: length of the buffer
 * @operation: current command transfer,
 *             that needs to be set in program_register
 * @direction: Read/write transfer
 * The Arasan NAND controller always read/write 4byte alinged
 * lengths.If the length of the data is not 4byte aligned, then
 * we need to make it as alinged.
 *
 * Returns 0 on success, a negative error code otherwise.
 */
static void anfc_data_cpy(struct anfc_nand_controller *nfc,
			  struct mtd_info *mtd, u8 *buf,
			  int len, int operation, bool direction)
{
	u32 Rem = 0, Div;

	if (!buf)
		return;

	Rem = len % 4;
	Div = len / 4;
	if (len < 4) {
		anfc_rw_pio_op(mtd, buf, 4, direction, operation);
	} else {
		anfc_rw_pio_op(mtd, buf, 4*Div, direction, operation);

		if (Rem) {
			buf += (4*Div);
			anfc_rw_pio_op(mtd, buf, 4, direction, operation);
		}
	}
}

static int anfc_status_type_exec(struct nand_chip *chip,
				   const struct nand_subop *subop)
{
	const struct nand_op_instr *instr;
	struct anfc_op nfc_op = {};
	unsigned int op_id, len;
	struct anfc_nand_chip *achip = to_anfc_nand(chip);
	struct anfc_nand_controller *nfc = to_anfc(chip->controller);

	anfc_parse_instructions(chip, subop, &nfc_op);
	instr = nfc_op.data_instr;
	op_id = nfc_op.data_instr_idx;

	anfc_prepare_cmd(nfc, nfc_op.cmnds[0], 0, 0, 0, 0);
	anfc_setpktszcnt(nfc, achip->spktsize / 4, 1);
	anfc_setpagecoladdr(nfc, nfc_op.row, nfc_op.col);
	nfc->prog = PROG_STATUS;

	anfc_enable_intrs(nfc, XFER_COMPLETE);
	writel(nfc->prog, nfc->base + PROG_OFST);
	anfc_wait_for_event(nfc);

	if (!nfc_op.data_instr)
		return 0;

	len = nand_subop_get_data_len(subop, op_id);

	/*
	 * The Arasan NAND controller will update the status value
	 * returned by the flash device in FLASH_STS register.
	 */
	nfc->status = readl(nfc->base + FLASH_STS_OFST);
	memcpy(instr->ctx.data.buf.in, &nfc->status, len);
	return 0;
}

static int anfc_erase_function(struct nand_chip *chip,
				   struct anfc_op nfc_op)
{

	struct anfc_nand_chip *achip = to_anfc_nand(chip);
	struct anfc_nand_controller *nfc = to_anfc(chip->controller);

	nfc->prog = PROG_ERASE;
	anfc_prepare_cmd(nfc, nfc_op.cmnds[0], NAND_CMD_ERASE2, 0, 0,
			 achip->raddr_cycles);
	nfc_op.col = nfc_op.row & 0xffff;
	nfc_op.row = (nfc_op.row >> PG_ADDR_SHIFT) & 0xffff;
	anfc_setpagecoladdr(nfc, nfc_op.row, nfc_op.col);

	anfc_enable_intrs(nfc, XFER_COMPLETE);
	writel(nfc->prog, nfc->base + PROG_OFST);
	anfc_wait_for_event(nfc);

	return 0;
}


static int anfc_exec_op_cmd(struct nand_chip *chip,
				   const struct nand_subop *subop)
{
	const struct nand_op_instr *instr;
	struct anfc_op nfc_op = {};
	struct anfc_nand_chip *achip = to_anfc_nand(chip);
	struct anfc_nand_controller *nfc = to_anfc(chip->controller);
	struct mtd_info *mtd = nand_to_mtd(chip);
	u32 addrcycles;
	unsigned int op_id, len = 0;
	bool reading;

	anfc_parse_instructions(chip, subop, &nfc_op);
	instr = nfc_op.data_instr;
	op_id = nfc_op.data_instr_idx;
	if (nfc_op.data_instr)
		len = nand_subop_get_data_len(subop, op_id);

	/*
	 * The switch case is to prepare a command and to set page/column
	 * address. Arasan NAND controller has program register(Off: 0x10)),
	 * which needs to be set for every command.
	 * Ex: When NAND_CMD_RESET is issued, then we need to set reset bit
	 * in program_register. etc..
	 */
	switch (nfc_op.cmnds[0]) {
	case NAND_CMD_SEQIN:
		addrcycles = achip->raddr_cycles + achip->caddr_cycles;

		anfc_prepare_cmd(nfc, nfc_op.cmnds[0], NAND_CMD_PAGEPROG, 1,
				 mtd->writesize, addrcycles);
		anfc_setpagecoladdr(nfc, nfc_op.row, nfc_op.col);
		break;
	case NAND_CMD_READOOB:
		nfc_op.col += mtd->writesize;
	case NAND_CMD_READ0:
	case NAND_CMD_READ1:
		addrcycles = achip->raddr_cycles + achip->caddr_cycles;
		anfc_prepare_cmd(nfc, NAND_CMD_READ0, NAND_CMD_READSTART, 1,
				 mtd->writesize, addrcycles);
		anfc_setpagecoladdr(nfc, nfc_op.row, nfc_op.col);
		if (!nfc_op.data_instr)
			return 0;

		anfc_read_data_op(mtd, instr->ctx.data.buf.in, len);
		break;
	case NAND_CMD_RNDOUT:
		anfc_prepare_cmd(nfc, nfc_op.cmnds[0], NAND_CMD_RNDOUTSTART, 1,
				 mtd->writesize, 2);
		anfc_setpagecoladdr(nfc, nfc_op.row, nfc_op.col);
		nfc->prog = PROG_PGRD;
		break;
	case NAND_CMD_PARAM:
		anfc_prepare_cmd(nfc, nfc_op.cmnds[0], 0, 0, 0, 1);
		anfc_setpagecoladdr(nfc, nfc_op.row, nfc_op.col);
		nfc->prog = PROG_RDPARAM;
		break;
	case NAND_CMD_READID:
		anfc_prepare_cmd(nfc, nfc_op.cmnds[0], 0, 0, 0, 1);
		anfc_setpagecoladdr(nfc, nfc_op.row, nfc_op.col);
		nfc->prog = PROG_RDID;
		break;
	case NAND_CMD_GET_FEATURES:
		anfc_prepare_cmd(nfc, nfc_op.cmnds[0], 0, 0, 0, 1);
		anfc_setpagecoladdr(nfc, nfc_op.row, nfc_op.col);
		nfc->prog = PROG_GET_FEATURE;
		break;
	case NAND_CMD_SET_FEATURES:
		anfc_prepare_cmd(nfc, nfc_op.cmnds[0], 0, 0, 0, 1);
		anfc_setpagecoladdr(nfc, nfc_op.row, nfc_op.col);
		nfc->prog = PROG_SET_FEATURE;
		break;
	case NAND_CMD_ERASE1:
		anfc_erase_function(chip, nfc_op);
		break;
	default:
		break;
	}

	if (!nfc_op.data_instr)
		return 0;

	reading = (nfc_op.data_instr->type == NAND_OP_DATA_IN_INSTR);
	if (reading) {
		if (nfc->curr_cmd == NAND_CMD_STATUS) {
			nfc->status = readl(nfc->base + FLASH_STS_OFST);
			memcpy(instr->ctx.data.buf.in, &nfc->status, len);
		} else {
			anfc_data_cpy(nfc, mtd, instr->ctx.data.buf.in, len,
				      nfc->prog, 1);
		}
	} else {
		anfc_data_cpy(nfc, mtd, (char *)instr->ctx.data.buf.out, len,
			      nfc->prog, 0);
	}

	return 0;
}

static int anfc_reset_type_exec(struct nand_chip *chip,
				   const struct nand_subop *subop)
{
	struct anfc_op nfc_op = {};
	struct anfc_nand_controller *nfc = to_anfc(chip->controller);

	anfc_parse_instructions(chip, subop, &nfc_op);
	anfc_prepare_cmd(nfc, nfc_op.cmnds[0], 0, 0, 0, 0);
	nfc->prog = PROG_RST;
	anfc_enable_intrs(nfc, XFER_COMPLETE);
	writel(nfc->prog, nfc->base + PROG_OFST);
	anfc_wait_for_event(nfc);

	return 0;
}

static const struct nand_op_parser anfc_op_parser = NAND_OP_PARSER
	(NAND_OP_PARSER_PATTERN
		(anfc_exec_op_cmd,
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_ADDR_ELEM(false, 7),
		NAND_OP_PARSER_PAT_WAITRDY_ELEM(false),
		NAND_OP_PARSER_PAT_DATA_IN_ELEM(true, 2048)),
	NAND_OP_PARSER_PATTERN
		(anfc_exec_op_cmd,
		NAND_OP_PARSER_PAT_DATA_IN_ELEM(true, 2048)),
	NAND_OP_PARSER_PATTERN
		(anfc_exec_op_cmd,
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_ADDR_ELEM(false, 7),
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_WAITRDY_ELEM(false),
		NAND_OP_PARSER_PAT_DATA_IN_ELEM(true, 2048)),
	NAND_OP_PARSER_PATTERN
		(anfc_exec_op_cmd,
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_ADDR_ELEM(false, 8),
		NAND_OP_PARSER_PAT_DATA_OUT_ELEM(false, 2048),
		//NAND_OP_PARSER_PAT_CMD_ELEM(true),
		NAND_OP_PARSER_PAT_WAITRDY_ELEM(true)),
	NAND_OP_PARSER_PATTERN
		(anfc_exec_op_cmd,
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_ADDR_ELEM(false, 8),
		NAND_OP_PARSER_PAT_CMD_ELEM(true),
		NAND_OP_PARSER_PAT_DATA_IN_ELEM(true, 2048)),
	NAND_OP_PARSER_PATTERN
		(anfc_reset_type_exec,
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_WAITRDY_ELEM(false)),
	NAND_OP_PARSER_PATTERN
		(anfc_status_type_exec,
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_DATA_IN_ELEM(true, 1)),
	);

static int anfc_exec_op(struct nand_chip *chip,
			     const struct nand_operation *op,
			     bool check_only)
{
	return nand_op_parser_exec_op(chip, &anfc_op_parser,
					      op, check_only);
}

static void anfc_select_chip(struct mtd_info *mtd, int num)
{

	u32 val;
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct anfc_nand_chip *achip = to_anfc_nand(chip);
	struct anfc_nand_controller *nfc = to_anfc(chip->controller);

	if (num == -1)
		return;

	val = readl(nfc->base + MEM_ADDR2_OFST);
	val &= (val & ~(CS_MASK | BCH_MODE_MASK));
	val |= (achip->csnum << CS_SHIFT) | (achip->bchmode << BCH_MODE_SHIFT);
	writel(val, nfc->base + MEM_ADDR2_OFST);
	nfc->csnum = achip->csnum;
	writel(achip->eccval, nfc->base + ECC_OFST);
	writel(achip->inftimeval, nfc->base + DATA_INTERFACE_OFST);
}

static irqreturn_t anfc_irq_handler(int irq, void *ptr)
{
	struct anfc_nand_controller *nfc = ptr;
	u32 status;

	status = readl(nfc->base + INTR_STS_OFST);
	if (status & EVENT_MASK) {
		complete(&nfc->event);
		writel((status & EVENT_MASK), nfc->base + INTR_STS_OFST);
		writel(0, nfc->base + INTR_STS_EN_OFST);
		writel(0, nfc->base + INTR_SIG_EN_OFST);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static int anfc_setup_data_interface(struct mtd_info *mtd, int csline,
				     const struct nand_data_interface *conf)
{

	struct nand_chip *chip = mtd_to_nand(mtd);
	struct anfc_nand_controller *nfc = to_anfc(chip->controller);
	struct anfc_nand_chip *achip = to_anfc_nand(chip);

	int mode, err;
	void __iomem *nand_clk;
	const struct nand_sdr_timings *sdr;
	u32 inftimeval;
	bool change_sdr_clk = false;

	if (csline == NAND_DATA_IFACE_CHECK_ONLY)
		return 0;

	/*
	 * If the controller is already in the same mode as flash device
	 * then no need to change the timing mode again.
	 */
	sdr = nand_get_sdr_timings(conf);
	if (IS_ERR(sdr))
		return PTR_ERR(sdr);

	mode = anfc_get_mode_frm_timings(sdr);

	if (mode < 0)
		return -ENOTSUPP;

	inftimeval = mode & 7;
	if (mode >= 2 && mode <= 5)
		change_sdr_clk = true;
	/*
	 * SDR timing modes 2-5 will not work for the arasan nand when
	 * freq > 90 MHz, so reduce the freq in SDR modes 2-5 to < 90Mhz
	 */
	if (change_sdr_clk) {
		clk_disable_unprepare(nfc->clk_sys);
		//err = clk_set_rate(nfc->clk_sys, SDR_MODE_DEFLT_FREQ);
		nand_clk = ioremap(0xFF5E00B4, 50);
		writel(0x01011200, nand_clk);
		err = clk_prepare_enable(nfc->clk_sys);
		if (err) {
			dev_err(nfc->dev, "Unable to enable sys clock.\n");
			clk_disable_unprepare(nfc->clk_sys);
			return err;
		}
	}
	achip->inftimeval = inftimeval;
	if (mode & ONFI_DATA_INTERFACE_NVDDR)
		achip->spktsize = NVDDR_MODE_PACKET_SIZE;

	return 0;
}

static int anfc_nand_attach_chip(struct nand_chip *chip)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct anfc_nand_chip *achip = to_anfc_nand(chip);
	u32 ret;

	if (mtd->writesize <= SZ_512)
		achip->caddr_cycles = 1;
	else
		achip->caddr_cycles = 2;

	if (chip->options & NAND_ROW_ADDR_3)
		achip->raddr_cycles = 3;
	else
		achip->raddr_cycles = 2;

	chip->ecc.calc_buf = kmalloc(mtd->oobsize, GFP_KERNEL);
	chip->ecc.code_buf = kmalloc(mtd->oobsize, GFP_KERNEL);
	ret = anfc_ecc_init(mtd, &chip->ecc, chip->ecc.mode);
	if (ret)
		return ret;

	return 0;
}

static const struct nand_controller_ops anfc_nand_controller_ops = {
	.attach_chip = anfc_nand_attach_chip,
};

static int anfc_nand_chip_init(struct anfc_nand_controller *nfc,
			       struct anfc_nand_chip *anand_chip,
			       struct device_node *np)
{
	struct nand_chip *chip = &anand_chip->chip;
	struct mtd_info *mtd = nand_to_mtd(chip);
	int ret;

	ret = of_property_read_u32(np, "reg", &anand_chip->csnum);
	if (ret) {
		dev_err(nfc->dev, "can't get chip-select\n");
		return -ENXIO;
	}
	mtd->name = devm_kasprintf(nfc->dev, GFP_KERNEL, "arasan_nand.%d",
				   anand_chip->csnum);
	mtd->dev.parent = nfc->dev;

	chip->chip_delay = 30;
	chip->controller = &nfc->controller;
	chip->options = NAND_BUSWIDTH_AUTO | NAND_NO_SUBPAGE_WRITE;
	chip->bbt_options = NAND_BBT_USE_FLASH;
	chip->select_chip = anfc_select_chip;
	chip->setup_data_interface = anfc_setup_data_interface;
	chip->exec_op = anfc_exec_op;
	nand_set_flash_node(chip, np);

	anand_chip->spktsize = SDR_MODE_PACKET_SIZE;

	ret = nand_scan(mtd, 1);
	if (ret) {
		dev_err(nfc->dev, "nand_scan_tail for NAND failed\n");
		return ret;
	}

	return mtd_device_register(mtd, NULL, 0);
}

static int anfc_probe(struct platform_device *pdev)
{
	struct anfc_nand_controller *nfc;
	struct anfc_nand_chip *anand_chip;
	struct device_node *np = pdev->dev.of_node, *child;
	struct resource *res;
	int err;

	nfc = devm_kzalloc(&pdev->dev, sizeof(*nfc), GFP_KERNEL);
	if (!nfc)
		return -ENOMEM;

	init_waitqueue_head(&nfc->controller.wq);
	INIT_LIST_HEAD(&nfc->chips);
	init_completion(&nfc->event);
	nfc->dev = &pdev->dev;
	platform_set_drvdata(pdev, nfc);
	nfc->csnum = -1;
	nfc->controller.ops = &anfc_nand_controller_ops;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	nfc->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(nfc->base))
		return PTR_ERR(nfc->base);
	nfc->dma = of_property_read_bool(pdev->dev.of_node,
					 "arasan,has-mdma");
	nfc->irq = platform_get_irq(pdev, 0);
	if (nfc->irq < 0) {
		dev_err(&pdev->dev, "platform_get_irq failed\n");
		return -ENXIO;
	}
	dma_set_mask(&pdev->dev, DMA_BIT_MASK(64));
	err = devm_request_irq(&pdev->dev, nfc->irq, anfc_irq_handler,
			       0, "arasannfc", nfc);
	if (err)
		return err;
	nfc->clk_sys = devm_clk_get(&pdev->dev, "sys");
	if (IS_ERR(nfc->clk_sys)) {
		dev_err(&pdev->dev, "sys clock not found.\n");
		return PTR_ERR(nfc->clk_sys);
	}

	nfc->clk_flash = devm_clk_get(&pdev->dev, "flash");
	if (IS_ERR(nfc->clk_flash)) {
		dev_err(&pdev->dev, "flash clock not found.\n");
		return PTR_ERR(nfc->clk_flash);
	}

	err = clk_prepare_enable(nfc->clk_sys);
	if (err) {
		dev_err(&pdev->dev, "Unable to enable sys clock.\n");
		return err;
	}

	err = clk_prepare_enable(nfc->clk_flash);
	if (err) {
		dev_err(&pdev->dev, "Unable to enable flash clock.\n");
		goto clk_dis_sys;
	}

	for_each_available_child_of_node(np, child) {
		anand_chip = devm_kzalloc(&pdev->dev, sizeof(*anand_chip),
					  GFP_KERNEL);
		if (!anand_chip) {
			of_node_put(child);
			err = -ENOMEM;
			goto nandchip_clean_up;
		}
		err = anfc_nand_chip_init(nfc, anand_chip, child);
		if (err) {
			devm_kfree(&pdev->dev, anand_chip);
			continue;
		}

		list_add_tail(&anand_chip->node, &nfc->chips);
	}
	return 0;

nandchip_clean_up:
	list_for_each_entry(anand_chip, &nfc->chips, node)
		nand_release(nand_to_mtd(&anand_chip->chip));
	clk_disable_unprepare(nfc->clk_flash);
clk_dis_sys:
	clk_disable_unprepare(nfc->clk_sys);

	return err;
}

static int anfc_remove(struct platform_device *pdev)
{
	struct anfc_nand_controller *nfc = platform_get_drvdata(pdev);
	struct anfc_nand_chip *anand_chip;

	list_for_each_entry(anand_chip, &nfc->chips, node)
		nand_release(nand_to_mtd(&anand_chip->chip));

	clk_disable_unprepare(nfc->clk_sys);
	clk_disable_unprepare(nfc->clk_flash);

	return 0;
}

static const struct of_device_id anfc_ids[] = {
	{ .compatible = "arasan,nfc-v3p10" },
	{ .compatible = "xlnx,zynqmp-nand" },
	{  }
};
MODULE_DEVICE_TABLE(of, anfc_ids);

static struct platform_driver anfc_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = anfc_ids,
	},
	.probe = anfc_probe,
	.remove = anfc_remove,
};
module_platform_driver(anfc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Xilinx, Inc");
MODULE_DESCRIPTION("Arasan NAND Flash Controller Driver");

