/*
 * ADS1015 - Texas Instruments Analog-to-Digital Converter
 *
 * Copyright (c) 2016, Intel Corporation.
 *
 * This file is subject to the terms and conditions of version 2 of
 * the GNU General Public License.  See the file COPYING in the main
 * directory of this archive for more details.
 *
 * IIO driver for ADS1015 ADC 7-bit I2C slave address:
 *	* 0x48 - ADDR connectd to Ground
 *	* 0x49 - ADDR connected to Vdd
 *	* 0x4A - ADDR connected to SDA
 *	* 0x4B - ADDR connected to SCL
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/pm_runtime.h>
#include <linux/iio/types.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

#define ADS1015_DRV_NAME "ads1015"

#define ADS1015_CONV_REG	0x00
#define ADS1015_CONFIG_REG	0x01

#define ADS1015_CONFIG_DR	GENMASK(7, 5)
#define ADS1015_CONFIG_MODE	BIT(8)
#define ADS1015_CONFIG_SCALE	GENMASK(11, 9)
#define ADS1015_CONFIG_MUX	GENMASK(14, 12)

#define ADS1015_SLEEP_DELAY_MS	2000

enum ads1015_channels {
	ADS1015_AIN0_AIN1 = 0,
	ADS1015_AIN0_AIN3,
	ADS1015_AIN1_AIN3,
	ADS1015_AIN2_AIN3,
	ADS1015_AIN0,
	ADS1015_AIN1,
	ADS1015_AIN2,
	ADS1015_AIN3,
};

static const unsigned int ads1015_data_rate[] = {
	128, 250, 490, 920, 1600, 2400, 3300, 3300
};

static const struct {
	int scale;
	int uscale;
} ads1015_scale[] = {
	{3, 0},
	{2, 0},
	{1, 0},
	{0, 500000},
	{0, 250000},
	{0, 125000},
	{0, 125000},
	{0, 125000},
};

#define ADS1015_V_CHAN(_chan, _addr) {				\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.address = _addr,					\
	.channel = _chan,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SCALE) |	\
				BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
	.scan_index = _addr,					\
	.scan_type = {						\
		.sign = 's',					\
		.realbits = 12,					\
		.storagebits = 16,				\
		.shift = 4,					\
		.endianness = IIO_CPU,				\
	},							\
}

#define ADS1015_V_DIFF_CHAN(_chan, _chan2, _addr) {		\
	.type = IIO_VOLTAGE,					\
	.differential = 1,					\
	.indexed = 1,						\
	.address = _addr,					\
	.channel = _chan,					\
	.channel2 = _chan2,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SCALE) |	\
				BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
	.scan_index = _addr,					\
	.scan_type = {						\
		.sign = 's',					\
		.realbits = 12,					\
		.storagebits = 16,				\
		.shift = 4,					\
		.endianness = IIO_CPU,				\
	},							\
}

struct ads1015_data {
	struct i2c_client *client;
	struct regmap *regmap;
	s16 buffer[8];
	int64_t timestamp;
};

static bool ads1015_is_writeable_reg(struct device *dev, unsigned int reg)
{
	return (reg == ADS1015_CONFIG_REG);
}

static const struct regmap_config ads1015_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = ADS1015_CONFIG_REG,
	.writeable_reg = ads1015_is_writeable_reg,
};

static const struct iio_chan_spec ads1015_channels[] = {
	ADS1015_V_DIFF_CHAN(0, 1, ADS1015_AIN0_AIN1),
	ADS1015_V_DIFF_CHAN(0, 3, ADS1015_AIN0_AIN3),
	ADS1015_V_DIFF_CHAN(1, 3, ADS1015_AIN1_AIN3),
	ADS1015_V_DIFF_CHAN(2, 3, ADS1015_AIN2_AIN3),
	ADS1015_V_CHAN(0, ADS1015_AIN0),
	ADS1015_V_CHAN(1, ADS1015_AIN1),
	ADS1015_V_CHAN(2, ADS1015_AIN2),
	ADS1015_V_CHAN(3, ADS1015_AIN3),
};

static int ads1015_set_power_state(struct ads1015_data *data, bool on)
{
	int ret;

	if (on) {
		ret = pm_runtime_get_sync(&data->client->dev);
		if (ret < 0)
			pm_runtime_put_noidle(&data->client->dev);
	} else {
		pm_runtime_mark_last_busy(&data->client->dev);
		ret = pm_runtime_put_autosuspend(&data->client->dev);
	}

	return ret;
}

static
int ads1015_get_adc_result(struct ads1015_data *data, int chan, int *val)
{
	int ret;

	ret = regmap_update_bits(data->regmap, ADS1015_CONFIG_REG,
				 ADS1015_CONFIG_MUX, chan << 12);
	if (ret < 0)
		return ret;

	ret = regmap_read(data->regmap, ADS1015_CONV_REG, val);
	if (ret < 0)
		return ret;

	return 0;
}

static irqreturn_t ads1015_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ads1015_data *data = iio_priv(indio_dev);
	s16 res;
	int chan, ret;

	chan = find_first_bit(indio_dev->active_scan_mask,
			      indio_dev->masklength);

	ret = ads1015_get_adc_result(data, chan, (int *)&res);
	if (ret < 0)
		goto err;

	data->buffer[0] = res;

	iio_push_to_buffers_with_timestamp(indio_dev, data->buffer,
					   data->timestamp);
err:
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

static int ads1015_set_scale(struct ads1015_data *data, int scale,
				 int uscale)
{
	int i, rindex = -1;

	for (i = 0; i < ARRAY_SIZE(ads1015_scale); i++)
		if (ads1015_scale[i].scale == scale &&
			ads1015_scale[i].uscale == uscale) {
			rindex = i;
			break;
		}
	if (rindex < 0)
		return -EINVAL;

	return regmap_update_bits(data->regmap, ADS1015_CONFIG_REG,
				  ADS1015_CONFIG_SCALE, rindex << 9);
}

static int ads1015_set_data_rate(struct ads1015_data *data, int rate)
{
	int i, rindex = -1;

	for (i = 0; i < ARRAY_SIZE(ads1015_data_rate); i++)
		if (ads1015_data_rate[i] == rate) {
			rindex = i;
			break;
		}
	if (rindex < 0)
		return -EINVAL;

	return regmap_update_bits(data->regmap, ADS1015_CONFIG_REG,
				  ADS1015_CONFIG_DR, rindex << 5);
}

static int ads1015_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int *val,
			    int *val2, long mask)
{
	int ret, idx;
	struct ads1015_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = ads1015_set_power_state(data, true);
		if (ret < 0)
			return ret;
		ret = ads1015_get_adc_result(data, chan->address, val);
		if (ret < 0) {
			ads1015_set_power_state(data, false);
			return ret;
		}

		/* 12 bit res, D0 is bit 4 in conversion register */
		*val = sign_extend32(*val >> 4, 11);

		ret = ads1015_set_power_state(data, false);
		if (ret < 0)
			return ret;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		ret = regmap_read(data->regmap, ADS1015_CONFIG_REG, val);
		if (ret < 0)
			return ret;
		idx = (*val >> 9) & 0x0007;
		*val = ads1015_scale[idx].scale;
		*val2 = ads1015_scale[idx].uscale;

		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = regmap_read(data->regmap, ADS1015_CONFIG_REG, val);
		if (ret < 0)
			return ret;

		idx = (*val >> 5) & 0x0007;
		*val = ads1015_data_rate[idx];

		return IIO_VAL_INT;
	default:
		break;
	}

	return -EINVAL;
}

static int ads1015_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, int val,
			     int val2, long mask)
{
	struct ads1015_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		return ads1015_set_scale(data, val, val2);
	case IIO_CHAN_INFO_SAMP_FREQ:
		return ads1015_set_data_rate(data, val);
	default:
		break;
	}

	return -EINVAL;
}

static int ads1015_buffer_preenable(struct iio_dev *indio_dev)
{
	struct ads1015_data *data = iio_priv(indio_dev);

	return ads1015_set_power_state(data, true);
}

static int ads1015_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct ads1015_data *data = iio_priv(indio_dev);

	return ads1015_set_power_state(data, false);
}
static const struct iio_buffer_setup_ops ads1015_buffer_setup_ops = {
	.preenable	= ads1015_buffer_preenable,
	.postenable	= iio_triggered_buffer_postenable,
	.postdisable	= ads1015_buffer_postdisable,
	.predisable	= iio_triggered_buffer_predisable,
	.validate_scan_mask = &iio_validate_scan_mask_onehot,
};

static IIO_CONST_ATTR(scale_available, "3 2 1 0.5 0.25 0.125");
static IIO_CONST_ATTR(sampling_frequency_available,
		      "128 250 490 920 1600 2400 3300");

static struct attribute *ads1015_attributes[] = {
	&iio_const_attr_scale_available.dev_attr.attr,
	&iio_const_attr_sampling_frequency_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group ads1015_attribute_group = {
	.attrs = ads1015_attributes,
};

static const struct iio_info ads1015_info = {
	.driver_module	= THIS_MODULE,
	.read_raw	= ads1015_read_raw,
	.write_raw	= ads1015_write_raw,
	.attrs		= &ads1015_attribute_group,
};

static int ads1015_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct iio_dev *indio_dev;
	struct ads1015_data *data;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	data->client = client;

	indio_dev->dev.parent = &client->dev;
	indio_dev->info = &ads1015_info;
	indio_dev->name = ADS1015_DRV_NAME;
	indio_dev->channels = ads1015_channels;
	indio_dev->num_channels = ARRAY_SIZE(ads1015_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	data->regmap = devm_regmap_init_i2c(client, &ads1015_regmap_config);
	if (IS_ERR(data->regmap)) {
		dev_err(&client->dev, "Failed to allocate register map\n");
		return PTR_ERR(data->regmap);
	}

	ret = iio_triggered_buffer_setup(indio_dev, &iio_pollfunc_store_time,
					 ads1015_trigger_handler,
					 &ads1015_buffer_setup_ops);
	if (ret < 0) {
		dev_err(&client->dev, "iio triggered buffer setup failed\n");
		return ret;
	}

	ret = pm_runtime_set_active(&client->dev);
	if (ret) {
		iio_triggered_buffer_cleanup(indio_dev);
		return ret;
	}

	pm_runtime_enable(&client->dev);

	pm_runtime_set_autosuspend_delay(&client->dev, ADS1015_SLEEP_DELAY_MS);
	pm_runtime_use_autosuspend(&client->dev);

	return devm_iio_device_register(&client->dev, indio_dev);
}

static int ads1015_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ads1015_data *data = iio_priv(indio_dev);

	pm_runtime_disable(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	pm_runtime_put_noidle(&client->dev);

	iio_triggered_buffer_cleanup(indio_dev);

	/* power down single shot mode */
	return regmap_update_bits(data->regmap, ADS1015_CONFIG_REG,
				  ADS1015_CONFIG_MODE, 0x01 << 8);
}

#ifdef CONFIG_PM
static int ads1015_runtime_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct ads1015_data *data = iio_priv(indio_dev);

	return regmap_update_bits(data->regmap, ADS1015_CONFIG_REG,
				  ADS1015_CONFIG_MODE, 0x01 << 8);
}

static int ads1015_runtime_resume(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct ads1015_data *data = iio_priv(indio_dev);

	return regmap_update_bits(data->regmap, ADS1015_CONFIG_REG,
				  ADS1015_CONFIG_MODE, 0x00 << 8);
}
#endif

static const struct dev_pm_ops ads1015_pm_ops = {
	SET_RUNTIME_PM_OPS(ads1015_runtime_suspend,
			   ads1015_runtime_resume, NULL)
};

static const struct i2c_device_id ads1015_id[] = {
	{"ads1015", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, ads1015_id);

static struct i2c_driver ads1015_driver = {
	.driver = {
		.name = ADS1015_DRV_NAME,
		.pm = &ads1015_pm_ops,
	},
	.probe		= ads1015_probe,
	.remove		= ads1015_remove,
	.id_table	= ads1015_id,
};

module_i2c_driver(ads1015_driver);

MODULE_AUTHOR("Daniel Baluta <daniel.baluta@intel.com>");
MODULE_DESCRIPTION("Texas Instruments ADS1015 ADC driver");
MODULE_LICENSE("GPL v2");
