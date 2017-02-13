/*
 * simple sigma delta modulator driver
 *
 * Copyright (C) 2016, STMicroelectronics - All Rights Reserved
 * Author: Arnaud Pouliquen <arnaud.pouliquen@st.com>.
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

#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/of_device.h>

#include <linux/iio/triggered_buffer.h>

static int simple_sd_of_xlate(struct iio_dev *iio,
			      const struct of_phandle_args *iiospec)
{
	dev_dbg(&iio->dev, "%s:\n", __func__);
	if (iiospec->args[0] != 0) {
		dev_err(&iio->dev, "Only one channel supported\n");
		return -EINVAL;
	}

	return 0;
}

static const struct iio_info simple_sd_iio_info = {
	.of_xlate = simple_sd_of_xlate,
};

static const struct iio_buffer_setup_ops simple_sd_buffer_ops;

static int simple_sd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *iio;
	struct iio_chan_spec *ch;

	dev_dbg(&pdev->dev, "%s:\n", __func__);
	iio = devm_iio_device_alloc(dev, 0);
	if (!iio)
		return -ENOMEM;

	/* Define one channel */
	ch = devm_kzalloc(&iio->dev, sizeof(*ch), GFP_KERNEL);
	if (!ch)
		return -ENOMEM;

	iio->dev.parent = dev;
	iio->dev.of_node = dev->of_node;
	iio->name = dev_name(dev);
	iio->info = &simple_sd_iio_info;
	iio->modes = INDIO_BUFFER_HARDWARE;

	ch->type = IIO_VOLTAGE;
	ch->indexed = 1;
	ch->scan_index = 0;
	ch->scan_type.sign = 'u';
	ch->scan_type.realbits = 1;
	ch->scan_type.storagebits = 1;
	ch->scan_type.shift = 0;

	iio->num_channels = 1;
	iio->channels = ch;

	platform_set_drvdata(pdev, iio);

	return iio_device_register(iio);
}

static int simple_sd_remove(struct platform_device *pdev)
{
	struct iio_dev *iio = platform_get_drvdata(pdev);

	iio_device_unregister(iio);

	return 0;
}

static const struct of_device_id sd_adc_of_match[] = {
	{ .compatible = "sd-modulator" },
	{ }
};
MODULE_DEVICE_TABLE(of, adc081c_of_match);

static struct platform_driver simple_sd_adc = {
	.driver = {
		.name = "simple_sd_adc",
		.of_match_table = of_match_ptr(sd_adc_of_match),
	},
	.probe = simple_sd_probe,
	.remove = simple_sd_remove,
};

module_platform_driver(simple_sd_adc);

MODULE_DESCRIPTION("simple signma delta modulator");
MODULE_AUTHOR("Arnaud Pouliquen <arnaud.pouliquen@st.com>");
MODULE_LICENSE("GPL v2");
