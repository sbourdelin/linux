/*
 * Basic sigma delta modulator driver
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

static int iio_sd_mod_of_xlate(struct iio_dev *iio,
			       const struct of_phandle_args *iiospec)
{
	dev_dbg(&iio->dev, "%s:\n", __func__);
	if (iiospec->args[0] != 0) {
		dev_err(&iio->dev, "Only one channel supported\n");
		return -EINVAL;
	}

	return 0;
}

static const struct iio_info iio_sd_mod_iio_info = {
	.of_xlate = iio_sd_mod_of_xlate,
};

static const struct iio_chan_spec stm32_dfsdm_ch = {
	.type = IIO_VOLTAGE,
	.indexed = 1,
	.scan_index = 0,
	.scan_type = {
		.sign = 'u',
		.realbits = 1,
		.shift = 0,
	},
};

static int iio_sd_mod_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *iio;

	dev_dbg(&pdev->dev, "%s:\n", __func__);
	iio = devm_iio_device_alloc(dev, 0);
	if (!iio)
		return -ENOMEM;

	iio->dev.parent = dev;
	iio->dev.of_node = dev->of_node;
	iio->name = dev_name(dev);
	iio->info = &iio_sd_mod_iio_info;
	iio->modes = INDIO_BUFFER_HARDWARE;

	iio->num_channels = 1;
	iio->channels = &stm32_dfsdm_ch;

	platform_set_drvdata(pdev, iio);

	return devm_iio_device_register(&pdev->dev, iio);
}

static const struct of_device_id sd_adc_of_match[] = {
	{ .compatible = "sd-modulator" },
	{ .compatible = "ads1201" },
	{ }
};
MODULE_DEVICE_TABLE(of, adc081c_of_match);

static struct platform_driver iio_sd_mod_adc = {
	.driver = {
		.name = "iio_sd_adc_mod",
		.of_match_table = of_match_ptr(sd_adc_of_match),
	},
	.probe = iio_sd_mod_probe,
};

module_platform_driver(iio_sd_mod_adc);

MODULE_DESCRIPTION("Basic sigma delta modulator");
MODULE_AUTHOR("Arnaud Pouliquen <arnaud.pouliquen@st.com>");
MODULE_LICENSE("GPL v2");
