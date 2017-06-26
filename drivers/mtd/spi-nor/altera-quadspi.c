/*
 * Copyright (C) 2014 Altera Corporation. All rights reserved.
 * Copyright (C) 2017 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <linux/bitrev.h>
#include <linux/module.h>
#include <linux/mtd/altera-quadspi.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/spi-nor.h>

#define ALTERA_QUADSPI_RESOURCE_NAME			"altera_quadspi"

#define EPCS_OPCODE_ID					1
#define NON_EPCS_OPCODE_ID				2

#define WRITE_CHECK					1
#define ERASE_CHECK					0

#define QUADSPI_SR_REG					0x0
#define QUADSPI_SR_MASK					0x0000000F

/* defines for device id register */
#define QUADSPI_SID_REG					0x4
#define QUADSPI_RDID_REG				0x8
#define QUADSPI_ID_MASK					0x000000FF

/*
 * QUADSPI_MEM_OP register offset
 *
 * The QUADSPI_MEM_OP register is used to do memory protect and erase operations
 *
 */
#define QUADSPI_MEM_OP_REG				0xC

#define QUADSPI_MEM_OP_CMD_MASK				0x00000003
#define QUADSPI_MEM_OP_BULK_ERASE_CMD			0x00000001
#define QUADSPI_MEM_OP_SECTOR_ERASE_CMD			0x00000002
#define QUADSPI_MEM_OP_SECTOR_PROTECT_CMD		0x00000003
#define QUADSPI_MEM_OP_SECTOR_WRITE_ENABLE_CMD		0x00000004
#define QUADSPI_MEM_OP_SECTOR_VALUE_MASK		0x0003FF00

#define QUADSPI_MEM_OP_SECTOR_PROTECT_SHIFT		8
#define QUADSPI_MEM_OP_SECTOR_PROTECT_VALUE_MASK	0x00001F00
/*
 * QUADSPI_ISR register offset
 *
 * The QUADSPI_ISR register is used to determine whether an invalid write or
 * erase operation trigerred an interrupt
 *
 */
#define QUADSPI_ISR_REG					0x10

#define QUADSPI_ISR_ILLEGAL_ERASE_MASK			0x00000001
#define QUADSPI_ISR_ILLEGAL_WRITE_MASK			0x00000002

/*
 * QUADSPI_IMR register offset
 *
 * The QUADSPI_IMR register is used to mask the invalid erase or the invalid
 * write interrupts.
 *
 */
#define QUADSPI_IMR_REG					0x14
#define QUADSPI_IMR_ILLEGAL_ERASE_MASK			0x00000001

#define QUADSPI_IMR_ILLEGAL_WRITE_MASK			0x00000002

#define QUADSPI_CHIP_SELECT_REG				0x18
#define QUADSPI_CHIP_SELECT_MASK			0x00000007
#define QUADSPI_CHIP_SELECT_0				0x00000001
#define QUADSPI_CHIP_SELECT_1				0x00000002
#define QUADSPI_CHIP_SELECT_2				0x00000004

#define QUADSPI_FLAG_STATUS_REG				0x1C
#define QUADSPI_DEV_ID_DATA_0				0x20
#define QUADSPI_DEV_ID_DATA_1				0x24
#define QUADSPI_DEV_ID_DATA_2				0x28
#define QUADSPI_DEV_ID_DATA_3				0x2C
#define QUADSPI_DEV_ID_DATA_4				0x30

#define QUADSPI_WIN_OCC_REG				0x4
#define QUADSPI_WIN_OCC_SFT				24

#define QUADSPI_WIN_SEL_REG				0x8

struct altera_quadspi {
	u32 opcode_id;
	void __iomem *csr_base;
	void __iomem *data_base;
	void __iomem *window_base;
	size_t window_size;
	u32 num_flashes;
	u32 flags;
	struct device *dev;
	struct altera_quadspi_flash *flash[ALTERA_QUADSPI_MAX_NUM_FLASH_CHIP];
	struct device_node *np[ALTERA_QUADSPI_MAX_NUM_FLASH_CHIP];
};

struct altera_quadspi_flash {
	struct spi_nor nor;
	struct altera_quadspi *q;
	u32 bank;
};

struct flash_device {
	char *name;
	u32 opcode_id;
	u32 device_id;
};

#ifdef DEBUG
static inline u32 alt_qspi_readl(void __iomem *base, off_t offset)
{
	u32 val = readl(base + offset);

	pr_info("%s 0x%x from offset 0x%lx\n", __func__, val, offset);
	return val;
}
static inline void alt_qspi_writel(u32 val, void __iomem *base, off_t offset)
{
	writel(val, base + offset);
	pr_info("%s 0x%x to offset 0x%lx\n", __func__, val, offset);
}
#else
#define alt_qspi_readl(base, offset) readl(base+offset)
#define alt_qspi_writel(val, base, offset) writel(val, base + offset)
#endif

static void altera_quadspi_chip_select(struct altera_quadspi *q, u32 bank)
{
	u32 val = 0;

	switch (bank) {
	case 0:
		val = QUADSPI_CHIP_SELECT_0;
		break;
	case 1:
		val = QUADSPI_CHIP_SELECT_1;
		break;
	case 2:
		val = QUADSPI_CHIP_SELECT_2;
		break;
	default:
		dev_err(q->dev, "invalid bank\n");
		return;
	}
	alt_qspi_writel(val, q->csr_base, QUADSPI_CHIP_SELECT_REG);
}

static int altera_quadspi_write_reg(struct spi_nor *nor, u8 opcode, u8 *val,
				    int len)
{
	struct altera_quadspi_flash *flash = nor->priv;
	struct altera_quadspi *q = flash->q;

	altera_quadspi_chip_select(q, flash->bank);

	switch (opcode) {
	case SPINOR_OP_WREN:
		dev_dbg(q->dev, "%s enabling write\n", __func__);
		alt_qspi_writel(QUADSPI_MEM_OP_SECTOR_WRITE_ENABLE_CMD,
				q->csr_base, QUADSPI_MEM_OP_REG);
		break;

	case SPINOR_OP_CHIP_ERASE:
		alt_qspi_writel(QUADSPI_MEM_OP_BULK_ERASE_CMD,
				q->csr_base, QUADSPI_MEM_OP_REG);
		break;

	default:
		dev_dbg(q->dev, "%s UNHANDLED write_reg 0x%x\n",
			__func__, opcode);

	}

	return 0;
}

static int altera_quadspi_read_reg(struct spi_nor *nor, u8 opcode, u8 *val,
				   int len)
{
	struct altera_quadspi_flash *flash = nor->priv;
	struct altera_quadspi *q = flash->q;
	u32 data = 0;

	memset(val, 0, len);

	altera_quadspi_chip_select(q, flash->bank);

	switch (opcode) {
	case SPINOR_OP_RDSR:
		data = alt_qspi_readl(q->csr_base, QUADSPI_SR_REG);
		dev_dbg(q->dev, "%s RDSR 0x%x\n", __func__, data);
		*val = (u8)data & QUADSPI_SR_MASK;
		break;
	case SPINOR_OP_RDID:
		if (q->opcode_id == EPCS_OPCODE_ID)
			data = alt_qspi_readl(q->csr_base, QUADSPI_SID_REG);
		else
			data = alt_qspi_readl(q->csr_base, QUADSPI_RDID_REG);

		*((u32 *)val) = data;
		break;
	case SPINOR_OP_RDFSR:
		data = alt_qspi_readl(q->csr_base, QUADSPI_FLAG_STATUS_REG);
		dev_dbg(q->dev, "%s RDFSR 0x%x\n", __func__, data);
		*val = (u8)(data & 0xff);
		break;
	default:
		dev_dbg(q->dev, "%s UNHANDLED read_reg 0x%x\n",
			__func__, opcode);
		*val = 0;
		break;
	}
	return 0;
}

static int altera_quadspi_write_erase_check(struct spi_nor *nor,
					    bool write_erase)
{
	struct altera_quadspi_flash *flash = nor->priv;
	struct altera_quadspi *q = flash->q;
	u32 val;
	u32 mask;

	if (write_erase)
		mask = QUADSPI_ISR_ILLEGAL_WRITE_MASK;
	else
		mask = QUADSPI_ISR_ILLEGAL_ERASE_MASK;

	val = alt_qspi_readl(q->csr_base, QUADSPI_ISR_REG);

	if (val & mask) {
		dev_err(nor->dev,
			"write/erase failed, sector might be protected\n");
		alt_qspi_writel(0, q->csr_base, QUADSPI_FLAG_STATUS_REG);

		return -EIO;
	}

	return 0;
}

static int altera_quadspi_addr_to_sector(struct mtd_info *mtd, uint64_t offset)
{
	if (mtd->erasesize_shift)
		return offset >> mtd->erasesize_shift;
	do_div(offset, mtd->erasesize);
	return offset;
}

static int altera_quadspi_erase(struct spi_nor *nor, loff_t offset)
{
	struct altera_quadspi_flash *flash = nor->priv;
	struct altera_quadspi *q = flash->q;
	struct mtd_info *mtd = &nor->mtd;
	u32 val;
	int sector_value;

	altera_quadspi_chip_select(q, flash->bank);

	sector_value = altera_quadspi_addr_to_sector(mtd, offset);

	dev_dbg(q->dev, "%s sector %d\n", __func__, sector_value);

	if (sector_value < 0)
		return -EINVAL;

	val = (sector_value << 8) & QUADSPI_MEM_OP_SECTOR_VALUE_MASK;

	val |= QUADSPI_MEM_OP_SECTOR_ERASE_CMD;

	alt_qspi_writel(val, q->csr_base, QUADSPI_MEM_OP_REG);

	dev_dbg(q->dev, "%s SR=0x%x FSR=0x%x\n", __func__,
		alt_qspi_readl(q->csr_base, QUADSPI_SR_REG),
		alt_qspi_readl(q->csr_base, QUADSPI_FLAG_STATUS_REG));

	return altera_quadspi_write_erase_check(nor, ERASE_CHECK);
}

#define WINDOW_ALIGN 4
#define WINDOW_MASK (WINDOW_ALIGN - 1)

static ssize_t altera_quadspi_windowed_read(struct altera_quadspi *q,
					    loff_t from,
					    size_t len, u_char *buf)
{
	size_t bytes_left = len;
	size_t bytes_to_read, i;
	loff_t next_window_off;
	u64 start_window;
	u32 window;
	u32 *dst;

	if ((from & WINDOW_MASK) || (len & WINDOW_MASK) ||
	    !IS_ALIGNED((unsigned long)buf, WINDOW_ALIGN)) {
		dev_err(q->dev, "%s only 32 bit aligned accesses allowed\n",
			__func__);
		return 0;
	}

	start_window = from;
	do_div(start_window, q->window_size);
	window = (u32)(start_window & 0xffffffff);

	next_window_off = (window + 1) * q->window_size;

	while (bytes_left > 0) {

		writel(window, q->window_base + QUADSPI_WIN_SEL_REG);

		bytes_to_read = min((size_t)bytes_left,
				    (size_t)(next_window_off - from));

		dev_dbg(q->dev,
			"window%u fr0x%llx next0x%llx left%zu num0x%zx\n",
			window,  from, next_window_off, bytes_left,
			bytes_to_read);

		dst = (u32 *)buf;
		for (i = 0; i < bytes_to_read; i += 4, dst++)
			*dst = readl(q->data_base +
				     (from & (q->window_size - 1)) + i);

		bytes_left -= bytes_to_read;
		buf += bytes_to_read;
		from += bytes_to_read;
		window++;
		next_window_off += q->window_size;
	}

	return len;
}
static ssize_t altera_quadspi_windowed_write(struct altera_quadspi *q,
					     loff_t to, size_t len,
					     const u_char *buf)
{
	size_t bytes_left = len;
	u32 window_mask = q->window_size - 1;
	u32 read_back;
	size_t bytes_to_write, i;
	loff_t next_window_off;
	u64 start_window;
	u32 window;
	const u32 *src;
	u32 words_can_write;

	if ((to & WINDOW_MASK) || (len & WINDOW_MASK) ||
	    !IS_ALIGNED((unsigned long)buf, WINDOW_ALIGN)) {
		dev_err(q->dev, "%s only 32 bit aligned accesses allowed\n",
			__func__);
		return 0;
	}

	start_window = to;
	do_div(start_window, q->window_size);
	window = (u32)(start_window & 0xffffffff);

	next_window_off = (window + 1) * q->window_size;

	while (bytes_left > 0) {

		writel(window, q->window_base + QUADSPI_WIN_SEL_REG);

		bytes_to_write = min((size_t)bytes_left,
				    (size_t)(next_window_off - to));

		dev_dbg(q->dev,
			"window%u to0x%llx next0x%llx left%zu num0x%zx\n",
			window,  to, next_window_off, bytes_left,
			bytes_to_write);

		src = (u32 *)buf;
		for (i = 0; i < bytes_to_write;) {
			words_can_write =
				readl(q->window_base + QUADSPI_WIN_OCC_REG) >>
				      QUADSPI_WIN_OCC_SFT;
			dev_dbg(q->dev, "can write 0x%x\n", words_can_write);

			for (; words_can_write > 0; words_can_write--) {
				writel(*src,
				       q->data_base +
				       (to & window_mask) + i);
				read_back = readl(q->data_base +
						  (to & window_mask) + i);
				if (*src != read_back) {
					dev_err(q->dev, "%s 0x%x != 0x%x\n",
						__func__, *src, read_back);
					return (len - bytes_left);
				}
				i += 4;
				src++;
			}
		}

		bytes_left -= bytes_to_write;
		buf += bytes_to_write;
		to += bytes_to_write;
		window++;
		next_window_off += q->window_size;
	}

	return len;
}

static ssize_t altera_quadspi_read(struct spi_nor *nor, loff_t from, size_t len,
				   u_char *buf)
{
	struct altera_quadspi_flash *flash = nor->priv;
	struct altera_quadspi *q = flash->q;
	size_t i;

	altera_quadspi_chip_select(q, flash->bank);

	if (q->window_size)
		altera_quadspi_windowed_read(q, from, len, buf);
	else
		memcpy_fromio(buf, q->data_base + from, len);

	if (q->flags & ALTERA_QUADSPI_FL_BITREV_READ) {
		for (i = 0; i < len; i++, buf++)
			*buf = bitrev8(*buf);
	}

	return len;
}

static ssize_t altera_quadspi_write(struct spi_nor *nor, loff_t to,
				    size_t len, const u_char *buf)
{
	struct altera_quadspi_flash *flash = nor->priv;
	struct altera_quadspi *q = flash->q;
	u_char *bitrev_buf = NULL;
	const u_char *src;
	u_char *dst;
	size_t i;
	int ret = 0;

	altera_quadspi_chip_select(q, flash->bank);

	if (q->flags & ALTERA_QUADSPI_FL_BITREV_WRITE) {
		bitrev_buf = devm_kzalloc(q->dev, len, GFP_KERNEL);
		if (!bitrev_buf)
			return 0;

		src = buf;
		dst = bitrev_buf;
		for (i = 0; i < len; i++, src++, dst++)
			*dst = bitrev8(*src);

		buf = bitrev_buf;
	}

	if (q->window_size)
		altera_quadspi_windowed_write(q, to, len, buf);
	else
		memcpy_toio(q->data_base + to, buf, len);


	if (bitrev_buf)
		devm_kfree(q->dev, bitrev_buf);

	ret = altera_quadspi_write_erase_check(nor, WRITE_CHECK);

	return len;

}

static int altera_quadspi_lock(struct spi_nor *nor, loff_t ofs, uint64_t len)
{
	struct altera_quadspi_flash *flash = nor->priv;
	struct altera_quadspi *q = flash->q;
	struct mtd_info *mtd = &nor->mtd;
	uint32_t offset = ofs;
	u32 sector_start, sector_end;
	uint64_t num_sectors;
	u32 mem_op;
	u32 sr_bp;
	u32 sr_tb;

	altera_quadspi_chip_select(q, flash->bank);

	sector_start = offset;
	sector_end = altera_quadspi_addr_to_sector(mtd, offset + len);
	num_sectors = mtd->size;
	do_div(num_sectors, mtd->erasesize);

	dev_dbg(nor->dev, "%s: sector start is %u,sector end is %u\n",
		__func__, sector_start, sector_end);

	if (sector_start >= num_sectors / 2) {
		sr_bp = fls(num_sectors - 1 - sector_start) + 1;
		sr_tb = 0;
	} else if ((sector_end < num_sectors / 2) &&
		  (q->opcode_id != EPCS_OPCODE_ID)) {
		sr_bp = fls(sector_end) + 1;
		sr_tb = 1;
	} else {
		sr_bp = 16;
		sr_tb = 0;
	}

	mem_op = (sr_tb << 12) | (sr_bp << 8);
	mem_op &= QUADSPI_MEM_OP_SECTOR_PROTECT_VALUE_MASK;
	mem_op |= QUADSPI_MEM_OP_SECTOR_PROTECT_CMD;

	alt_qspi_writel(mem_op, q->csr_base, QUADSPI_MEM_OP_REG);

	return 0;
}

static int altera_quadspi_unlock(struct spi_nor *nor, loff_t ofs, uint64_t len)
{
	struct altera_quadspi_flash *flash = nor->priv;
	struct altera_quadspi *q = flash->q;
	u32 mem_op;

	dev_dbg(nor->dev, "Unlock all protected area\n");

	altera_quadspi_chip_select(q, flash->bank);

	mem_op = QUADSPI_MEM_OP_SECTOR_PROTECT_CMD;
	alt_qspi_writel(mem_op, q->csr_base, QUADSPI_MEM_OP_REG);

	return 0;
}

static int altera_quadspi_setup_banks(struct device *dev,
				      u32 bank, struct device_node *np)
{
	struct altera_quadspi *q = dev_get_drvdata(dev);
	struct altera_quadspi_flash *flash;
	struct spi_nor *nor;
	int ret = 0;
	char modalias[40] = {0};
	struct spi_nor_hwcaps hwcaps = {
		.mask = SNOR_HWCAPS_READ |
			SNOR_HWCAPS_READ_FAST |
			SNOR_HWCAPS_READ_1_1_2 |
			SNOR_HWCAPS_READ_1_1_4 |
			SNOR_HWCAPS_PP,
	};

	if (bank > q->num_flashes - 1)
		return -EINVAL;

	altera_quadspi_chip_select(q, bank);

	flash = devm_kzalloc(q->dev, sizeof(*flash), GFP_KERNEL);
	if (!flash)
		return -ENOMEM;

	q->flash[bank] = flash;
	nor = &flash->nor;
	nor->dev = dev;
	nor->priv = flash;
	nor->mtd.priv = nor;
	flash->q = q;
	flash->bank = bank;
	spi_nor_set_flash_node(nor, np);

	/* spi nor framework*/
	nor->read_reg = altera_quadspi_read_reg;
	nor->write_reg = altera_quadspi_write_reg;
	nor->read = altera_quadspi_read;
	nor->write = altera_quadspi_write;
	nor->erase = altera_quadspi_erase;
	nor->flash_lock = altera_quadspi_lock;
	nor->flash_unlock = altera_quadspi_unlock;

	/* scanning flash and checking dev id */
#ifdef CONFIG_OF
	if (np && (of_modalias_node(np, modalias, sizeof(modalias)) < 0))
		return -EINVAL;
#endif

	ret = spi_nor_scan(nor, modalias, &hwcaps);
	if (ret) {
		dev_err(nor->dev, "flash not found\n");
		return ret;
	}

	ret =  mtd_device_register(&nor->mtd, NULL, 0);

	altera_quadspi_unlock(nor, 0, 0);

	return ret;
}

int altera_quadspi_create(struct device *dev, void __iomem *csr_base,
			  void __iomem *data_base, void __iomem *window_base,
			  size_t window_size, u32 flags)
{
	struct altera_quadspi *q;

	q = devm_kzalloc(dev, sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;

	q->dev = dev;
	q->csr_base = csr_base;
	q->data_base = data_base;
	q->window_base = window_base;
	q->window_size = window_size;

	q->flags = flags;

	dev_set_drvdata(dev, q);

	dev_dbg(dev, "%s SR=0x%x FSR=0x%x\n", __func__,
		alt_qspi_readl(q->csr_base, QUADSPI_SR_REG),
		alt_qspi_readl(q->csr_base, QUADSPI_FLAG_STATUS_REG));

	return 0;
}
EXPORT_SYMBOL_GPL(altera_quadspi_create);

int altera_qspi_add_bank(struct device *dev,
			 u32 bank, struct device_node *np)
{
	struct altera_quadspi *q = dev_get_drvdata(dev);

	if (q->num_flashes >= ALTERA_QUADSPI_MAX_NUM_FLASH_CHIP)
		return -ENOMEM;

	q->num_flashes++;

	return altera_quadspi_setup_banks(dev, bank, np);
}
EXPORT_SYMBOL_GPL(altera_qspi_add_bank);

int altera_quadspi_remove_banks(struct device *dev)
{
	struct altera_quadspi *q = dev_get_drvdata(dev);
	struct altera_quadspi_flash *flash;
	int i;
	int ret = 0;

	/* clean up for all nor flash */
	for (i = 0; i < q->num_flashes; i++) {
		flash = q->flash[i];
		if (!flash)
			continue;

		/* clean up mtd stuff */
		ret = mtd_device_unregister(&flash->nor.mtd);
		if (ret) {
			dev_err(dev, "error removing mtd\n");
			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(altera_quadspi_remove_banks);

MODULE_AUTHOR("Viet Nga Dao <vndao@altera.com>");
MODULE_AUTHOR("Yong Sern Lau <lau.yong.sern@intel.com>");
MODULE_AUTHOR("Matthew Gerlach <matthew.gerlach@linux.intel.com>");
MODULE_DESCRIPTION("Altera QuadSPI Version 2 Driver");
MODULE_LICENSE("GPL v2");
