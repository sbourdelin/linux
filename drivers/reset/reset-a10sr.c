/*
 *  Copyright Intel Corporation (C) 2017. All Rights Reserved
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
 *
 * Reset driver for Altera Arria10 MAX5 System Resource Chip
 *
 * Adapted from reset-socfpga.c
 */

#include <linux/err.h>
#include <linux/io.h>
#include <linux/mfd/altera-a10sr.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/types.h>

#include <dt-bindings/reset/altr,rst-mgr-a10sr.h>

/* Number of A10 System Controller Resets */
#define A10SR_RESETS		16

struct a10sr_reset {
	struct reset_controller_dev     rcdev;
	struct regmap *regmap;
};

static inline struct a10sr_reset *to_a10sr_rst(struct reset_controller_dev *rc)
{
	return container_of(rc, struct a10sr_reset, rcdev);
}

static inline int a10sr_reset_shift(unsigned long id)
{
	switch (id) {
	case A10SR_RESET_ENET_HPS:
		return 1;
	case A10SR_RESET_PCIE:
	case A10SR_RESET_FILE:
	case A10SR_RESET_BQSPI:
	case A10SR_RESET_USB:
		return id + 11;
	default:
		return -EINVAL;
	}
}

static int a10sr_reset_update(struct reset_controller_dev *rcdev,
			      unsigned long id, bool assert)
{
	struct a10sr_reset *a10r = to_a10sr_rst(rcdev);
	int offset = a10sr_reset_shift(id);
	u8 mask = ALTR_A10SR_REG_BIT_MASK(offset);
	int index = ALTR_A10SR_HPS_RST_REG + ALTR_A10SR_REG_OFFSET(offset);

	if (id >= rcdev->nr_resets)
		return -EINVAL;

	return regmap_update_bits(a10r->regmap, index, mask, assert ? 0 : mask);
}

static int a10sr_reset_assert(struct reset_controller_dev *rcdev,
			      unsigned long id)
{
	return a10sr_reset_update(rcdev, id, true);
}

static int a10sr_reset_deassert(struct reset_controller_dev *rcdev,
				unsigned long id)
{
	return a10sr_reset_update(rcdev, id, false);
}

static int a10sr_reset_status(struct reset_controller_dev *rcdev,
			      unsigned long id)
{
	int error;
	struct a10sr_reset *a10r = to_a10sr_rst(rcdev);
	int offset = a10sr_reset_shift(id);
	u8 mask = ALTR_A10SR_REG_BIT_MASK(offset);
	int index = ALTR_A10SR_HPS_RST_REG + ALTR_A10SR_REG_OFFSET(offset);
	unsigned int value;

	if (id >= rcdev->nr_resets)
		return -EINVAL;

	error = regmap_read(a10r->regmap, index, &value);
	if (error < 0)
		return -ENOMEM;

	return !!(value & mask);
}

static const struct reset_control_ops a10sr_reset_ops = {
	.assert		= a10sr_reset_assert,
	.deassert	= a10sr_reset_deassert,
	.status		= a10sr_reset_status,
};

static const struct of_device_id a10sr_reset_of_match[];
static int a10sr_reset_probe(struct platform_device *pdev)
{
	struct altr_a10sr *a10sr = dev_get_drvdata(pdev->dev.parent);
	struct a10sr_reset *a10r;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;

	/* Ensure we have a valid DT entry. */
	np = of_find_matching_node(NULL, a10sr_reset_of_match);
	if (!np) {
		dev_err(&pdev->dev, "A10 Reset DT Entry not found\n");
		return -EINVAL;
	}

	if (!of_find_property(np, "#reset-cells", NULL)) {
		dev_err(&pdev->dev, "%s missing #reset-cells property\n",
			np->full_name);
		return -EINVAL;
	}

	a10r = devm_kzalloc(&pdev->dev, sizeof(struct a10sr_reset),
			    GFP_KERNEL);
	if (!a10r)
		return -ENOMEM;

	a10r->rcdev.owner = THIS_MODULE;
	a10r->rcdev.nr_resets = A10SR_RESETS;
	a10r->rcdev.ops = &a10sr_reset_ops;
	a10r->rcdev.of_node = np;
	a10r->regmap = a10sr->regmap;

	platform_set_drvdata(pdev, a10r);

	return devm_reset_controller_register(dev, &a10r->rcdev);
}

static int a10sr_reset_remove(struct platform_device *pdev)
{
	struct a10sr_reset *a10r = platform_get_drvdata(pdev);

	reset_controller_unregister(&a10r->rcdev);

	return 0;
}

static const struct of_device_id a10sr_reset_of_match[] = {
	{ .compatible = "altr,a10sr-reset" },
	{ },
};
MODULE_DEVICE_TABLE(of, a10sr_reset_of_match);

static struct platform_driver a10sr_reset_driver = {
	.probe	= a10sr_reset_probe,
	.remove	= a10sr_reset_remove,
	.driver = {
		.name		= "altr_a10sr_reset",
		.owner		= THIS_MODULE,
	},
};
module_platform_driver(a10sr_reset_driver);

MODULE_AUTHOR("Thor Thayer <thor.thayer@linux.intel.com>");
MODULE_DESCRIPTION("Altera Arria10 System Resource Reset Controller Driver");
MODULE_LICENSE("GPL v2");
