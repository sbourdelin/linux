/*
 * Industrial I/O - GPIO based trigger support
 *
 * Copyright (C) 2017, STMicroelectronics - All Rights Reserved
 * Author: Fabrice Gasnier <fabrice.gasnier@st.com>.
 *
 * License type: GPLv2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/gpio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>

static const struct iio_trigger_ops iio_gpio_trigger_ops = {
	.owner = THIS_MODULE,
};

static int iio_gpio_trigger_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct gpio_desc *gpio;
	char name[16];
	const char *label = NULL;
	struct iio_trigger *trig;
	unsigned long irqflags = IRQF_SHARED;
	int irq, ret;

	gpio = devm_gpiod_get(&pdev->dev, NULL, GPIOD_IN);
	if (IS_ERR(gpio)) {
		if (PTR_ERR(gpio) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "gpio get failed (%ld)\n",
				PTR_ERR(gpio));
		return PTR_ERR(gpio);
	}

	if (of_property_read_string(np, "label", &label))
		snprintf(name, sizeof(name), "gpiotrig%d", desc_to_gpio(gpio));

	if (of_property_read_bool(np, "gpio-trigger-rising-edge"))
		irqflags |= IRQF_TRIGGER_RISING;

	if (of_property_read_bool(np, "gpio-trigger-falling-edge"))
		irqflags |= IRQF_TRIGGER_FALLING;

	/* Default to rising edge */
	if (!(irqflags & IRQF_TRIGGER_MASK))
		irqflags |= IRQF_TRIGGER_RISING;

	trig = devm_iio_trigger_alloc(&pdev->dev, "%s", label ? label : name);
	if (!trig)
		return -ENOMEM;
	trig->dev.parent = &pdev->dev;
	trig->dev.of_node = pdev->dev.of_node;
	trig->ops = &iio_gpio_trigger_ops;

	irq = gpiod_to_irq(gpio);
	if (irq < 0) {
		dev_err(&pdev->dev, "gpio %d to irq failed (%d)\n",
			desc_to_gpio(gpio), irq);
		return irq;
	}

	ret = devm_request_irq(&pdev->dev, irq,
			       iio_trigger_generic_data_rdy_poll, irqflags,
			       trig->name, trig);
	if (ret) {
		dev_err(&pdev->dev, "request IRQ %d failed\n", irq);
		return ret;
	}

	ret = devm_iio_trigger_register(&pdev->dev, trig);
	if (ret)
		return ret;

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id iio_gpio_trigger_of_match[] = {
	{ .compatible = "iio-gpio-trigger" },
	{},
};
MODULE_DEVICE_TABLE(of, iio_gpio_trigger_of_match);
#endif

static struct platform_driver iio_gpio_trigger_driver = {
	.probe = iio_gpio_trigger_probe,
	.driver = {
		.name = "iio-gpio-trigger",
		.of_match_table = of_match_ptr(iio_gpio_trigger_of_match),
	},
};
module_platform_driver(iio_gpio_trigger_driver);

MODULE_AUTHOR("Fabrice Gasnier <fabrice.gasnier@st.com>");
MODULE_DESCRIPTION("GPIO trigger for iio subsystem");
MODULE_LICENSE("GPL v2");
