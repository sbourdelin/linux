/*
 * GPIO based MDIO bitbang driver.
 * Supports OpenFirmware.
 *
 * Copyright (c) 2008 CSE Semaphore Belgium.
 *  by Laurent Pinchart <laurentp@cse-semaphore.com>
 *
 * Copyright (C) 2008, Paulius Zaleckas <paulius.zaleckas@teltonika.lt>
 *
 * Based on earlier work by
 *
 * Copyright (c) 2003 Intracom S.A.
 *  by Pantelis Antoniou <panto@intracom.gr>
 *
 * 2005 (c) MontaVista Software, Inc.
 * Vitaly Bordug <vbordug@ru.mvista.com>
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2. This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/mdio-bitbang.h>
#include <linux/gpio.h>

#include <linux/of_gpio.h>
#include <linux/of_mdio.h>

struct mdio_gpio_info {
	struct mdiobb_ctrl ctrl;
	struct gpio_desc *mdc, *mdio, *mdo;
};

static void mdio_dir(struct mdiobb_ctrl *ctrl, int dir)
{
	struct mdio_gpio_info *bitbang =
		container_of(ctrl, struct mdio_gpio_info, ctrl);

	if (bitbang->mdo) {
		/* Separate output pin. Always set its value to high
		 * when changing direction. If direction is input,
		 * assume the pin serves as pull-up. If direction is
		 * output, the default value is high.
		 */
		gpiod_set_value(bitbang->mdo, 1);
		return;
	}

	if (dir)
		gpiod_direction_output(bitbang->mdio, 1);
	else
		gpiod_direction_input(bitbang->mdio);
}

static int mdio_get(struct mdiobb_ctrl *ctrl)
{
	struct mdio_gpio_info *bitbang =
		container_of(ctrl, struct mdio_gpio_info, ctrl);

	return gpiod_get_value(bitbang->mdio);
}

static void mdio_set(struct mdiobb_ctrl *ctrl, int what)
{
	struct mdio_gpio_info *bitbang =
		container_of(ctrl, struct mdio_gpio_info, ctrl);

	if (bitbang->mdo)
		gpiod_set_value(bitbang->mdo, what);
	else
		gpiod_set_value(bitbang->mdio, what);
}

static void mdc_set(struct mdiobb_ctrl *ctrl, int what)
{
	struct mdio_gpio_info *bitbang =
		container_of(ctrl, struct mdio_gpio_info, ctrl);

	gpiod_set_value(bitbang->mdc, what);
}

static const struct mdiobb_ops mdio_gpio_ops = {
	.owner = THIS_MODULE,
	.set_mdc = mdc_set,
	.set_mdio_dir = mdio_dir,
	.set_mdio_data = mdio_set,
	.get_mdio_data = mdio_get,
};

static struct mii_bus *mdio_gpio_bus_init(struct device *dev,
					  struct mdio_gpio_info *bitbang)
{
	unsigned long mdo_flags = GPIOF_OUT_INIT_HIGH;
	unsigned long mdc_flags = GPIOF_OUT_INIT_LOW;
	unsigned long mdio_flags = GPIOF_DIR_IN;
	struct device_node *np = dev->of_node;
	enum of_gpio_flags flags;
	struct mii_bus *new_bus;
	bool mdio_active_low;
	bool mdc_active_low;
	bool mdo_active_low;
	unsigned int mdio;
	unsigned int mdc;
	unsigned int mdo;
	int bus_id;
	int ret, i;

	ret = of_get_gpio_flags(np, 0, &flags);
	if (ret < 0)
		return NULL;

	mdc = ret;
	mdc_active_low = flags & OF_GPIO_ACTIVE_LOW;

	ret = of_get_gpio_flags(np, 1, &flags);
	if (ret < 0)
		return NULL;
	mdio = ret;
	mdio_active_low = flags & OF_GPIO_ACTIVE_LOW;

	ret = of_get_gpio_flags(np, 2, &flags);
	if (ret > 0) {
		mdo = ret;
		mdo_active_low = flags & OF_GPIO_ACTIVE_LOW;
	} else {
		mdo = 0;
	}

	bus_id = of_alias_get_id(np, "mdio-gpio");
	if (bus_id < 0) {
		dev_warn(dev, "failed to get alias id\n");
		bus_id = 0;
	}

	bitbang->ctrl.ops = &mdio_gpio_ops;
	bitbang->mdc = gpio_to_desc(mdc);
	if (mdc_active_low)
		mdc_flags = GPIOF_OUT_INIT_HIGH | GPIOF_ACTIVE_LOW;
	bitbang->mdio = gpio_to_desc(mdio);
	if (mdio_active_low)
		mdio_flags |= GPIOF_ACTIVE_LOW;
	if (mdo) {
		bitbang->mdo = gpio_to_desc(mdo);
		if (mdo_active_low)
			mdo_flags = GPIOF_OUT_INIT_LOW | GPIOF_ACTIVE_LOW;
	}

	new_bus = alloc_mdio_bitbang(&bitbang->ctrl);
	if (!new_bus)
		goto out;

	new_bus->name = "GPIO Bitbanged MDIO",
	new_bus->parent = dev;

	if (new_bus->phy_mask == ~0)
		goto out_free_bus;

	for (i = 0; i < PHY_MAX_ADDR; i++)
		if (!new_bus->irq[i])
			new_bus->irq[i] = PHY_POLL;

	if (bus_id != -1)
		snprintf(new_bus->id, MII_BUS_ID_SIZE, "gpio-%x", bus_id);
	else
		strncpy(new_bus->id, "gpio", MII_BUS_ID_SIZE);

	if (devm_gpio_request_one(dev, mdc, mdc_flags, "mdc"))
		goto out_free_bus;

	if (devm_gpio_request_one(dev, mdio, mdio_flags, "mdio"))
		goto out_free_bus;

	if (mdo && devm_gpio_request_one(dev, mdo, mdo_flags, "mdo"))
		goto out_free_bus;

	dev_set_drvdata(dev, new_bus);

	return new_bus;

out_free_bus:
	free_mdio_bitbang(new_bus);
out:
	return NULL;
}

static void mdio_gpio_bus_deinit(struct device *dev)
{
	struct mii_bus *bus = dev_get_drvdata(dev);

	free_mdio_bitbang(bus);
}

static void mdio_gpio_bus_destroy(struct device *dev)
{
	struct mii_bus *bus = dev_get_drvdata(dev);

	mdiobus_unregister(bus);
	mdio_gpio_bus_deinit(dev);
}

static int mdio_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mdio_gpio_info *bitbang;
	struct mii_bus *new_bus;
	struct device_node *np;
	int ret;

	np = dev->of_node;
	bitbang = devm_kzalloc(dev, sizeof(*bitbang), GFP_KERNEL);
	if (!bitbang)
		return -ENOMEM;

	new_bus = mdio_gpio_bus_init(dev, bitbang);
	if (!new_bus)
		return -ENODEV;

	ret = of_mdiobus_register(new_bus, np);
	if (ret)
		mdio_gpio_bus_deinit(dev);

	return ret;
}

static int mdio_gpio_remove(struct platform_device *pdev)
{
	mdio_gpio_bus_destroy(&pdev->dev);

	return 0;
}

static const struct of_device_id mdio_gpio_of_match[] = {
	{ .compatible = "virtual,mdio-gpio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mdio_gpio_of_match);

static struct platform_driver mdio_gpio_driver = {
	.probe = mdio_gpio_probe,
	.remove = mdio_gpio_remove,
	.driver		= {
		.name	= "mdio-gpio",
		.of_match_table = mdio_gpio_of_match,
	},
};

module_platform_driver(mdio_gpio_driver);

MODULE_ALIAS("platform:mdio-gpio");
MODULE_AUTHOR("Laurent Pinchart, Paulius Zaleckas");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Generic driver for MDIO bus emulation using GPIO");
