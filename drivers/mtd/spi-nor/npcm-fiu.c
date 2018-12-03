// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018 Nuvoton Technology corporation.

#include <linux/slab.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/of_irq.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/spi-nor.h>
#include <linux/vmalloc.h>
#include <linux/regmap.h>
#include <linux/log2.h>
#include <linux/mod_devicetable.h>
#include <linux/of_device.h>

#include <asm/sizes.h>
#include <mtd/mtd-abi.h>

/* Flash Interface Unit (FIU) Registers */
#define NPCM_FIU_DRD_CFG		0x00
#define NPCM_FIU_DWR_CFG		0x04
#define NPCM_FIU_UMA_CFG		0x08
#define NPCM_FIU_UMA_CTS		0x0C
#define NPCM_FIU_UMA_CMD		0x10
#define NPCM_FIU_UMA_ADDR		0x14
#define NPCM_FIU_PRT_CFG		0x18
#define NPCM_FIU_UMA_DW0		0x20
#define NPCM_FIU_UMA_DW1		0x24
#define NPCM_FIU_UMA_DW2		0x28
#define NPCM_FIU_UMA_DW3		0x2C
#define NPCM_FIU_UMA_DR0		0x30
#define NPCM_FIU_UMA_DR1		0x34
#define NPCM_FIU_UMA_DR2		0x38
#define NPCM_FIU_UMA_DR3		0x3C
#define NPCM_FIU_MAX_REG_LIMIT		0x80

/* FIU Direct Read Configuration Register */
#define NPCM_FIU_DRD_CFG_LCK		BIT(31)
#define NPCM_FIU_DRD_CFG_R_BURST	GENMASK(25, 24)
#define NPCM_FIU_DRD_CFG_ADDSIZ		GENMASK(17, 16)
#define NPCM_FIU_DRD_CFG_DBW		GENMASK(13, 12)
#define NPCM_FIU_DRD_CFG_ACCTYPE	GENMASK(9, 8)
#define NPCM_FIU_DRD_CFG_RDCMD		GENMASK(7, 0)
#define NPCM_FIU_DRD_ADDSIZ_SHIFT	16
#define NPCM_FIU_DRD_DBW_SHIFT		12
#define NPCM_FIU_DRD_ACCTYPE_SHIFT	8

/* FIU Direct Write Configuration Register */
#define NPCM_FIU_DWR_CFG_LCK		BIT(31)
#define NPCM_FIU_DWR_CFG_W_BURST	GENMASK(25, 24)
#define NPCM_FIU_DWR_CFG_ADDSIZ		GENMASK(17, 16)
#define NPCM_FIU_DWR_CFG_ABPCK		GENMASK(11, 10)
#define NPCM_FIU_DWR_CFG_DBPCK		GENMASK(9, 8)
#define NPCM_FIU_DWR_CFG_WRCMD		GENMASK(7, 0)
#define NPCM_FIU_DWR_ADDSIZ_SHIFT	16
#define NPCM_FIU_DWR_ABPCK_SHIFT	10
#define NPCM_FIU_DWR_DBPCK_SHIFT	8

/* FIU UMA Configuration Register */
#define NPCM_FIU_UMA_CFG_LCK		BIT(31)
#define NPCM_FIU_UMA_CFG_CMMLCK		BIT(30)
#define NPCM_FIU_UMA_CFG_RDATSIZ	GENMASK(28, 24)
#define NPCM_FIU_UMA_CFG_DBSIZ		GENMASK(23, 21)
#define NPCM_FIU_UMA_CFG_WDATSIZ	GENMASK(20, 16)
#define NPCM_FIU_UMA_CFG_ADDSIZ		GENMASK(13, 11)
#define NPCM_FIU_UMA_CFG_CMDSIZ		BIT(10)
#define NPCM_FIU_UMA_CFG_RDBPCK		GENMASK(9, 8)
#define NPCM_FIU_UMA_CFG_DBPCK		GENMASK(7, 6)
#define NPCM_FIU_UMA_CFG_WDBPCK		GENMASK(5, 4)
#define NPCM_FIU_UMA_CFG_ADBPCK		GENMASK(3, 2)
#define NPCM_FIU_UMA_CFG_CMBPCK		GENMASK(1, 0)
#define NPCM_FIU_UMA_CFG_ADBPCK_SHIFT	2
#define NPCM_FIU_UMA_CFG_WDBPCK_SHIFT	4
#define NPCM_FIU_UMA_CFG_DBPCK_SHIFT	6
#define NPCM_FIU_UMA_CFG_RDBPCK_SHIFT	8
#define NPCM_FIU_UMA_CFG_ADDSIZ_SHIFT	11
#define NPCM_FIU_UMA_CFG_WDATSIZ_SHIFT	16
#define NPCM_FIU_UMA_CFG_DBSIZ_SHIFT	21
#define NPCM_FIU_UMA_CFG_RDATSIZ_SHIFT	24

/* FIU UMA Control and Status Register */
#define NPCM_FIU_UMA_CTS_RDYIE		BIT(25)
#define NPCM_FIU_UMA_CTS_RDYST		BIT(24)
#define NPCM_FIU_UMA_CTS_SW_CS		BIT(16)
#define NPCM_FIU_UMA_CTS_DEV_NUM	GENMASK(9, 8)
#define NPCM_FIU_UMA_CTS_EXEC_DONE	BIT(0)
#define NPCM_FIU_UMA_CTS_DEV_NUM_SHIFT	8

/* FIU UMA Command Register */
#define NPCM_FIU_UMA_CMD_DUM3		GENMASK(31, 24)
#define NPCM_FIU_UMA_CMD_DUM2		GENMASK(23, 16)
#define NPCM_FIU_UMA_CMD_DUM1		GENMASK(15, 8)
#define NPCM_FIU_UMA_CMD_CMD		GENMASK(7, 0)

/* FIU UMA Address Register */
#define NPCM_FIU_UMA_ADDR_UMA_ADDR	GENMASK(31, 0)
#define NPCM_FIU_UMA_ADDR_AB3		GENMASK(31, 24)
#define NPCM_FIU_UMA_ADDR_AB2		GENMASK(23, 16)
#define NPCM_FIU_UMA_ADDR_AB1		GENMASK(15, 8)
#define NPCM_FIU_UMA_ADDR_AB0		GENMASK(7, 0)

/* FIU UMA Write Data Bytes 0-3 Register */
#define NPCM_FIU_UMA_DW0_WB3		GENMASK(31, 24)
#define NPCM_FIU_UMA_DW0_WB2		GENMASK(23, 16)
#define NPCM_FIU_UMA_DW0_WB1		GENMASK(15, 8)
#define NPCM_FIU_UMA_DW0_WB0		GENMASK(7, 0)

/* FIU UMA Write Data Bytes 4-7 Register */
#define NPCM_FIU_UMA_DW1_WB7		GENMASK(31, 24)
#define NPCM_FIU_UMA_DW1_WB6		GENMASK(23, 16)
#define NPCM_FIU_UMA_DW1_WB5		GENMASK(15, 8)
#define NPCM_FIU_UMA_DW1_WB4		GENMASK(7, 0)

/* FIU UMA Write Data Bytes 8-11 Register */
#define NPCM_FIU_UMA_DW2_WB11		GENMASK(31, 24)
#define NPCM_FIU_UMA_DW2_WB10		GENMASK(23, 16)
#define NPCM_FIU_UMA_DW2_WB9		GENMASK(15, 8)
#define NPCM_FIU_UMA_DW2_WB8		GENMASK(7, 0)

/* FIU UMA Write Data Bytes 12-15 Register */
#define NPCM_FIU_UMA_DW3_WB15		GENMASK(31, 24)
#define NPCM_FIU_UMA_DW3_WB14		GENMASK(23, 16)
#define NPCM_FIU_UMA_DW3_WB13		GENMASK(15, 8)
#define NPCM_FIU_UMA_DW3_WB12		GENMASK(7, 0)

/* FIU UMA Read Data Bytes 0-3 Register */
#define NPCM_FIU_UMA_DR0_RB3		GENMASK(31, 24)
#define NPCM_FIU_UMA_DR0_RB2		GENMASK(23, 16)
#define NPCM_FIU_UMA_DR0_RB1		GENMASK(15, 8)
#define NPCM_FIU_UMA_DR0_RB0		GENMASK(7, 0)

/* FIU UMA Read Data Bytes 4-7 Register */
#define NPCM_FIU_UMA_DR1_RB15		GENMASK(31, 24)
#define NPCM_FIU_UMA_DR1_RB14		GENMASK(23, 16)
#define NPCM_FIU_UMA_DR1_RB13		GENMASK(15, 8)
#define NPCM_FIU_UMA_DR1_RB12		GENMASK(7, 0)

/* FIU UMA Read Data Bytes 8-11 Register */
#define NPCM_FIU_UMA_DR2_RB15		GENMASK(31, 24)
#define NPCM_FIU_UMA_DR2_RB14		GENMASK(23, 16)
#define NPCM_FIU_UMA_DR2_RB13		GENMASK(15, 8)
#define NPCM_FIU_UMA_DR2_RB12		GENMASK(7, 0)

/* FIU UMA Read Data Bytes 12-15 Register */
#define NPCM_FIU_UMA_DR3_RB15		GENMASK(31, 24)
#define NPCM_FIU_UMA_DR3_RB14		GENMASK(23, 16)
#define NPCM_FIU_UMA_DR3_RB13		GENMASK(15, 8)
#define NPCM_FIU_UMA_DR3_RB12		GENMASK(7, 0)

/* FIU Read Mode */
enum {
	DRD_SINGLE_WIRE_MODE	= 0,
	DRD_DUAL_IO_MODE	= 1,
	DRD_QUAD_IO_MODE	= 2,
	DRD_SPI_X_MODE		= 3,
};

enum {
	DWR_ABPCK_BIT_PER_CLK	= 0,
	DWR_ABPCK_2_BIT_PER_CLK	= 1,
	DWR_ABPCK_4_BIT_PER_CLK	= 2,
};

enum {
	DWR_DBPCK_BIT_PER_CLK	= 0,
	DWR_DBPCK_2_BIT_PER_CLK	= 1,
	DWR_DBPCK_4_BIT_PER_CLK	= 2,
};

#define NPCM_FIU_DRD_16_BYTE_BURST	0x3000000
#define NPCM_FIU_DWR_16_BYTE_BURST	0x3000000

#define MAP_SIZE_128MB			0x8000000
#define MAP_SIZE_16MB			0x1000000
#define MAP_SIZE_8MB			0x800000

#define NUM_BITS_IN_BYTE		8
#define FIU_DRD_MAX_DUMMY_NUMBER	3
#define NPCM_MAX_CHIP_NUM		4
#define CHUNK_SIZE			16
#define UMA_MICRO_SEC_TIMEOUT		150

enum {
	FIU0 = 0,
	FIU3,
	FIUX,
};

struct npcm_fiu_info {
	char *name;
	u32 fiu_id;
	u32 max_map_size;
	u32 max_cs;
};

struct fiu_data {
	const struct npcm_fiu_info *npcm_fiu_data_info;
	int fiu_max;
};

static const struct npcm_fiu_info npxm7xx_fiu_info[] = {
	{.name = "FIU0", .fiu_id = FIU0,
		.max_map_size = MAP_SIZE_128MB, .max_cs = 2},
	{.name = "FIU3", .fiu_id = FIU3,
		.max_map_size = MAP_SIZE_128MB, .max_cs = 4},
	{.name = "FIUX", .fiu_id = FIUX,
		.max_map_size = MAP_SIZE_16MB, .max_cs = 2} };

static const struct fiu_data npxm7xx_fiu_data = {
	.npcm_fiu_data_info = npxm7xx_fiu_info,
	.fiu_max = 3,
};

struct npcm_fiu_bus;

struct npcm_chip {
	void __iomem *flash_region_mapped_ptr;
	enum spi_nor_protocol direct_rd_proto;
	struct npcm_fiu_bus *host;
	struct spi_nor nor;
	bool direct_read;
	u32 read_proto;
	u32 chipselect;
	u32 clkrate;
};

struct npcm_fiu_bus {
	struct npcm_chip *chip[NPCM_MAX_CHIP_NUM];
	enum spi_nor_protocol direct_rd_proto;
	const struct npcm_fiu_info *info;
	struct resource *res_mem;
	resource_size_t iosize;
	struct regmap *regmap;
	void __iomem *regbase;
	u32 direct_read_proto;
	struct device *dev;
	struct mutex lock;	/* controller access mutex */
	struct clk *clk;
	bool spix_mode;
	int id;
};

static const struct regmap_config npcm_mtd_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = NPCM_FIU_MAX_REG_LIMIT,
};

static int npcm_fiu_direct_read(struct mtd_info *mtd, loff_t from, size_t len,
				size_t *retlen, u_char *buf)
{
	struct spi_nor *nor = mtd->priv;
	struct npcm_chip *chip = nor->priv;

	memcpy_fromio(buf, chip->flash_region_mapped_ptr + from, len);

	*retlen = len;
	return 0;
}

static int npcm_fiu_direct_write(struct mtd_info *mtd, loff_t to, size_t len,
				 size_t *retlen, const u_char *buf)
{
	struct spi_nor *nor = mtd->priv;
	struct npcm_chip *chip = nor->priv;

	memcpy_toio(chip->flash_region_mapped_ptr + to, buf, len);

	*retlen = len;
	return 0;
}

static int npcm_fiu_uma_read(struct spi_nor *nor, u8 transaction_code,
			     u32 address, bool is_address_size, u8 *data,
			     u32 data_size)
{
	struct npcm_chip *chip = nor->priv;
	struct npcm_fiu_bus *host = chip->host;
	u32 uma_cfg = BIT(10);
	u32 dummy_bytes;
	u32 data_reg[4];
	int ret;
	u32 val;
	u32 i;

	regmap_update_bits(host->regmap, NPCM_FIU_UMA_CTS,
			   NPCM_FIU_UMA_CTS_DEV_NUM,
			   (chip->chipselect <<
			    NPCM_FIU_UMA_CTS_DEV_NUM_SHIFT));
	regmap_update_bits(host->regmap, NPCM_FIU_UMA_CMD,
			   NPCM_FIU_UMA_CMD_CMD, transaction_code);
	regmap_write(host->regmap, NPCM_FIU_UMA_ADDR, address);

	if (is_address_size) {
		uma_cfg |=
			ilog2(spi_nor_get_protocol_inst_nbits(nor->read_proto));
		uma_cfg |=
			ilog2(spi_nor_get_protocol_addr_nbits(nor->read_proto))
		<< NPCM_FIU_UMA_CFG_ADBPCK_SHIFT;
		uma_cfg |=
			ilog2(spi_nor_get_protocol_addr_nbits(nor->read_proto))
		<< NPCM_FIU_UMA_CFG_DBPCK_SHIFT;
		uma_cfg |=
			ilog2(spi_nor_get_protocol_data_nbits(nor->read_proto))
		<< NPCM_FIU_UMA_CFG_RDBPCK_SHIFT;
		dummy_bytes =
			(nor->read_dummy *
			 spi_nor_get_protocol_addr_nbits(nor->read_proto)) /
			NUM_BITS_IN_BYTE;
		uma_cfg |= dummy_bytes << NPCM_FIU_UMA_CFG_DBSIZ_SHIFT;
		uma_cfg |= (nor->addr_width << NPCM_FIU_UMA_CFG_ADDSIZ_SHIFT);
	}

	uma_cfg |= data_size << NPCM_FIU_UMA_CFG_RDATSIZ_SHIFT;
	regmap_write(host->regmap, NPCM_FIU_UMA_CFG, uma_cfg);

	regmap_write_bits(host->regmap, NPCM_FIU_UMA_CTS,
			  NPCM_FIU_UMA_CTS_EXEC_DONE,
			  NPCM_FIU_UMA_CTS_EXEC_DONE);

	ret = regmap_read_poll_timeout(host->regmap, NPCM_FIU_UMA_CTS, val,
				       (!(val & NPCM_FIU_UMA_CTS_EXEC_DONE)), 0,
				       UMA_MICRO_SEC_TIMEOUT);
	if (ret)
		return ret;

	if (data_size) {
		for (i = 0; i <= data_size / 4; i++)
			regmap_read(host->regmap, NPCM_FIU_UMA_DR0 + (i * 4),
				    &data_reg[i]);
		memcpy(data, data_reg, data_size);
	}

	return 0;
}

static int npcm_fiu_uma_write(struct spi_nor *nor, u8 transaction_code,
			      u32 address, bool is_address_size, u8 *data,
			      u32 data_size)
{
	struct npcm_chip *chip = nor->priv;
	struct npcm_fiu_bus *host = chip->host;
	u32 uma_cfg = BIT(10);
	u32 data_reg[4] = {0};
	u32 val;
	u32 i;

	regmap_update_bits(host->regmap, NPCM_FIU_UMA_CTS,
			   NPCM_FIU_UMA_CTS_DEV_NUM,
			   (chip->chipselect <<
			    NPCM_FIU_UMA_CTS_DEV_NUM_SHIFT));

	regmap_update_bits(host->regmap, NPCM_FIU_UMA_CMD,
			   NPCM_FIU_UMA_CMD_CMD, transaction_code);
	regmap_write(host->regmap, NPCM_FIU_UMA_ADDR, address);

	if (data_size) {
		memcpy(data_reg, data, data_size);
		for (i = 0; i <= data_size / 4; i++)
			regmap_write(host->regmap, NPCM_FIU_UMA_DW0 + (i * 4),
				     data_reg[i]);
	}

	if (is_address_size) {
		uma_cfg |=
			ilog2(spi_nor_get_protocol_inst_nbits
			      (nor->write_proto));
		uma_cfg |=
			ilog2(spi_nor_get_protocol_addr_nbits(nor->write_proto))
		<< NPCM_FIU_UMA_CFG_ADBPCK_SHIFT;
		uma_cfg |=
			ilog2(spi_nor_get_protocol_data_nbits(nor->write_proto))
		<< NPCM_FIU_UMA_CFG_WDBPCK_SHIFT;
		uma_cfg |= (nor->addr_width << NPCM_FIU_UMA_CFG_ADDSIZ_SHIFT);
	}

	uma_cfg |= (data_size << NPCM_FIU_UMA_CFG_WDATSIZ_SHIFT);
	regmap_write(host->regmap, NPCM_FIU_UMA_CFG, uma_cfg);

	regmap_write_bits(host->regmap, NPCM_FIU_UMA_CTS,
			  NPCM_FIU_UMA_CTS_EXEC_DONE,
			  NPCM_FIU_UMA_CTS_EXEC_DONE);

	return regmap_read_poll_timeout(host->regmap, NPCM_FIU_UMA_CTS, val,
				       (!(val & NPCM_FIU_UMA_CTS_EXEC_DONE)), 0,
					UMA_MICRO_SEC_TIMEOUT);
}

static int npcm_fiu_manualwrite(struct spi_nor *nor, u8 transaction_code,
				u32 address, u8 *data, u32 data_size)
{
	u32 num_data_chunks;
	u32 remain_data;
	u32 idx = 0;
	int ret;

	struct npcm_chip *chip = nor->priv;
	struct npcm_fiu_bus *host = chip->host;

	num_data_chunks  = data_size / CHUNK_SIZE;
	remain_data  = data_size % CHUNK_SIZE;

	regmap_update_bits(host->regmap, NPCM_FIU_UMA_CTS,
			   NPCM_FIU_UMA_CTS_DEV_NUM,
			   (chip->chipselect <<
			    NPCM_FIU_UMA_CTS_DEV_NUM_SHIFT));
	regmap_update_bits(host->regmap, NPCM_FIU_UMA_CTS,
			   NPCM_FIU_UMA_CTS_SW_CS, 0);

	ret = npcm_fiu_uma_write(nor, transaction_code, address, true,
				 NULL, 0);
	if (ret)
		return ret;

	/* Starting the data writing loop in multiples of 8 */
	for (idx = 0; idx < num_data_chunks; ++idx) {
		ret = npcm_fiu_uma_write(nor, data[0], (u32)NULL, false,
					 &data[1], CHUNK_SIZE - 1);
		if (ret)
			return ret;

		data += CHUNK_SIZE;
	}

	/* Handling chunk remains */
	if (remain_data > 0) {
		ret = npcm_fiu_uma_write(nor, data[0], (u32)NULL, false,
					 &data[1], remain_data - 1);
		if (ret)
			return ret;
	}

	regmap_update_bits(host->regmap, NPCM_FIU_UMA_CTS,
			   NPCM_FIU_UMA_CTS_SW_CS, NPCM_FIU_UMA_CTS_SW_CS);

	return 0;
}

static ssize_t npcm_fiu_write(struct spi_nor *nor, loff_t to,
			      size_t len, const u_char *write_buf)
{
	u32 local_addr = (u32)to;
	struct mtd_info *mtd;
	u32 actual_size = 0;
	u32 cnt = (u32)len;
	int ret;

	mtd = &nor->mtd;

	if (cnt != 0) {
		while (cnt) {
			actual_size = ((((local_addr) / nor->page_size) + 1)
				       * nor->page_size) - (local_addr);
			if (actual_size > cnt)
				actual_size = cnt;

			ret = npcm_fiu_manualwrite(nor, nor->program_opcode,
						   local_addr,
						   (u_char *)write_buf,
						   actual_size);
			if (ret)
				return ret;

			write_buf += actual_size;
			local_addr += actual_size;
			cnt -= actual_size;
		}
	}

	return (len - cnt);
}

static void npcm_fiu_set_drd(struct spi_nor *nor, struct npcm_fiu_bus *host)
{
	regmap_update_bits(host->regmap, NPCM_FIU_DRD_CFG,
			   NPCM_FIU_DRD_CFG_ACCTYPE,
			   ilog2(spi_nor_get_protocol_addr_nbits
				 (nor->read_proto)) <<
			   NPCM_FIU_DRD_ACCTYPE_SHIFT);
	regmap_update_bits(host->regmap, NPCM_FIU_DRD_CFG,
			   NPCM_FIU_DRD_CFG_DBW,
			   ((nor->read_dummy *
			     spi_nor_get_protocol_addr_nbits(nor->read_proto))
			    / NUM_BITS_IN_BYTE) << NPCM_FIU_DRD_DBW_SHIFT);
	regmap_update_bits(host->regmap, NPCM_FIU_DRD_CFG,
			   NPCM_FIU_DRD_CFG_RDCMD, nor->read_opcode);
	regmap_update_bits(host->regmap, NPCM_FIU_DRD_CFG,
			   NPCM_FIU_DRD_CFG_ADDSIZ,
			   (nor->addr_width - 3) << NPCM_FIU_DRD_ADDSIZ_SHIFT);
}

static ssize_t npcm_fiu_read(struct spi_nor *nor, loff_t from, size_t len,
			     u_char *read_buf)
{
	struct npcm_chip *chip = nor->priv;
	struct npcm_fiu_bus *host = chip->host;
	int i, readlen, currlen;
	struct mtd_info *mtd;
	size_t retlen = 0;
	u8 *buf_ptr;
	u32 addr;
	int ret;

	mtd = &nor->mtd;

	if (chip->direct_read) {
		regmap_read(host->regmap, NPCM_FIU_DRD_CFG, &addr);
		if (host->direct_rd_proto != chip->direct_rd_proto) {
			npcm_fiu_set_drd(nor, host);
			host->direct_rd_proto = chip->direct_rd_proto;
		}
		npcm_fiu_direct_read(mtd, from, len, &retlen, read_buf);
	} else {
		i = 0;
		currlen = (int)len;

		do {
			addr = ((u32)from + i);
			if (currlen < 4)
				readlen = currlen;
			else
				readlen = 4;

			buf_ptr = read_buf + i;
			ret = npcm_fiu_uma_read(nor, nor->read_opcode, addr,
						true, buf_ptr, readlen);
			if (ret)
				return ret;

			i += readlen;
			currlen -= 4;
		} while (currlen > 0);

		retlen = i;
	}

	return retlen;
}

static int npcm_fiu_erase(struct spi_nor *nor, loff_t offs)
{
	return npcm_fiu_uma_write(nor, nor->erase_opcode, (u32)offs, true,
				  NULL, 0);
}

static int npcm_fiu_read_reg(struct spi_nor *nor, u8 opcode, u8 *buf,
			     int len)
{
	return npcm_fiu_uma_read(nor, opcode, 0, false, buf, len);
}

static int npcm_fiu_write_reg(struct spi_nor *nor, u8 opcode, u8 *buf,
			      int len)
{
	return npcm_fiu_uma_write(nor, opcode, 0, false, buf, len);
}

static int npcm_fiu_nor_prep(struct spi_nor *nor, enum spi_nor_ops ops)
{
	struct npcm_chip *chip = nor->priv;
	struct npcm_fiu_bus *host = chip->host;

	mutex_lock(&host->lock);

	return 0;
}

static void npcm_fiu_nor_unprep(struct spi_nor *nor, enum spi_nor_ops ops)
{
	struct npcm_chip *chip = nor->priv;
	struct npcm_fiu_bus *host = chip->host;

	mutex_unlock(&host->lock);
}

/* Expansion bus registers as mtd_ram device */
static int npcm_mtd_ram_register(struct device_node *np,
				 struct npcm_fiu_bus *host)
{
	struct device *dev = host->dev;
	struct npcm_chip *chip;
	struct mtd_info *mtd;
	struct spi_nor *nor;
	u32 chipselect;
	u32 rx_dummy = 0;
	int ret;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	ret = of_property_read_u32(np, "reg", &chipselect);
	if (ret) {
		dev_err(dev, "There's no reg property for %s\n",
			dev->of_node->full_name);
		return ret;
	}

	of_property_read_u32(np, "npcm,fiu-spix-rx-dummy-num", &rx_dummy);
	if (rx_dummy > FIU_DRD_MAX_DUMMY_NUMBER) {
		dev_warn(dev, "npcm,fiu-spix-rx-dummy-num %d not supported\n",
			 rx_dummy);
		rx_dummy = 0;
	}

	chip->host = host;
	chip->chipselect = chipselect;
	nor = &chip->nor;
	nor->dev = dev;
	nor->priv = chip;
	mtd = &nor->mtd;

	chip->flash_region_mapped_ptr =
		devm_ioremap(dev, (host->res_mem->start +
				   (host->info->max_map_size *
				    chip->chipselect)), MAP_SIZE_8MB);
	if (!chip->flash_region_mapped_ptr) {
		dev_err(dev, "Error mapping memory region!\n");
		return -ENOMEM;
	}

	/* Populate mtd_info data structure */
	*mtd = (struct mtd_info) {
		.dev		= { .parent = dev },
		.name		= "exp-bus",
		.type		= MTD_RAM,
		.priv		= nor,
		.size		= MAP_SIZE_8MB,
		.writesize	= 1,
		.writebufsize	= 1,
		.flags		= MTD_CAP_RAM,
		._read		= npcm_fiu_direct_read,
		._write		= npcm_fiu_direct_write,
	};

	/* set read and write direct to configuration to SPI-X mode */
	regmap_write(host->regmap, NPCM_FIU_DRD_CFG,
		     NPCM_FIU_DRD_16_BYTE_BURST);
	regmap_update_bits(host->regmap, NPCM_FIU_DRD_CFG,
			   NPCM_FIU_DRD_CFG_ACCTYPE,
			   DRD_SPI_X_MODE << NPCM_FIU_DRD_ACCTYPE_SHIFT);
	regmap_update_bits(host->regmap, NPCM_FIU_DRD_CFG,
			   NPCM_FIU_DRD_CFG_DBW,
			   rx_dummy << NPCM_FIU_DRD_DBW_SHIFT);
	regmap_write(host->regmap, NPCM_FIU_DWR_CFG,
		     NPCM_FIU_DWR_16_BYTE_BURST);
	regmap_update_bits(host->regmap, NPCM_FIU_DWR_CFG,
			   NPCM_FIU_DWR_CFG_ABPCK,
			   DWR_ABPCK_4_BIT_PER_CLK << NPCM_FIU_DWR_ABPCK_SHIFT);
	regmap_update_bits(host->regmap, NPCM_FIU_DWR_CFG,
			   NPCM_FIU_DWR_CFG_DBPCK,
			   DWR_DBPCK_4_BIT_PER_CLK << NPCM_FIU_DWR_DBPCK_SHIFT);

	ret = mtd_device_parse_register(mtd, NULL, NULL, NULL, 0);
	if (ret)
		return ret;

	host->chip[chip->chipselect] = chip;

	return 0;
}

static void npcm_fiu_enable_direct_rd(struct spi_nor *nor,
				      struct npcm_fiu_bus *host,
				      struct npcm_chip *chip)
{
	struct device *dev = host->dev;
	u32 flashsize;

	if (!host->res_mem) {
		dev_warn(dev, "Reserved memory not defined, direct read disabled\n");
		return;
	}

	/* direct read supports only I/O read mode */
	if (nor->read_proto != SNOR_PROTO_1_1_1 &&
	    nor->read_proto != SNOR_PROTO_1_2_2 &&
	    nor->read_proto != SNOR_PROTO_1_4_4) {
		dev_warn(dev, "Only Read I/O commands supported, direct read disabled\n");
		return;
	}

	flashsize = (u32)(nor->mtd.size >> 10) * 1024;
	if (flashsize == 0 || flashsize > host->info->max_map_size) {
		dev_warn(dev, "Flash size ecxeed(0x%x) map size(0x%x), direct read disabled\n"
			 , flashsize, host->info->max_map_size);
		return;
	}

	chip->flash_region_mapped_ptr =
		devm_ioremap(dev, (host->res_mem->start +
				   (host->info->max_map_size *
				    chip->chipselect)), flashsize);
	if (!chip->flash_region_mapped_ptr) {
		dev_warn(dev, "Error mapping memory region, direct read disabled\n");
		return;
	}

	npcm_fiu_set_drd(nor, host);

	host->direct_rd_proto = nor->read_proto;
	chip->direct_rd_proto = nor->read_proto;
	chip->direct_read = true;
}

/* Get spi flash device information and register it as a mtd device. */
static int npcm_fiu_nor_register(struct device_node *np,
				 struct npcm_fiu_bus *host)
{
	struct device *dev = host->dev;
	struct npcm_chip *chip;
	struct mtd_info *mtd;
	struct spi_nor *nor;
	u32 chipselect;
	int ret;
	u32 val;
	struct spi_nor_hwcaps hwcaps = {
		.mask = SNOR_HWCAPS_READ |
			SNOR_HWCAPS_READ_FAST |
			SNOR_HWCAPS_PP |
			SNOR_HWCAPS_PP_1_1_4 |
			SNOR_HWCAPS_PP_1_4_4 |
			SNOR_HWCAPS_PP_4_4_4,
	};

	/* This driver mode supports only NOR flash devices. */
	if (!of_device_is_compatible(np, "jedec,spi-nor")) {
		dev_err(dev, "The device is no compatible to jedec,spi-nor\n");
		return -ENOMEM;
	}

	ret = of_property_read_u32(np, "reg", &chipselect);
	if (ret) {
		dev_err(dev, "There's no reg property for %s\n", np->full_name);
		return ret;
	}

	if (chipselect >= host->info->max_cs) {
		dev_err(dev, "Flash device number exceeds the maximum chipselect number\n");
		return -ENOMEM;
	}

	if (!of_property_read_u32(np, "spi-rx-bus-width", &val)) {
		switch (val) {
		case 1:
			break;
		case 2:
			hwcaps.mask |= SNOR_HWCAPS_READ_1_1_2
				| SNOR_HWCAPS_READ_1_2_2
				| SNOR_HWCAPS_READ_2_2_2;
			break;
		case 4:
			hwcaps.mask |= SNOR_HWCAPS_READ_1_1_4
				 | SNOR_HWCAPS_READ_1_4_4
				 | SNOR_HWCAPS_READ_4_4_4;
				break;
		default:
			dev_warn(dev, "spi-rx-bus-width %d not supported\n", val);
			break;
		}
	}

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->host = host;
	chip->chipselect = chipselect;

	nor = &chip->nor;
	mtd = &nor->mtd;

	nor->dev = dev;
	nor->priv = chip;

	spi_nor_set_flash_node(nor, np);

	nor->prepare = npcm_fiu_nor_prep;
	nor->unprepare = npcm_fiu_nor_unprep;
	nor->read_reg = npcm_fiu_read_reg;
	nor->write_reg = npcm_fiu_write_reg;
	nor->read = npcm_fiu_read;
	nor->write = npcm_fiu_write;
	nor->erase = npcm_fiu_erase;

	ret = spi_nor_scan(nor, NULL, &hwcaps);
	if (ret)
		return ret;

	npcm_fiu_enable_direct_rd(nor, host, chip);
	ret = mtd_device_register(mtd, NULL, 0);
	if (ret) {
		dev_err(dev, "MTD NOR device register failed\n");
		return ret;
	}

	host->chip[chip->chipselect] = chip;
	return 0;
}

static void npcm_fiu_unregister_all(struct npcm_fiu_bus *host)
{
	struct npcm_chip *chip;
	int n;

	for (n = 0; n < host->info->max_cs; n++) {
		chip = host->chip[n];
		if (chip)
			mtd_device_unregister(&chip->nor.mtd);
	}
}

static void npcm_fiu_register_all(struct npcm_fiu_bus *host)
{
	struct device *dev = host->dev;
	struct device_node *np;
	int ret;

	for_each_available_child_of_node(dev->of_node, np) {
		if (host->spix_mode)
			ret = npcm_mtd_ram_register(np, host);
		else
			ret = npcm_fiu_nor_register(np, host);
		if (ret)
			dev_warn(dev, "npcm fiu %s registration failed\n", np->full_name);
	}
}

static const struct of_device_id npcm_fiu_dt_ids[] = {
	{ .compatible = "nuvoton,npcm750-fiu", .data = &npxm7xx_fiu_data  },
	{ /* sentinel */ }
};

static int npcm_fiu_probe(struct platform_device *pdev)
{
	const struct fiu_data *fiu_data_match;
	const struct of_device_id *match;
	struct device *dev = &pdev->dev;
	struct npcm_fiu_bus *host;
	struct resource *res;

	host = devm_kzalloc(dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	match = of_match_device(npcm_fiu_dt_ids, dev);
	if (!match || !match->data) {
		dev_err(dev, "No compatible OF match\n");
		return -ENODEV;
	}

	fiu_data_match = match->data;

	host->id = of_alias_get_id(dev->of_node, "fiu");
	if (host->id < 0 || host->id >= fiu_data_match->fiu_max) {
		dev_err(dev, "Invalid platform device id: %d\n", host->id);
		return -EINVAL;
	}

	host->info = &fiu_data_match->npcm_fiu_data_info[host->id];

	platform_set_drvdata(pdev, host);
	host->dev = dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "control");
	host->regbase = devm_ioremap_resource(dev, res);
	if (IS_ERR(host->regbase))
		return PTR_ERR(host->regbase);

	host->regmap = devm_regmap_init_mmio(dev, host->regbase,
					     &npcm_mtd_regmap_config);
	if (IS_ERR(host->regmap)) {
		dev_err(dev, "Failed to create regmap\n");
		return PTR_ERR(host->regmap);
	}

	host->res_mem = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						     "memory");
	host->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(host->clk))
		return PTR_ERR(host->clk);

	host->spix_mode = of_property_read_bool(dev->of_node, "spix-mode");

	mutex_init(&host->lock);
	clk_prepare_enable(host->clk);

	npcm_fiu_register_all(host);

	dev_info(dev, "NPCM %s probe succeed\n", host->info->name);

	return 0;
}

static int npcm_fiu_remove(struct platform_device *pdev)
{
	struct npcm_fiu_bus *host = platform_get_drvdata(pdev);

	npcm_fiu_unregister_all(host);
	mutex_destroy(&host->lock);
	clk_disable_unprepare(host->clk);
	return 0;
}

MODULE_DEVICE_TABLE(of, npcm_fiu_dt_ids);

static struct platform_driver npcm_fiu_driver = {
	.driver = {
		.name	= "NPCM-FIU",
		.bus	= &platform_bus_type,
		.of_match_table = npcm_fiu_dt_ids,
	},
	.probe      = npcm_fiu_probe,
	.remove	    = npcm_fiu_remove,
};
module_platform_driver(npcm_fiu_driver);

MODULE_DESCRIPTION("Nuvoton FLASH Interface Unit SPI Controller Driver");
MODULE_AUTHOR("Tomer Maimon <tomer.maimon@nuvoton.com>");
MODULE_LICENSE("GPL v2");
