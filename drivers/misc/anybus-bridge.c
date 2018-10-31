// SPDX-License-Identifier: GPL-2.0
/*
 * Arcx Anybus Bridge driver
 *
 * Copyright (C) 2018 Arcx Inc
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/idr.h>
#include <linux/spinlock.h>
#include <linux/reset-controller.h>

#define CPLD_STATUS1		0x80
#define CPLD_CONTROL		0x80
#define CPLD_CONTROL_CRST	0x40
#define CPLD_CONTROL_RST1	0x04
#define CPLD_CONTROL_RST2	0x80
#define CPLD_STATUS1_AB		0x02
#define CPLD_STATUS1_CAN_POWER	0x01
#define CPLD_DESIGN_LO		0x81
#define CPLD_DESIGN_HI		0x82
#define CPLD_CAP		0x83
#define CPLD_CAP_COMPAT		0x01
#define CPLD_CAP_SEP_RESETS	0x02

struct bridge_priv {
	struct device *class_dev;
	struct reset_controller_dev rcdev;
	bool common_reset;
	int reset_gpio;
	void __iomem *cpld_base;
	spinlock_t regs_lock;
	u8 control_reg;
	char version[3];
	u16 design_no;
};

static void do_reset(struct bridge_priv *cd, u8 rst_bit, bool reset)
{
	unsigned long flags;

	spin_lock_irqsave(&cd->regs_lock, flags);
	/*
	 * CPLD_CONTROL is write-only, so cache its value in
	 * cd->control_reg
	 */
	if (reset)
		cd->control_reg &= ~rst_bit;
	else
		cd->control_reg |= rst_bit;
	writeb(cd->control_reg, cd->cpld_base + CPLD_CONTROL);
	/*
	 * h/w work-around:
	 * the hardware is 'too fast', so a reset followed by an immediate
	 * not-reset will _not_ change the anybus reset line in any way,
	 * losing the reset. to prevent this from happening, introduce
	 * a minimum reset duration.
	 * Verified minimum safe duration required using a scope
	 * on 14-June-2018: 100 us.
	 */
	if (reset)
		udelay(100);
	spin_unlock_irqrestore(&cd->regs_lock, flags);
}

static int anybuss_reset(struct bridge_priv *cd,
			     unsigned long id, bool reset)
{
	if (id >= 2)
		return -EINVAL;
	if (cd->common_reset)
		do_reset(cd, CPLD_CONTROL_CRST, reset);
	else
		do_reset(cd, id ? CPLD_CONTROL_RST2 : CPLD_CONTROL_RST1, reset);
	return 0;
}

static int anybuss_reset_assert(struct reset_controller_dev *rcdev,
			     unsigned long id)
{
	struct bridge_priv *cd = container_of(rcdev, struct bridge_priv, rcdev);

	return anybuss_reset(cd, id, true);
}

static int anybuss_reset_deassert(struct reset_controller_dev *rcdev,
			     unsigned long id)
{
	struct bridge_priv *cd = container_of(rcdev, struct bridge_priv, rcdev);

	return anybuss_reset(cd, id, false);
}

static const struct reset_control_ops anybuss_reset_ops = {
	.assert		= anybuss_reset_assert,
	.deassert	= anybuss_reset_deassert,
};

static ssize_t version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bridge_priv *cd = dev_get_drvdata(dev);

	return sprintf(buf, "%s\n", cd->version);
}
static DEVICE_ATTR_RO(version);

static ssize_t design_number_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bridge_priv *cd = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", cd->design_no);
}
static DEVICE_ATTR_RO(design_number);

static ssize_t can_power_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bridge_priv *cd = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n",
		!(readb(cd->cpld_base + CPLD_STATUS1) &
					CPLD_STATUS1_CAN_POWER));
}
static DEVICE_ATTR_RO(can_power);

static struct attribute *bridge_attributes[] = {
	&dev_attr_version.attr,
	&dev_attr_design_number.attr,
	&dev_attr_can_power.attr,
	NULL,
};

static struct attribute_group bridge_attribute_group = {
	.attrs = bridge_attributes,
};

static const struct attribute_group *bridge_attribute_groups[] = {
	&bridge_attribute_group,
	NULL,
};

static void bridge_device_release(struct device *dev)
{
	kfree(dev);
}

static struct class *bridge_class;
static DEFINE_IDA(bridge_index_ida);

static int bridge_probe(struct platform_device *pdev)
{
	struct bridge_priv *cd;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int err, id;
	struct resource *res;
	u8 status1, cap;

	cd = devm_kzalloc(dev, sizeof(*cd), GFP_KERNEL);
	if (!cd)
		return -ENOMEM;
	dev_set_drvdata(dev, cd);
	spin_lock_init(&cd->regs_lock);
	cd->reset_gpio = of_get_named_gpio(np, "reset-gpios", 0);
	if (!gpio_is_valid(cd->reset_gpio)) {
		dev_err(dev, "reset-gpios not found\n");
		return -EINVAL;
	}
	devm_gpio_request(dev, cd->reset_gpio, NULL);
	gpio_direction_output(cd->reset_gpio, 0);

	/* CPLD control memory, sits at index 0 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	cd->cpld_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(cd->cpld_base)) {
		dev_err(dev,
			"failed to map cpld base address\n");
		return PTR_ERR(cd->cpld_base);
	}

	/* identify cpld */
	status1 = readb(cd->cpld_base + CPLD_STATUS1);
	cd->design_no = (readb(cd->cpld_base + CPLD_DESIGN_HI) << 8) |
				readb(cd->cpld_base + CPLD_DESIGN_LO);
	snprintf(cd->version, sizeof(cd->version), "%c%d",
			'A' + ((status1>>5) & 0x7),
			(status1>>2) & 0x7);
	dev_info(dev, "Bridge is design number %d, revision %s\n",
		cd->design_no,
		cd->version);
	cap = readb(cd->cpld_base + CPLD_CAP);
	if (!(cap & CPLD_CAP_COMPAT)) {
		dev_err(dev, "unsupported bridge [cap=0x%02X]", cap);
		return -ENODEV;
	}

	if (status1 & CPLD_STATUS1_AB) {
		dev_info(dev, "Bridge has anybus-S slot(s)");
		cd->common_reset = !(cap & CPLD_CAP_SEP_RESETS);
		dev_info(dev, "Bridge supports %s", cd->common_reset ?
			"a common reset" : "separate resets");
		cd->rcdev.owner	= THIS_MODULE;
		cd->rcdev.nr_resets = 2;
		cd->rcdev.ops = &anybuss_reset_ops;
		cd->rcdev.of_node = dev->of_node;
		err = devm_reset_controller_register(dev, &cd->rcdev);
		if (err)
			return err;
	}

	id = ida_simple_get(&bridge_index_ida, 0, 0, GFP_KERNEL);
	if (id < 0)
		return id;
	/* make bridge info visible to userspace */
	cd->class_dev = kzalloc(sizeof(*cd->class_dev), GFP_KERNEL);
	if (!cd->class_dev) {
		err = -ENOMEM;
		goto out_ida;
	}
	cd->class_dev->class = bridge_class;
	cd->class_dev->groups = bridge_attribute_groups;
	cd->class_dev->parent = dev;
	cd->class_dev->id = id;
	cd->class_dev->release = bridge_device_release;
	dev_set_name(cd->class_dev, "bridge%d", cd->class_dev->id);
	dev_set_drvdata(cd->class_dev, cd);
	err = device_register(cd->class_dev);
	if (err)
		goto out_dev;
	return 0;
out_dev:
	put_device(cd->class_dev);
out_ida:
	ida_simple_remove(&bridge_index_ida, id);
	return err;
}

static int bridge_remove(struct platform_device *pdev)
{
	struct bridge_priv *cd = platform_get_drvdata(pdev);
	int id = cd->class_dev->id;

	device_unregister(cd->class_dev);
	ida_simple_remove(&bridge_index_ida, id);
	gpio_direction_input(cd->reset_gpio);
	return 0;
}

static const struct of_device_id bridge_of_match[] = {
	{ .compatible = "arcx,anybus-bridge" },
	{ }
};

MODULE_DEVICE_TABLE(of, bridge_of_match);

static struct platform_driver bridge_driver = {
	.probe = bridge_probe,
	.remove = bridge_remove,
	.driver		= {
		.name   = "arcx-anybus-bridge",
		.owner	= THIS_MODULE,
		.of_match_table	= of_match_ptr(bridge_of_match),
	},
};

static int __init bridge_init(void)
{
	int err;

	bridge_class = class_create(THIS_MODULE, "arcx_anybus_bridge");
	if (!IS_ERR(bridge_class)) {
		err = platform_driver_register(&bridge_driver);
		if (err)
			class_destroy(bridge_class);
	} else
		err = PTR_ERR(bridge_class);
	return err;
}

static void __exit bridge_exit(void)
{
	platform_driver_unregister(&bridge_driver);
	class_destroy(bridge_class);
}

module_init(bridge_init);
module_exit(bridge_exit);

MODULE_DESCRIPTION("Arcx Anybus Bridge driver");
MODULE_AUTHOR("Sven Van Asbroeck <svendev@arcx.com>");
MODULE_LICENSE("GPL v2");
