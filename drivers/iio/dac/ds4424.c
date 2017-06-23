/*
 * Maxim Integrated
 * 7-bit, Multi-Channel Sink/Source Current DAC Driver
 * Copyright (C) 2017 Maxim Integrated
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/iio/driver.h>
#include <linux/iio/machine.h>
#include <linux/iio/dac/ds4424.h>

#define DS4424_DAC_ADDR(chan)   ((chan) + 0xf8)
#define SOURCE_I	1
#define SINK_I		0

#define PWR_ON		true
#define PWR_OFF		false

#define DS4424_CHANNEL(chan) { \
	.type = IIO_CURRENT, \
	.indexed = 1, \
	.output = 1, \
	.channel = chan, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
			BIT(IIO_CHAN_INFO_PROCESSED) | \
			BIT(IIO_CHAN_INFO_SCALE),\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_OFFSET), \
	.address = DS4424_DAC_ADDR(chan),	\
	.scan_type = { \
		.sign = 'u', \
		.realbits = 8, \
		.storagebits = 8, \
		.shift = 0, \
		}, \
}

union raw_data {
	struct {
		u8 dx:7;
		u8 source_bit:1;  /* 1 is source, 0 is sink */
	};
	u8 bits;
};

enum ds4424_device_ids {
	ID_DS4422,
	ID_DS4424,
};

struct ds4424_data {
	struct i2c_client *client;
	struct mutex lock;
	uint16_t raw[DS442X_MAX_DAC_CHANNELS];
#ifdef CONFIG_PM_SLEEP
	uint16_t save[DS442X_MAX_DAC_CHANNELS];
#endif
	uint32_t max_rfs;
	uint32_t min_rfs;
	uint32_t ifs_scale;
	uint32_t max_picoamp;
	uint32_t rfs_res[DS442X_MAX_DAC_CHANNELS];
	struct iio_map dac_iio_map[DS442X_MAX_DAC_CHANNELS + 1];
	struct regulator *vcc_reg;
	const char *vcc_reg_name;
	bool regulator_state;
};

static const struct ds4424_pdata ds4424_pdata_default = {
	/* .vcc_supply_name = "dac_vdd_3v3", */
	.min_rfs = 400,
	.max_rfs = 1600,
	.ifs_scale = 61000, /* 61000*100 = 6100000 = 100,000,000 * .976/16 */
	.max_picoamp = 200000000,
	.rfs_res = {400, 800, 1000, 1600},
	.dac_iio_map = {
		{	.consumer_dev_name = "ds4424_dac-consumer-dev_name-1",
			.consumer_channel = "ds4424_dac1",
			.adc_channel_label = "OUT1"
		},
		{
			.consumer_dev_name = "ds4424_dac-consumer-dev_name-2",
			.consumer_channel = "ds4424_dac2",
			.adc_channel_label = "OUT2"
		},
		{
			.consumer_dev_name = "ds4424_dac-consumer-dev_name-3",
			.consumer_channel = "ds4424_dac3",
			.adc_channel_label = "OUT3"
		},
		{
			.consumer_dev_name = "ds4424_dac-consumer-dev_name-4",
			.consumer_channel = "ds4424_dac4",
			.adc_channel_label = "OUT4"
		},
		{},
	},
};

static const struct iio_chan_spec ds4424_channels[] = {
	DS4424_CHANNEL(0),
	DS4424_CHANNEL(1),
	DS4424_CHANNEL(2),
	DS4424_CHANNEL(3)
};

int ds4424_regulator_onoff(struct iio_dev *indio_dev, bool enable)
{
	struct ds4424_data *data = iio_priv(indio_dev);
	int ret = 0;

	if (data->vcc_reg == NULL)
		return ret;

	if (data->regulator_state == PWR_OFF && enable == PWR_ON) {
		ret = regulator_enable(data->vcc_reg);
		if (ret) {
			pr_err("%s - enable vcc_reg failed, ret=%d\n",
				__func__, ret);
			goto done;
		}
	} else if (data->regulator_state == PWR_ON && enable == PWR_OFF) {
		ret = regulator_disable(data->vcc_reg);
		if (ret) {
			pr_err("%s - disable vcc_reg failed, ret=%d\n",
				__func__, ret);
			goto done;
		}
	}

	data->regulator_state = enable;
done:
	return ret;
}

static int ds4424_get_value(struct iio_dev *indio_dev,
			     int *val, int channel)
{
	struct ds4424_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;
	u8 outbuf[1];
	u8 inbuf[1];
	int ret;

	if ((channel < 0) && (channel >= indio_dev->num_channels))
		return -EINVAL;

	outbuf[0] = DS4424_DAC_ADDR(channel);
	mutex_lock(&data->lock);
	ret = i2c_master_send(client, outbuf, 1);
	if (ret < 0) {
		mutex_unlock(&data->lock);
		return ret;
	} else if (ret != 1) {
		mutex_unlock(&data->lock);
		return -EIO;
	}

	ret = i2c_master_recv(client, inbuf, 1);
	if (ret < 0) {
		mutex_unlock(&data->lock);
		return ret;
	} else if (ret != 1) {
		mutex_unlock(&data->lock);
		return -EIO;
	}

	mutex_unlock(&data->lock);

	*val = inbuf[0];
	return 0;
}

/*
 * DS4432 DAC control register 8 bits
 * [7]		0: to sink; 1: to source
 * [6:0]	steps to sink/source
 * bit[7] looks like a sign bit, but the value of the register is
 * not a complemental code considering the bit[6:0] is a absolute
 * distance from the zero point.
 */

/*
 * val is positive if sourcing
 *  val is negative if sinking
 *  val can be -127 to 127
 */
static int ds4424_set_value(struct iio_dev *indio_dev,
			     int val, struct iio_chan_spec const *chan)
{
	struct ds4424_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;
	u8 outbuf[2];
	int ret;
	int max_val = ((1 << chan->scan_type.realbits) - 1);

	if (val < 0 || val > max_val)
		return -EINVAL;

	if ((chan->channel < 0)
		&& (chan->channel >= indio_dev->num_channels))
		return -EINVAL;

	outbuf[0] = DS4424_DAC_ADDR(chan->channel);
	outbuf[1] = (val & 0xff);

	mutex_lock(&data->lock);
	ret = i2c_master_send(client, outbuf, ARRAY_SIZE(outbuf));
	mutex_unlock(&data->lock);

	if (ret < 0)
		return ret;
	else if (ret >= 0 && ret != ARRAY_SIZE(outbuf))
		return -EIO;

	data->raw[chan->channel] = outbuf[1];
	return 0;
}

static int ds4424_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	union raw_data raw;
	int round_up, ret;
	struct ds4424_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		/* Raw is processed a little bit
		 * outputs positive values for sourcing
		 * and negative values for sinking
		 */
		ret = ds4424_get_value(indio_dev, val, chan->channel);
		if (ret < 0) {
			pr_err("%s : ds4424_get_value returned %d\n",
							__func__, ret);
			return ret;
		}
		raw.bits = *val;
		*val = raw.dx;
		if (raw.source_bit == SINK_I)
			*val = -*val;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_PROCESSED:
		/**
		 * To get the processed current using the 8-bit raw data:
		 * bit 7 is a 1 if sourcing current and it's a 0 if sinking
		 * current.
		 * The current full scale (Ifs) depends on the Rfs resistor
		 * value in ohms:
		 * Ifs = (0.976/Rfs)*(127/16)
		 * Then the current sourced or sinked can be determined as
		 * follows:
		 * I = Ifs * (Dx/127)
		 * where Dx is the value of the seven bits 6 to 0.
		 */
		if (data->rfs_res[chan->channel] < data->min_rfs ||
				data->rfs_res[chan->channel] > data->max_rfs) {
			pr_err("%s : rfs_res out of range. rfs_res[%d]: %d\n",
					__func__,
					chan->channel,
					data->rfs_res[chan->channel]);
			return -EINVAL;
		}

		ret = ds4424_get_value(indio_dev, val, chan->channel);
		if (ret < 0) {
			pr_err("%s : ds4424_get_value returned %d\n",
					__func__, ret);
			return ret;
		}
		raw.bits = *val;
		*val = data->ifs_scale * raw.dx * 100;
		round_up = data->rfs_res[chan->channel] / 2;
		*val = (*val + round_up) / data->rfs_res[chan->channel];

		if (raw.source_bit == SINK_I)
			*val = -*val;
		*val = *val * 100;	/* picoAmps */
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		round_up = data->rfs_res[chan->channel] / 2;
		/* picoAmps */
		*val = (data->ifs_scale * 10000 + round_up) /
			data->rfs_res[chan->channel];
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_OFFSET:
		*val = 0;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

/**
 * val is positive if sourcing
 * val is negative if sinking
 */
static int ds4424_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	union raw_data raw;
	struct ds4424_data *data = iio_priv(indio_dev);
	int val0, max_val, min_val, tmp_scale;

	if (val2 != 0)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:

		max_val = ((1 << chan->scan_type.realbits)/2) - 1;
		min_val = -max_val;
		if ((val > max_val) || (val < min_val))
			return -EINVAL;

		if (val > 0) {
			raw.source_bit = SOURCE_I;
			raw.dx = val;
		} else {
			raw.source_bit = SINK_I;
			raw.dx = -val;
		}

		return ds4424_set_value(indio_dev, raw.bits, chan);

	case IIO_CHAN_INFO_PROCESSED:  /* val input is picoAmps */
		/*   val can be 0 to 200,000,000 (200 picoAmps)  */
		val0 = val;
		raw.source_bit = SOURCE_I;
		if (val < 0) {
			raw.source_bit = SINK_I;
			val = -val;
		}
		if (val > data->max_picoamp) {
			pr_err("%s : Requested current %d ", __func__, val);
			pr_err("exceeds %d picoAmps\n",	data->max_picoamp);
			return -EINVAL;
		}
		if (data->rfs_res[chan->channel] < data->min_rfs ||
				data->rfs_res[chan->channel] > data->max_rfs) {
			pr_info("%s : Resistor values out of range\n",
				__func__);
			return -EINVAL;
		}
		val = val / 1000;
		tmp_scale = data->ifs_scale / 10;  /* preserve resolution */
		val = (val * data->rfs_res[chan->channel]) /
			tmp_scale;
		val = (val + 50) / 100;
		val2 = ((1 << chan->scan_type.realbits) / 2) - 1;
		if (val > val2) {
			pr_info("%s : Requested current %d %d",
				__func__, val0, val);
			pr_info("exceeds maximum. DAC set to maximum %d\n",
				val2);
			val = val2;
		}
		raw.dx = val;
		return ds4424_set_value(indio_dev, raw.bits, chan);

	default:
		return -EINVAL;
	}
}

static int ds4424_verify_chip(struct iio_dev *indio_dev)
{
	int ret = 0, val;
	int i;

	usleep_range(1000, 1200);
	for (i = 0; i < indio_dev->num_channels; i++) {
		ret = ds4424_get_value(indio_dev, &val, i);
		if (ret < 0) {
			pr_err("%s : read %d, should be 0\n", __func__, ret);
			break;
		}
	}
	return ret;
}

#ifdef CONFIG_PM_SLEEP
static int ds4424_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ds4424_data *data = iio_priv(indio_dev);
	int ret = 0;
	u32 i;

	for (i = 0; i < indio_dev->num_channels; i++) {
		data->save[i] = data->raw[i];
		ret = ds4424_set_value(indio_dev, 0,
				&(indio_dev->channels[i]));
		if (ret < 0)
			return ret;
	}
	return ret;
}

static int ds4424_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ds4424_data *data = iio_priv(indio_dev);
	int ret = 0;
	u32 i;

	for (i = 0; i < indio_dev->num_channels; i++) {
		ret = ds4424_set_value(indio_dev, data->save[i],
				&(indio_dev->channels[i]));
		if (ret < 0)
			return ret;
	}
	return ret;
}

static SIMPLE_DEV_PM_OPS(ds4424_pm_ops, ds4424_suspend, ds4424_resume);
#define DS4424_PM_OPS (&ds4424_pm_ops)
#else
#define DS4424_PM_OPS NULL
#endif /* CONFIG_PM_SLEEP */

static const struct iio_info ds4424_info = {
	.read_raw = ds4424_read_raw,
	.write_raw = ds4424_write_raw,
	.driver_module = THIS_MODULE,
};

#ifdef CONFIG_OF
static int ds4424_parse_dt(struct iio_dev *indio_dev)
{
	int ret;
	int len;
	int num_ch;
	int i;
	int count;
	struct property *prop;
	struct ds4424_data *data = iio_priv(indio_dev);
	struct device_node *node = indio_dev->dev.parent->of_node;

	if (!node) {
		pr_info("%s:%d ds4424 dts not found\n", __func__, __LINE__);
		return -ENODEV;
	}

	prop = of_find_property(node, "rfs-resistors", &len);
	if (!prop) {
		pr_err("Invalid rfs-resistor in dt. len: %d\n", len);
		return -EINVAL;
	}

	if (len != (DS442X_MAX_DAC_CHANNELS * sizeof(uint32_t))) {
		pr_err("Invalid rfs-resistor length in dt. len: %d\n", len);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "rfs-resistors",
				 data->rfs_res, DS442X_MAX_DAC_CHANNELS);
	if (ret < 0) {
		pr_err("Reading rfs-resistors from dt failed. ret: %d\n", ret);
		return ret;
	}

	pr_info("ds4424 rfs-resistors: %d, %d, %d, %d\n",
			data->rfs_res[0], data->rfs_res[1],
			data->rfs_res[2], data->rfs_res[3]);

	ret = of_property_read_u32(node, "max-rfs",
				   &data->max_rfs);
	if (ret < 0) {
		pr_err("Reading max-rfs from dt failed. ret: %d\n", ret);
		return ret;
	}

	ret = of_property_read_u32(node, "min-rfs",
				  (u32 *)&data->min_rfs);
	if (ret < 0) {
		pr_err("Reading min-rfs from dt failed. ret: %d\n", ret);
		return ret;
	}

	pr_info("ds4424 max-rfs: %d, min-rfs: %d\n",
			data->max_rfs, data->min_rfs);

	ret = of_property_read_u32(node, "max-picoamp",
				  (u32 *)&data->max_picoamp);
	if (ret < 0) {
		pr_err("Reading max-picoamp from dt failed. ret: %d\n", ret);
		return ret;
	}

	ret = of_property_read_u32(node, "ifs-scale",
				  (u32 *)&data->ifs_scale);
	if (ret < 0) {
		pr_err("Reading ifs-scale from dt failed. ret: %d\n", ret);
		return ret;
	}

	pr_info("ds4424 max-picoamp: %d, ifs-scale: %d\n",
			data->max_picoamp, data->ifs_scale);

	count = of_property_count_strings(node, "dac-iio-map");
	if (count < 0) {
		pr_info("dac-iio-map not found in dts\n");
		return count;
	}

	ret = of_property_read_string(node, "vcc-supply", &data->vcc_reg_name);
	if (ret < 0) {
		pr_info("DAC vcc-supply is not available in dts\n");
		data->vcc_reg_name = NULL;
	}

	if (count != DS4424_MAX_DAC_CHANNELS * 3 &&
		count != DS4424_MAX_DAC_CHANNELS * 3) {
		pr_info("Incorrect dac-iio-map in dts. count: %d\n", count);
		return -EINVAL;
	}

	num_ch = count / 3;
	for (i = 0; i < num_ch; i++) {
		ret = of_property_read_string_index(node,
				"dac-iio-map", i * 3,
				&data->dac_iio_map[i].consumer_dev_name);
		if (ret < 0) {
			pr_info("%s:%d\n", __func__, __LINE__);
			return ret;
		}

		ret = of_property_read_string_index(node, "dac-iio-map",
			i * 3 + 1,
			&data->dac_iio_map[i].consumer_channel);
		if (ret < 0) {
			pr_info("%s:%d\n", __func__, __LINE__);
			return ret;
		}

		ret = of_property_read_string_index(node, "dac-iio-map",
				i * 3 + 2,
				&data->dac_iio_map[i].adc_channel_label);
		if (ret < 0) {
			pr_info("%s:%d\n", __func__, __LINE__);
			return ret;
		}

		pr_info("ds4424 iio-map[%d]: %s, %s, %s\n", i,
				data->dac_iio_map[i].consumer_dev_name,
				data->dac_iio_map[i].consumer_channel,
				data->dac_iio_map[i].adc_channel_label);
	}

	return 0;
}
#else
static int ds4424_parse_dt(struct iio_dev *indio_dev)
{
	return -ENODEV;
}
#endif

static int ds4424_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	const struct ds4424_pdata *pdata;
	struct ds4424_data *data;
	struct iio_dev *indio_dev;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "I2C is not supported\n");
		return -ENODEV;
	}

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev) {
		pr_err("%s:%d\n", __func__, __LINE__);
		return -ENOMEM;
	}

	data = iio_priv(indio_dev);
	memset(data, 0, sizeof(*data));
	i2c_set_clientdata(client, indio_dev);
	data->client = client;
	indio_dev->name = id->name;
	indio_dev->dev.parent = &client->dev;

	ret = ds4424_verify_chip(indio_dev);
	if (ret < 0) {
		dev_err(&client->dev, "%s failed. ret:%d\n", __func__, ret);
		return -ENXIO;
	}

	if (client->dev.of_node) {
		ret = ds4424_parse_dt(indio_dev);
		if (ret < 0) {
			dev_err(&client->dev,
					"%s - of_node error\n", __func__);
			ret = -EINVAL;
		}
	} else {
		pdata =  client->dev.platform_data;
		if (!pdata) {
			dev_err(&client->dev,
				"dts/platform data not found.\n");
			/* Use default driver settings */
			pdata = &ds4424_pdata_default;
		}

		pdata = client->dev.platform_data;
		data->min_rfs = pdata->min_rfs;
		data->max_rfs = pdata->max_rfs;
		data->ifs_scale = pdata->ifs_scale;
		data->max_picoamp = pdata->max_picoamp;
		data->vcc_reg_name = pdata->vcc_supply_name;
		memcpy(data->rfs_res, pdata->rfs_res,
			sizeof(uint32_t) * DS442X_MAX_DAC_CHANNELS);
		memcpy(data->dac_iio_map, pdata->dac_iio_map,
			sizeof(struct iio_map) * DS442X_MAX_DAC_CHANNELS);
	}

	if (data->vcc_reg_name) {
		data->vcc_reg = devm_regulator_get(&client->dev,
			data->vcc_reg_name);
		if (IS_ERR(data->vcc_reg)) {
			ret = PTR_ERR(data->vcc_reg);
			dev_err(&client->dev,
				"Failed to get vcc_reg regulator: %d\n", ret);
			return ret;
		}
	}

	mutex_init(&data->lock);
	ret = ds4424_regulator_onoff(indio_dev, PWR_ON);
	if (ret < 0) {
		pr_err("Unable to turn on the regulator. %s:%d, ret: %d\n",
			__func__, __LINE__, ret);
		return ret;
	}

	switch (id->driver_data) {
	case ID_DS4422:
		indio_dev->num_channels = DS4422_MAX_DAC_CHANNELS;
		break;
	case ID_DS4424:
		indio_dev->num_channels = DS4424_MAX_DAC_CHANNELS;
		break;
	default:
		indio_dev->num_channels = DS4424_MAX_DAC_CHANNELS;
		break;
	}

	indio_dev->channels = ds4424_channels;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ds4424_info;

	ret = iio_map_array_register(indio_dev, data->dac_iio_map);
	if (ret < 0)
		goto err_iio_device_0;

	ret = iio_device_register(indio_dev);
	if (ret < 0) {
		pr_err("iio_device_register failed . %s:%d, ret: %d\n",
			__func__, __LINE__, ret);
		goto err_iio_device_1;
	}

	return ret;

err_iio_device_0:
	ds4424_regulator_onoff(indio_dev, PWR_OFF);
err_iio_device_1:
	iio_map_array_unregister(indio_dev);
	return ret;
}

static int ds4424_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);

	iio_device_unregister(indio_dev);
	iio_map_array_unregister(indio_dev);
	ds4424_regulator_onoff(indio_dev, PWR_OFF);
	return 0;
}

static const struct i2c_device_id ds4424_id[] = {
	{ "ds4422", ID_DS4422 },
	{ "ds4424", ID_DS4424 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ds4424_id);

static const struct of_device_id ds4424_of_match[] = {
	{ .compatible = "maxim,ds4422" },
	{ .compatible = "maxim,ds4424" },
	{ }
};

MODULE_DEVICE_TABLE(of, ds4424_of_match);

static struct i2c_driver ds4424_driver = {
	.driver = {
		.name	= "ds4424",
		.pm     = DS4424_PM_OPS,
	},
	.probe		= ds4424_probe,
	.remove		= ds4424_remove,
	.id_table	= ds4424_id,
};
module_i2c_driver(ds4424_driver);

MODULE_DESCRIPTION("Maxim DS4424 DAC Driver");
MODULE_AUTHOR("Ismail H. Kose <ismail.kose@maximintegrated.com>");
MODULE_AUTHOR("Vishal Sood <vishal.sood@maximintegrated.com>");
MODULE_AUTHOR("David Jung <david.jung@maximintegrated.com>");
MODULE_LICENSE("GPL v2");
