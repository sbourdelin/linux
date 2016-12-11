/*
 * Technologic Systems TS-73xx SBC FPGA loader
 *
 * Copyright (C) 2016 Florian Fainelli <f.fainelli@gmail.com>
 *
 * FPGA Manager Driver for the on-board Altera Cyclone II FPGA found on
 * TS-7300, heavily based on load_fpga.c in their vendor tree.
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

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/bitrev.h>
#include <linux/fpga/fpga-mgr.h>

#define TS73XX_FPGA_DATA_REG	0
#define TS73XX_FPGA_CONFIG_REG	1

struct ts73xx_fpga_priv {
	void __iomem	*io_base;
	struct device	*dev;
};

static enum fpga_mgr_states ts73xx_fpga_state(struct fpga_manager *mgr)
{
	return FPGA_MGR_STATE_UNKNOWN;
}

static int ts73xx_fpga_write_init(struct fpga_manager *mgr, u32 flags,
				  const char *buf, size_t count)
{
	struct ts73xx_fpga_priv *priv = mgr->priv;

	/* Reset the FPGA */
	writeb(0, priv->io_base + TS73XX_FPGA_CONFIG_REG);
	udelay(30);
	writeb(0x2, priv->io_base + TS73XX_FPGA_CONFIG_REG);
	udelay(80);

	return 0;
}

static inline int ts73xx_fpga_can_write(struct ts73xx_fpga_priv *priv)
{
	unsigned int timeout = 1000;
	u8 reg;

	while (timeout--) {
		reg = readb(priv->io_base + TS73XX_FPGA_CONFIG_REG);
		if (!(reg & 0x1))
			return 0;
		cpu_relax();
	}

	return -ETIMEDOUT;
}

static int ts73xx_fpga_write(struct fpga_manager *mgr, const char *buf,
			     size_t count)
{
	struct ts73xx_fpga_priv *priv = mgr->priv;
	size_t i = 0;
	int ret;
	u8 reg;

	while (count--) {
		ret = ts73xx_fpga_can_write(priv);
		if (ret < 0)
			return ret;

		writeb(buf[i], priv->io_base + TS73XX_FPGA_DATA_REG);
		i++;
	}

	usleep_range(1000, 2000);
	reg = readb(priv->io_base + TS73XX_FPGA_CONFIG_REG);
	reg |= 0x8;
	writeb(reg, priv->io_base + TS73XX_FPGA_CONFIG_REG);
	usleep_range(1000, 2000);

	reg = readb(priv->io_base + TS73XX_FPGA_CONFIG_REG);
	reg &= ~0x8;
	writeb(reg, priv->io_base + TS73XX_FPGA_CONFIG_REG);

	return 0;
}

static int ts73xx_fpga_write_complete(struct fpga_manager *mgr, u32 flags)
{
	struct ts73xx_fpga_priv *priv = mgr->priv;
	u8 reg;

	reg = readb(priv->io_base + TS73XX_FPGA_CONFIG_REG);
	if ((reg & 0x4) != 0x4)
		return -ETIMEDOUT;

	return 0;
}

static const struct fpga_manager_ops ts73xx_fpga_ops = {
	.state		= ts73xx_fpga_state,
	.write_init	= ts73xx_fpga_write_init,
	.write		= ts73xx_fpga_write,
	.write_complete	= ts73xx_fpga_write_complete,
};

static int ts73xx_fpga_probe(struct platform_device *pdev)
{
	struct device *kdev = &pdev->dev;
	struct ts73xx_fpga_priv *priv;
	struct fpga_manager *mgr;
	struct resource *res;
	int err;

	priv = devm_kzalloc(kdev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = kdev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->io_base = devm_ioremap_resource(kdev, res);
	if (IS_ERR(priv->io_base))
		return PTR_ERR(priv->io_base);

	err = fpga_mgr_register(kdev, "TS-73xx FPGA Manager",
				&ts73xx_fpga_ops, priv);
	if (err) {
		dev_err(kdev, "failed to register FPGA manager\n");
		return err;
	}

	return err;
}

static int ts73xx_fpga_remove(struct platform_device *pdev)
{
	fpga_mgr_unregister(&pdev->dev);

	return 0;
}

static struct platform_driver ts73xx_fpga_driver = {
	.driver	= {
		.name	= "ts73xx-fpga-mgr",
	},
	.probe	= ts73xx_fpga_probe,
	.remove	= ts73xx_fpga_remove,
};
module_platform_driver(ts73xx_fpga_driver);

MODULE_AUTHOR("Florian Fainelli <f.fainelli@gmail.com>");
MODULE_DESCRIPTION("TS-73xx FPGA Manager driver");
MODULE_LICENSE("GPL v2");
