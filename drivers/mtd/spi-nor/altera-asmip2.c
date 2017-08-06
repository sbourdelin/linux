/*
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

#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mtd/altera-asmip2.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/spi-nor.h>
#include <linux/of_device.h>

#define QSPI_ACTION_REG 0
#define QSPI_ACTION_RST BIT(0)
#define QSPI_ACTION_EN BIT(1)
#define QSPI_ACTION_SC BIT(2)
#define QSPI_ACTION_CHIP_SEL_SFT 4
#define QSPI_ACTION_DUMMY_SFT 8
#define QSPI_ACTION_READ_BACK_SFT 16

#define QSPI_FIFO_CNT_REG 4
#define QSPI_FIFO_DEPTH 0x200
#define QSPI_FIFO_CNT_MSK 0x3ff
#define QSPI_FIFO_CNT_RX_SFT 0
#define QSPI_FIFO_CNT_TX_SFT 12

#define QSPI_DATA_REG 0x8

#define QSPI_POLL_TIMEOUT 10000000
#define QSPI_POLL_INTERVAL 5

struct altera_asmip2 {
	void __iomem *csr_base;
	u32 num_flashes;
	struct device *dev;
	struct altera_asmip2_flash *flash[ALTERA_ASMIP2_MAX_NUM_FLASH_CHIP];
	struct mutex bus_mutex;
};

struct altera_asmip2_flash {
	struct spi_nor nor;
	struct altera_asmip2 *q;
	u32 bank;
};

static int altera_asmip2_write_reg(struct spi_nor *nor, u8 opcode, u8 *val,
				    int len)
{
	struct altera_asmip2_flash *flash = nor->priv;
	struct altera_asmip2 *q = flash->q;
	u32 reg;
	int ret;
	int i;

	if ((len + 1) > QSPI_FIFO_DEPTH) {
		dev_err(q->dev, "%s bad len %d > %d\n",
			__func__, len + 1, QSPI_FIFO_DEPTH);
		return -EINVAL;
	}

	writel(opcode, q->csr_base + QSPI_DATA_REG);

	for (i = 0; i < len; i++) {
		writel((u32)val[i], q->csr_base + QSPI_DATA_REG);
	}

	reg = QSPI_ACTION_EN | QSPI_ACTION_SC;

	writel(reg, q->csr_base + QSPI_ACTION_REG);

	ret = readl_poll_timeout(q->csr_base + QSPI_FIFO_CNT_REG, reg,
				 (((reg >> QSPI_FIFO_CNT_TX_SFT) &
				 QSPI_FIFO_CNT_MSK) == 0), QSPI_POLL_INTERVAL,
				 QSPI_POLL_TIMEOUT);
	if (ret)
		dev_err(q->dev, "%s timed out\n", __func__);

	reg = QSPI_ACTION_EN;

	writel(reg, q->csr_base + QSPI_ACTION_REG);

	return ret;
}

static int altera_asmip2_read_reg(struct spi_nor *nor, u8 opcode, u8 *val,
				   int len)
{
	struct altera_asmip2_flash *flash = nor->priv;
	struct altera_asmip2 *q = flash->q;
	u32 reg;
	int ret;
	int i;

	if (len > QSPI_FIFO_DEPTH) {
		dev_err(q->dev, "%s bad len %d > %d\n",
			__func__, len, QSPI_FIFO_DEPTH);
		return -EINVAL;
	}

	writel(opcode, q->csr_base + QSPI_DATA_REG);

	reg = QSPI_ACTION_EN | QSPI_ACTION_SC |
		(len << QSPI_ACTION_READ_BACK_SFT);

	writel(reg, q->csr_base + QSPI_ACTION_REG);

	ret = readl_poll_timeout(q->csr_base + QSPI_FIFO_CNT_REG, reg,
				 ((reg & QSPI_FIFO_CNT_MSK) == len),
				 QSPI_POLL_INTERVAL, QSPI_POLL_TIMEOUT);

	if (!ret)
		for (i = 0; i < len; i++) {
			reg = readl(q->csr_base + QSPI_DATA_REG);
			val[i] = (u8)(reg & 0xff);
		}
	else
		dev_err(q->dev, "%s timeout\n", __func__);

	writel(QSPI_ACTION_EN, q->csr_base + QSPI_ACTION_REG);

	return ret;
}

static ssize_t altera_asmip2_read(struct spi_nor *nor, loff_t from, size_t len,
				   u_char *buf)
{
	struct altera_asmip2_flash *flash = nor->priv;
	struct altera_asmip2 *q = flash->q;
	size_t bytes_to_read, i;
	u32 reg;
	int ret;

	bytes_to_read = min_t(size_t, len, QSPI_FIFO_DEPTH);

	writel(nor->read_opcode, q->csr_base + QSPI_DATA_REG);

	writel((from & 0xff000000) >> 24, q->csr_base + QSPI_DATA_REG);
	writel((from & 0x00ff0000) >> 16, q->csr_base + QSPI_DATA_REG);
	writel((from & 0x0000ff00) >> 8, q->csr_base + QSPI_DATA_REG);
	writel((from & 0x000000ff), q->csr_base + QSPI_DATA_REG);

	reg = QSPI_ACTION_EN | QSPI_ACTION_SC |
		(10 << QSPI_ACTION_DUMMY_SFT) |
		(bytes_to_read << QSPI_ACTION_READ_BACK_SFT);

	writel(reg, q->csr_base + QSPI_ACTION_REG);

	ret = readl_poll_timeout(q->csr_base + QSPI_FIFO_CNT_REG, reg,
				 ((reg & QSPI_FIFO_CNT_MSK) ==
				 bytes_to_read), QSPI_POLL_INTERVAL,
				 QSPI_POLL_TIMEOUT);
	if (ret) {
		dev_err(q->dev, "%s timed out\n", __func__);
		bytes_to_read = 0;
	} else
		for (i = 0; i < bytes_to_read; i++) {
			reg = readl(q->csr_base + QSPI_DATA_REG);
			*buf++ = (u8)(reg & 0xff);
		}

	writel(QSPI_ACTION_EN, q->csr_base + QSPI_ACTION_REG);

	return bytes_to_read;
}

static ssize_t altera_asmip2_write(struct spi_nor *nor, loff_t to,
				    size_t len, const u_char *buf)
{
	struct altera_asmip2_flash *flash = nor->priv;
	struct altera_asmip2 *q = flash->q;
	size_t bytes_to_write, i;
	u32 reg;
	int ret;

	bytes_to_write = min_t(size_t, len, (QSPI_FIFO_DEPTH - 5));

	writel(nor->program_opcode, q->csr_base + QSPI_DATA_REG);

	writel((to & 0xff000000) >> 24, q->csr_base + QSPI_DATA_REG);
	writel((to & 0x00ff0000) >> 16, q->csr_base + QSPI_DATA_REG);
	writel((to & 0x0000ff00) >> 8, q->csr_base + QSPI_DATA_REG);
	writel((to & 0x000000ff), q->csr_base + QSPI_DATA_REG);

	for (i = 0; i < bytes_to_write; i++) {
		reg = (u32)*buf++;
		writel(reg, q->csr_base + QSPI_DATA_REG);
	}

	reg = QSPI_ACTION_EN | QSPI_ACTION_SC;

	writel(reg, q->csr_base + QSPI_ACTION_REG);

	ret = readl_poll_timeout(q->csr_base + QSPI_FIFO_CNT_REG, reg,
				 (((reg >> QSPI_FIFO_CNT_TX_SFT) &
				 QSPI_FIFO_CNT_MSK) == 0), QSPI_POLL_INTERVAL,
				 QSPI_POLL_TIMEOUT);

	if (ret) {
		dev_err(q->dev,
			"%s timed out waiting for fifo to clear\n",
			__func__);
		bytes_to_write = 0;
	}

	writel(QSPI_ACTION_EN, q->csr_base + QSPI_ACTION_REG);

	return bytes_to_write;

}

static int altera_asmip2_prep(struct spi_nor *nor, enum spi_nor_ops ops)
{
	struct altera_asmip2_flash *flash = nor->priv;
	struct altera_asmip2 *q = flash->q;

	mutex_lock(&q->bus_mutex);

	return 0;
}

static void altera_asmip2_unprep(struct spi_nor *nor, enum spi_nor_ops ops)
{
	struct altera_asmip2_flash *flash = nor->priv;
	struct altera_asmip2 *q = flash->q;

	mutex_unlock(&q->bus_mutex);
}

static int altera_asmip2_setup_banks(struct device *dev,
				      u32 bank, struct device_node *np)
{
	const struct spi_nor_hwcaps hwcaps = {
		.mask = SNOR_HWCAPS_READ |
			SNOR_HWCAPS_READ_FAST |
			SNOR_HWCAPS_PP,
	};
	struct altera_asmip2 *q = dev_get_drvdata(dev);
	struct altera_asmip2_flash *flash;
	struct spi_nor *nor;
	int ret = 0;
	char modalias[40] = {0};

	if (bank > q->num_flashes - 1)
		return -EINVAL;

	flash = devm_kzalloc(q->dev, sizeof(*flash), GFP_KERNEL);
	if (!flash)
		return -ENOMEM;

	q->flash[bank] = flash;
	flash->q = q;
	flash->bank = bank;

	nor = &flash->nor;
	nor->dev = dev;
	nor->priv = flash;
	nor->mtd.priv = nor;
	spi_nor_set_flash_node(nor, np);

	/* spi nor framework*/
	nor->read_reg = altera_asmip2_read_reg;
	nor->write_reg = altera_asmip2_write_reg;
	nor->read = altera_asmip2_read;
	nor->write = altera_asmip2_write;
	nor->prepare = altera_asmip2_prep;
	nor->unprepare = altera_asmip2_unprep;

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

	return ret;
}

static int altera_asmip2_create(struct device *dev, void __iomem *csr_base)
{
	struct altera_asmip2 *q;
	u32 reg;

	q = devm_kzalloc(dev, sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;

	q->dev = dev;
	q->csr_base = csr_base;

	mutex_init(&q->bus_mutex);

	dev_set_drvdata(dev, q);

	reg = readl(q->csr_base + QSPI_ACTION_REG);
	if (!(reg & QSPI_ACTION_RST)) {
		writel((reg | QSPI_ACTION_RST), q->csr_base + QSPI_ACTION_REG);
		dev_info(dev, "%s asserting reset\n", __func__);
		udelay(10);
	}

	writel((reg & ~QSPI_ACTION_RST), q->csr_base + QSPI_ACTION_REG);
	udelay(10);

	return 0;
}

static int altera_qspi_add_bank(struct device *dev,
			 u32 bank, struct device_node *np)
{
	struct altera_asmip2 *q = dev_get_drvdata(dev);

	if (q->num_flashes >= ALTERA_ASMIP2_MAX_NUM_FLASH_CHIP)
		return -ENOMEM;

	q->num_flashes++;

	return altera_asmip2_setup_banks(dev, bank, np);
}

static int altera_asmip2_remove_banks(struct device *dev)
{
	struct altera_asmip2 *q = dev_get_drvdata(dev);
	struct altera_asmip2_flash *flash;
	int i;
	int ret = 0;

	if (!q)
		return -EINVAL;

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

static int __probe_with_data(struct platform_device *pdev,
			     struct altera_asmip2_plat_data *qdata)
{
	struct device *dev = &pdev->dev;
	int ret, i;

	ret = altera_asmip2_create(dev, qdata->csr_base);

	if (ret) {
		dev_err(dev, "failed to create qspi device %d\n", ret);
		return ret;
	}

	for (i = 0; i < qdata->num_chip_sel; i++) {
		ret = altera_qspi_add_bank(dev, i, NULL);
		if (ret) {
			dev_err(dev, "failed to add qspi bank %d\n", ret);
			break;
		}
	}

	return ret;
}

static int altera_asmip2_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct altera_asmip2_plat_data *qdata;
	struct resource *res;
	void __iomem *csr_base;
	u32 bank;
	int ret;
	struct device_node *pp;

	qdata = dev_get_platdata(dev);

	if (qdata)
		return __probe_with_data(pdev, qdata);

	if (!np) {
		dev_err(dev, "no device tree found %p\n", pdev);
		return -ENODEV;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	csr_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(csr_base)) {
		dev_err(dev, "%s: ERROR: failed to map csr base\n", __func__);
		return PTR_ERR(csr_base);
	}

	ret = altera_asmip2_create(dev, csr_base);

	if (ret) {
		dev_err(dev, "failed to create qspi device\n");
		return ret;
	}

	for_each_available_child_of_node(np, pp) {
		of_property_read_u32(pp, "reg", &bank);
		if (bank >= ALTERA_ASMIP2_MAX_NUM_FLASH_CHIP) {
			dev_err(dev, "bad reg value %u >= %u\n", bank,
				ALTERA_ASMIP2_MAX_NUM_FLASH_CHIP);
			goto error;
		}

		if (altera_qspi_add_bank(dev, bank, pp)) {
			dev_err(dev, "failed to add bank %u\n", bank);
			goto error;
		}
	}

	return 0;
error:
	altera_asmip2_remove_banks(dev);
	return -EIO;
}

static int altera_asmip2_remove(struct platform_device *pdev)
{
	if (!pdev) {
		dev_err(&pdev->dev, "%s NULL\n", __func__);
		return -EINVAL;
	} else {
		return altera_asmip2_remove_banks(&pdev->dev);
	}
}

static const struct of_device_id altera_asmip2_id_table[] = {

	{ .compatible = "altr,asmi_parallel2",},
	{}
};
MODULE_DEVICE_TABLE(of, altera_asmip2_id_table);

static struct platform_driver altera_asmip2_driver = {
	.driver = {
		.name = ALTERA_ASMIP2_DRV_NAME,
		.of_match_table = altera_asmip2_id_table,
	},
	.probe = altera_asmip2_probe,
	.remove = altera_asmip2_remove,
};
module_platform_driver(altera_asmip2_driver);

MODULE_AUTHOR("Matthew Gerlach <matthew.gerlach@linux.intel.com>");
MODULE_DESCRIPTION("Altera ASMI Parallel II");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" ALTERA_ASMIP2_DRV_NAME);
