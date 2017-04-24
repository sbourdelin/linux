/*
 * TI ADC1x8S102 SPI ADC driver
 *
 * Copyright (c) 2013-2015 Intel Corporation.
 * Copyright (c) 2017 Siemens AG
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
 * This IIO device driver is is designed to work with the following
 * analog to digital converters from Texas Instruments:
 *  ADC108S102
 *  ADC128S102
 * The communication with ADC chip is via the SPI bus (mode 3).
 */

#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/types.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/spi/spi.h>

#include <linux/platform_data/adc1x8s102.h>
#include <linux/regulator/consumer.h>

#include <linux/delay.h>
#include <linux/acpi.h>
#include <linux/property.h>
#include <linux/gpio.h>

#include <linux/spi/pxa2xx_spi.h>

/*
 * Defining the ADC resolution being 12 bits, we can use the same driver for
 * both ADC108S102 (10 bits resolution) and ADC128S102 (12 bits resolution)
 * chips. The ADC108S102 effectively returns a 12-bit result with the 2
 * least-significant bits unset.
 */
#define ADC1x8S102_BITS		12
#define ADC1x8S102_MAX_CHANNELS	8

/* 16-bit SPI command format:
 *   [15:14] Ignored
 *   [13:11] 3-bit channel address
 *   [10:0]  Ignored
 */
#define ADC1x8S102_CMD(ch)		(((ch) << (8)) << (3))

/*
 * 16-bit SPI response format:
 *   [15:12] Zeros
 *   [11:0]  12-bit ADC sample (for ADC108S102, [1:0] will always be 0).
 */
#define ADC1x8S102_RES_DATA(res)	(res & ((1 << ADC1x8S102_BITS) - 1))

#define ADC1x8S102_GALILEO2_CS	8

struct adc1x8s102_state {
	struct spi_device		*spi;
	struct regulator		*reg;
	u16				ext_vin;
	/* SPI transfer used by triggered buffer handler*/
	struct spi_transfer		ring_xfer;
	/* SPI transfer used by direct scan */
	struct spi_transfer		scan_single_xfer;
	/* SPI message used by ring_xfer SPI transfer */
	struct spi_message		ring_msg;
	/* SPI message used by scan_single_xfer SPI transfer */
	struct spi_message		scan_single_msg;

	/* SPI message buffers:
	 *  tx_buf: |C0|C1|C2|C3|C4|C5|C6|C7|XX|
	 *  rx_buf: |XX|R0|R1|R2|R3|R4|R5|R6|R7|tt|tt|tt|tt|
	 *
	 *  tx_buf: 8 channel read commands, plus 1 dummy command
	 *  rx_buf: 1 dummy response, 8 channel responses, plus 64-bit timestamp
	 */
	__be16				rx_buf[13] ____cacheline_aligned;
	__be16				tx_buf[9];
};

#define ADC1X8S102_V_CHAN(index)					\
	{								\
		.type = IIO_VOLTAGE,					\
		.indexed = 1,						\
		.channel = index,					\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
			BIT(IIO_CHAN_INFO_SCALE),			\
		.address = index,					\
		.scan_index = index,					\
		.scan_type = {						\
			.sign = 'u',					\
			.realbits = ADC1x8S102_BITS,			\
			.storagebits = 16,				\
			.endianness = IIO_BE,				\
		},							\
	}

static const struct iio_chan_spec adc1x8s102_channels[] = {
	ADC1X8S102_V_CHAN(0),
	ADC1X8S102_V_CHAN(1),
	ADC1X8S102_V_CHAN(2),
	ADC1X8S102_V_CHAN(3),
	ADC1X8S102_V_CHAN(4),
	ADC1X8S102_V_CHAN(5),
	ADC1X8S102_V_CHAN(6),
	ADC1X8S102_V_CHAN(7),
	IIO_CHAN_SOFT_TIMESTAMP(8),
};

static int adc1x8s102_update_scan_mode(struct iio_dev *indio_dev,
		unsigned long const *active_scan_mask)
{
	struct adc1x8s102_state *st;
	int i, j;

	st = iio_priv(indio_dev);

	/* Fill in the first x shorts of tx_buf with the number of channels
	 * enabled for sampling by the triggered buffer
	 */
	for (i = 0, j = 0; i < ADC1x8S102_MAX_CHANNELS; i++) {
		if (test_bit(i, active_scan_mask)) {
			st->tx_buf[j] = cpu_to_be16(ADC1x8S102_CMD(i));
			j++;
		}
	}
	/* One dummy command added, to clock in the last response */
	st->tx_buf[j] = 0x00;

	/* build SPI ring message */
	st->ring_xfer.tx_buf = &st->tx_buf[0];
	st->ring_xfer.rx_buf = &st->rx_buf[0];
	st->ring_xfer.len = (j + 1) * sizeof(__be16);

	spi_message_init(&st->ring_msg);
	spi_message_add_tail(&st->ring_xfer, &st->ring_msg);

	return 0;
}

static irqreturn_t adc1x8s102_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev;
	struct adc1x8s102_state *st;
	s64 time_ns = 0;
	int b_sent;

	indio_dev = pf->indio_dev;
	st = iio_priv(indio_dev);

	b_sent = spi_sync(st->spi, &st->ring_msg);
	if (b_sent == 0) {
		if (indio_dev->scan_timestamp) {
			time_ns = iio_get_time_ns(indio_dev);
			memcpy((u8 *)st->rx_buf + st->ring_xfer.len, &time_ns,
			       sizeof(time_ns));
		}

		/* Skip the dummy response in the first slot */
		iio_push_to_buffers(indio_dev, (u8 *)&st->rx_buf[1]);
	}

	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static int adc1x8s102_scan_direct(struct adc1x8s102_state *st, unsigned int ch)
{
	int ret;

	st->tx_buf[0] = cpu_to_be16(ADC1x8S102_CMD(ch));
	ret = spi_sync(st->spi, &st->scan_single_msg);
	if (ret)
		return ret;

	/* Skip the dummy response in the first slot */
	return be16_to_cpu(st->rx_buf[1]);
}

static int adc1x8s102_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val,
			   int *val2,
			   long m)
{
	int ret;
	struct adc1x8s102_state *st;

	st = iio_priv(indio_dev);

	switch (m) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&indio_dev->mlock);
		if (indio_dev->currentmode == INDIO_BUFFER_TRIGGERED) {
			ret = -EBUSY;
			dev_warn(&st->spi->dev,
			 "indio_dev->currentmode is INDIO_BUFFER_TRIGGERED\n");
		} else {
			ret = adc1x8s102_scan_direct(st, chan->address);
		}
		mutex_unlock(&indio_dev->mlock);

		if (ret < 0)
			return ret;
		*val = ADC1x8S102_RES_DATA(ret);

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_VOLTAGE:
			if (st->reg)
				*val = regulator_get_voltage(st->reg) / 1000;
			else
				*val = st->ext_vin;

			*val2 = chan->scan_type.realbits;
			return IIO_VAL_FRACTIONAL_LOG2;
		default:
			dev_warn(&st->spi->dev,
				 "Invalid channel type %u for channel %d\n",
				 chan->type, chan->channel);
			return -EINVAL;
		}
	default:
		dev_warn(&st->spi->dev, "Invalid IIO_CHAN_INFO: %lu\n", m);
		return -EINVAL;
	}
}

static const struct iio_info adc1x8s102_info = {
	.read_raw		= &adc1x8s102_read_raw,
	.update_scan_mode	= &adc1x8s102_update_scan_mode,
	.driver_module		= THIS_MODULE,
};

#ifdef CONFIG_ACPI
typedef int (*acpi_setup_handler)(struct spi_device *,
				  const struct adc1x8s102_platform_data **);

static const struct adc1x8s102_platform_data int3495_platform_data = {
	.ext_vin = 5000,	/* 5 V */
};

/* Galileo Gen 2 SPI setup */
static int
adc1x8s102_setup_int3495(struct spi_device *spi,
			 const struct adc1x8s102_platform_data **pdata)
{
	struct pxa2xx_spi_chip *chip_data;

	chip_data = devm_kzalloc(&spi->dev, sizeof(*chip_data), GFP_KERNEL);
	if (!chip_data)
		return -ENOMEM;

	chip_data->gpio_cs = ADC1x8S102_GALILEO2_CS;
	spi->controller_data = chip_data;
	dev_info(&spi->dev, "setting GPIO CS value to %d\n",
		 chip_data->gpio_cs);
	spi_setup(spi);

	*pdata = &int3495_platform_data;

	return 0;
}

static const struct acpi_device_id adc1x8s102_acpi_ids[] = {
	{ "INT3495",  (kernel_ulong_t)&adc1x8s102_setup_int3495 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, adc1x8s102_acpi_ids);
#endif

static int adc1x8s102_probe(struct spi_device *spi)
{
	const struct adc1x8s102_platform_data *pdata = spi->dev.platform_data;
	struct adc1x8s102_state *st;
	struct iio_dev *indio_dev;
	int ret;

#ifdef CONFIG_ACPI
	if (ACPI_COMPANION(&spi->dev)) {
		acpi_setup_handler setup_handler;
		const struct acpi_device_id *id;

		id = acpi_match_device(adc1x8s102_acpi_ids, &spi->dev);
		if (!id)
			return -ENODEV;

		setup_handler = (acpi_setup_handler)id->driver_data;
		if (setup_handler) {
			ret = setup_handler(spi, &pdata);
			if (ret)
				return ret;
		}
	}
#endif

	if (!pdata) {
		dev_err(&spi->dev, "Cannot get adc1x8s102 platform data\n");
		return -ENODEV;
	}

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->ext_vin = pdata->ext_vin;

	/* Use regulator, if available. */
	st->reg = devm_regulator_get(&spi->dev, "vref");
	if (IS_ERR(st->reg)) {
		dev_err(&spi->dev, "Cannot get 'vref' regulator\n");
		return PTR_ERR(st->reg);
	}
	ret = regulator_enable(st->reg);
	if (ret < 0) {
		dev_err(&spi->dev, "Cannot enable vref regulator\n");
		return ret;
	}

	spi_set_drvdata(spi, indio_dev);
	st->spi = spi;

	indio_dev->name = spi->modalias;
	indio_dev->dev.parent = &spi->dev;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = adc1x8s102_channels;
	indio_dev->num_channels = ARRAY_SIZE(adc1x8s102_channels);
	indio_dev->info = &adc1x8s102_info;

	/* Setup default message */
	st->scan_single_xfer.tx_buf = st->tx_buf;
	st->scan_single_xfer.rx_buf = st->rx_buf;
	st->scan_single_xfer.len = 2 * sizeof(__be16);
	st->scan_single_xfer.cs_change = 0;

	spi_message_init(&st->scan_single_msg);
	spi_message_add_tail(&st->scan_single_xfer, &st->scan_single_msg);

	ret = iio_triggered_buffer_setup(indio_dev, NULL,
			&adc1x8s102_trigger_handler, NULL);
	if (ret)
		goto error_disable_reg;

	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(&spi->dev,
			"Failed to register IIO device\n");
		goto error_cleanup_ring;
	}
	return 0;

error_cleanup_ring:
	iio_triggered_buffer_cleanup(indio_dev);
error_disable_reg:
	regulator_disable(st->reg);

	return ret;
}

static int adc1x8s102_remove(struct spi_device *spi)
{
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct adc1x8s102_state *st = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	iio_triggered_buffer_cleanup(indio_dev);

	regulator_disable(st->reg);

	return 0;
}

static const struct spi_device_id adc1x8s102_id[] = {
	{ "adc1x8s102", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, adc1x8s102_id);

static struct spi_driver adc1x8s102_driver = {
	.driver = {
		.name   = "adc1x8s102",
		.owner	= THIS_MODULE,
		.acpi_match_table = ACPI_PTR(adc1x8s102_acpi_ids),
	},
	.probe		= adc1x8s102_probe,
	.remove		= adc1x8s102_remove,
	.id_table	= adc1x8s102_id,
};
module_spi_driver(adc1x8s102_driver);

MODULE_AUTHOR("Bogdan Pricop <bogdan.pricop@emutex.com>");
MODULE_DESCRIPTION("Texas Instruments ADC1x8S102 driver");
MODULE_LICENSE("GPL v2");
