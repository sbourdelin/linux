// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2018 ROHM Semiconductors
// bd718xx-pwrkey.c -- ROHM BD71837MWV and BD71847 power button driver

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/mfd/bd71837.h>

struct bd718xx_pwrkey {
	struct input_dev *idev;
	struct bd71837 *mfd;
	int irq;
};

static irqreturn_t button_irq(int irq, void *_priv)
{
	struct input_dev *idev = (struct input_dev *)_priv;

	input_report_key(idev, KEY_POWER, 1);
	input_sync(idev);
	input_report_key(idev, KEY_POWER, 0);
	input_sync(idev);

	return IRQ_HANDLED;
}

static int bd718xx_pwr_btn_probe(struct platform_device *pdev)
{
	int err = -ENOMEM;
	struct bd718xx_pwrkey *pk;

	pk = devm_kzalloc(&pdev->dev, sizeof(*pk), GFP_KERNEL);
	if (!pk)
		goto err_out;

	pk->mfd = dev_get_drvdata(pdev->dev.parent);

	pk->idev = devm_input_allocate_device(&pdev->dev);
	if (!pk->idev)
		goto err_out;

	pk->idev->name = "bd718xx-pwrkey";
	pk->idev->phys = "bd718xx-pwrkey/input0";
	pk->idev->dev.parent = &pdev->dev;

	input_set_capability(pk->idev, EV_KEY, KEY_POWER);

	err = platform_get_irq_byname(pdev, "pwr-btn-s");
	if (err < 0) {
		dev_err(&pdev->dev, "could not get power key interrupt\n");
		goto err_out;
	}

	pk->irq = err;
	err = devm_request_threaded_irq(&pdev->dev, pk->irq, NULL, &button_irq,
					0, "bd718xx-pwrkey", pk);
	if (err)
		goto err_out;

	platform_set_drvdata(pdev, pk);
	err = regmap_update_bits(pk->mfd->regmap,
				 BD71837_REG_PWRONCONFIG0,
				 BD718XX_PWRBTN_SHORT_PRESS_MASK,
				 BD718XX_PWRBTN_SHORT_PRESS_10MS);
	if (err)
		goto err_out;

	err = input_register_device(pk->idev);

err_out:

	return err;
}

static struct platform_driver bd718xx_pwr_btn_driver = {
	.probe	= bd718xx_pwr_btn_probe,
	.driver = {
		.name	= "bd718xx-pwrkey",
	},
};
module_platform_driver(bd718xx_pwr_btn_driver);
MODULE_DESCRIPTION("Power button driver for buttons connected to ROHM bd71837/bd71847 PMIC");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matti Vaittinen <matti.vaittinen@fi.rohmeurope.com>");

