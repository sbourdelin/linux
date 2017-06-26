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

#include <linux/module.h>
#include <linux/mtd/altera-quadspi.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>

static int altera_quadspi_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct resource *res;
	void __iomem *csr_base;
	void __iomem *data_base;
	void __iomem *window_base = NULL;
	u32 window_size = 0;
	u32 flags = 0;
	u32 bank;
	int ret;
	struct device_node *pp;

	if (!np) {
		dev_err(dev, "no device found\n");
		return -ENODEV;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "avl_csr");
	csr_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(csr_base)) {
		dev_err(dev, "%s: ERROR: failed to map csr base\n", __func__);
		return PTR_ERR(csr_base);
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "avl_mem");
	data_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(data_base)) {
		dev_err(dev, "%s: ERROR: failed to map data base\n", __func__);
		return PTR_ERR(data_base);
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "avl_window");
	if (res) {
		window_base = NULL;
		window_base = devm_ioremap_resource(dev, res);
		if (IS_ERR(window_base)) {
			dev_err(dev, "%s: ERROR: failed to map window base\n",
				__func__);
			return PTR_ERR(data_base);
		}

		of_property_read_u32(dev->of_node, "window-size", &window_size);

		if (!window_size) {
			dev_err(dev,
				"alv_window defined, %s",
				"but no window-size defined\n");
			return -EINVAL;
		}
	}

	if (of_property_read_bool(np, "read-bit-reverse"))
		flags |= ALTERA_QUADSPI_FL_BITREV_READ;

	if (of_property_read_bool(np, "write-bit-reverse"))
		flags |= ALTERA_QUADSPI_FL_BITREV_WRITE;

	ret = altera_quadspi_create(dev, csr_base, data_base,
				    window_base, (size_t)window_size, flags);

	if (ret) {
		dev_err(dev, "failed to create qspi device\n");
		return ret;
	}

	for_each_available_child_of_node(np, pp) {
		of_property_read_u32(pp, "reg", &bank);
		if (bank >= ALTERA_QUADSPI_MAX_NUM_FLASH_CHIP) {
			dev_err(dev, "bad reg value %u >= %u\n", bank,
				ALTERA_QUADSPI_MAX_NUM_FLASH_CHIP);
			goto error;
		}

		if (altera_qspi_add_bank(dev, bank, pp)) {
			dev_err(dev, "failed to add bank %u\n", bank);
			goto error;
		}
	}

	return 0;
error:
	altera_quadspi_remove_banks(dev);
	return -EIO;
}

static int altera_quadspi_remove(struct platform_device *pdev)
{
	return altera_quadspi_remove_banks(&pdev->dev);
}

static const struct of_device_id altera_quadspi_id_table[] = {

	{ .compatible = "altr,quadspi-v2",},
	{}
};
MODULE_DEVICE_TABLE(of, altera_quadspi_id_table);

static struct platform_driver altera_quadspi_driver = {
	.driver = {
		.name = "altera_quadspi_platform",
		.of_match_table = altera_quadspi_id_table,
	},
	.probe = altera_quadspi_probe,
	.remove = altera_quadspi_remove,
};
module_platform_driver(altera_quadspi_driver);

MODULE_AUTHOR("Viet Nga Dao <vndao@altera.com>");
MODULE_AUTHOR("Yong Sern Lau <lau.yong.sern@intel.com>");
MODULE_AUTHOR("Matthew Gerlach <matthew.gerlach@linux.intel.com>");
MODULE_DESCRIPTION("Altera QuadSPI Version 2 Platform Driver");
MODULE_LICENSE("GPL v2");
