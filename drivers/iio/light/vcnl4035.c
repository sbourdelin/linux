// SPDX-License-Identifier: GPL-2.0
/*
 * VCNL4035 Ambient Light and Proximity Sensor - 7-bit I2C slave address 0x60
 *
 * Copyright (c) 2018, DENX Software Engineering GmbH
 * Author: Parthiban Nallathambi <pn@denx.de>
 *
 *
 * TODO: Proximity
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/pm_runtime.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#define VCNL4035_DRV_NAME	"vcnl4035"
#define VCNL4035_IRQ_NAME	"vcnl4035_event"
#define VCNL4035_REGMAP_NAME	"vcnl4035_regmap"

/* Device registers */
#define VCNL4035_ALS_CONF	0x00
#define VCNL4035_ALS_THDH	0x01
#define VCNL4035_ALS_THDL	0x02
#define VCNL4035_ALS_DATA	0x0B
#define	VCNL4035_INT_FLAG	0x0D
#define VCNL4035_DEV_ID		0x0E

/* Register masks */
#define VCNL4035_MODE_ALS_MASK		BIT(0)
#define VCNL4035_MODE_ALS_INT_MASK	BIT(1)
#define VCNL4035_ALS_IT_MASK		GENMASK(7, 5)
#define VCNL4035_ALS_PERS_MASK		GENMASK(3, 2)
#define VCNL4035_INT_ALS_IF_H_MASK	BIT(12)
#define VCNL4035_INT_ALS_IF_L_MASK	BIT(13)

/* Default values */
#define VCNL4035_MODE_ALS_ENABLE	BIT(0)
#define VCNL4035_MODE_ALS_DISABLE	0x00
#define VCNL4035_MODE_ALS_INT_ENABLE	BIT(1)
#define VCNL4035_MODE_ALS_INT_DISABLE	0x00
#define VCNL4035_DEV_ID_VAL		0x80
#define VCNL4035_ALS_IT_DEFAULT		0x01
#define VCNL4035_ALS_PERS_DEFAULT	0x00
#define VCNL4035_ALS_THDH_DEFAULT	5000
#define VCNL4035_ALS_THDL_DEFAULT	100
#define VCNL4035_SLEEP_DELAY_MS		2000

struct vcnl4035_data {
	struct i2c_client *client;
	struct regmap *regmap;
	/* protect device settings persistence, integration time, threshold */
	struct mutex lock;
	unsigned int als_it_val;
	unsigned int als_persistence:4;
	unsigned int als_thresh_low;
	unsigned int als_thresh_high;
	struct iio_trigger *drdy_trigger0;
	s64 irq_timestamp;
};

static inline bool vcnl4035_is_triggered(struct vcnl4035_data *data)
{
	int ret;
	int reg;

	ret = regmap_read(data->regmap, VCNL4035_INT_FLAG, &reg);
	if (ret < 0)
		return false;
	if (reg & (VCNL4035_INT_ALS_IF_H_MASK | VCNL4035_INT_ALS_IF_L_MASK))
		return true;
	else
		return false;
}

static irqreturn_t vcnl4035_drdy_irq_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct vcnl4035_data *data = iio_priv(indio_dev);

	data->irq_timestamp = iio_get_time_ns(indio_dev);
	return IRQ_WAKE_THREAD;
}

static irqreturn_t vcnl4035_drdy_irq_thread(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct vcnl4035_data *data = iio_priv(indio_dev);

	if (vcnl4035_is_triggered(data)) {
		iio_trigger_poll_chained(data->drdy_trigger0);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

/* Triggered buffer */
static irqreturn_t vcnl4035_trigger_consumer_store_time(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;

	if (!iio_trigger_using_own(indio_dev))
		pf->timestamp = iio_get_time_ns(indio_dev);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t vcnl4035_trigger_consumer_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct vcnl4035_data *data = iio_priv(indio_dev);
	int als_data;
	int ret;

	if (iio_trigger_using_own(indio_dev) && data->irq_timestamp) {
		pf->timestamp = data->irq_timestamp;
		data->irq_timestamp = 0;
	}

	if (!pf->timestamp)
		pf->timestamp = iio_get_time_ns(indio_dev);

	mutex_lock(&data->lock);
	ret = regmap_read(data->regmap, VCNL4035_ALS_DATA, &als_data);
	mutex_unlock(&data->lock);
	if (!ret)
		iio_push_to_buffers_with_timestamp(indio_dev,
						   &als_data,
						   pf->timestamp);
	else
		dev_err(&data->client->dev,
			"Trigger consumer can't read from sensor.\n");
	pf->timestamp = 0;

	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static int vcnl4035_als_drdy_set_state(struct iio_trigger *trigger,
					bool enable_drdy)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trigger);
	struct vcnl4035_data *data = iio_priv(indio_dev);
	int val = enable_drdy ? VCNL4035_MODE_ALS_INT_ENABLE :
					VCNL4035_MODE_ALS_INT_DISABLE;
	int ret;

	ret = regmap_update_bits(data->regmap, VCNL4035_ALS_CONF,
				 VCNL4035_MODE_ALS_INT_MASK,
				 val);
	if (ret)
		dev_err(&data->client->dev, "%s failed\n", __func__);

	return ret;
}

static const struct iio_trigger_ops vcnl4035_trigger_ops = {
	.set_trigger_state = vcnl4035_als_drdy_set_state,
};

/*
 *	Device IT	INT Time(ms)	Scale (lux/step)
 *	000		50		0.064
 *	001		100		0.032
 *	010		200		0.016
 *	100		400		0.008
 *	101 - 111	800		0.004
 * Values are proportial, so ALS INT is selected for input due to
 * simplicity reason. Integration time value and scaling is
 * calculated based on device INT value
 *
 * Raw value needs to be scaled using ALS STEPS
 *
 */
static int vcnl4035_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int *val,
			    int *val2, long mask)
{
	struct vcnl4035_data *data = iio_priv(indio_dev);
	int ret;
	int busy;
	int raw_data;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		busy = iio_device_claim_direct_mode(indio_dev);
		if (busy)
			return -EBUSY;

		ret = regmap_read(data->regmap, VCNL4035_ALS_DATA, &raw_data);
		iio_device_release_direct_mode(indio_dev);
		if (ret < 0)
			return ret;

		*val = raw_data;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_INT_TIME:
		mutex_lock(&data->lock);
		*val = data->als_it_val * 100;
		if (!*val)
			*val = 50;
		mutex_unlock(&data->lock);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		mutex_lock(&data->lock);
		*val = 64;
		if (!data->als_it_val)
			*val2 = 1000;
		else
			*val2 = data->als_it_val * 2 * 1000;
		mutex_unlock(&data->lock);
		return IIO_VAL_FRACTIONAL;
	default:
		return -EINVAL;
	}
}

static int vcnl4035_write_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int val, int val2, long mask)
{
	int ret;
	struct vcnl4035_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		if (val <= 0 || val > 800)
			return -EINVAL;
		mutex_lock(&data->lock);
		ret = regmap_update_bits(data->regmap, VCNL4035_ALS_CONF,
					 VCNL4035_ALS_IT_MASK,
					 val / 100);
		if (!ret)
			data->als_it_val = val / 100;
		mutex_unlock(&data->lock);
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

/* No direct ABI for persistence and threshold, so eventing */
static int vcnl4035_read_thresh(struct iio_dev *indio_dev,
		const struct iio_chan_spec *chan, enum iio_event_type type,
		enum iio_event_direction dir, enum iio_event_info info,
		int *val, int *val2)
{
	struct vcnl4035_data *data = iio_priv(indio_dev);

	switch (info) {
	case IIO_EV_INFO_VALUE:
		switch (dir) {
		case IIO_EV_DIR_RISING:
			mutex_lock(&data->lock);
			*val = data->als_thresh_high;
			mutex_unlock(&data->lock);
			break;
		case IIO_EV_DIR_FALLING:
			mutex_lock(&data->lock);
			*val = data->als_thresh_low;
			mutex_unlock(&data->lock);
			break;
		default:
			return -EINVAL;
		}
		break;
	case IIO_EV_INFO_PERIOD:
		mutex_lock(&data->lock);
		*val = data->als_persistence;
		mutex_unlock(&data->lock);
		break;
	default:
		return -EINVAL;
	}

	return IIO_VAL_INT;
}

static int vcnl4035_write_thresh(struct iio_dev *indio_dev,
		const struct iio_chan_spec *chan, enum iio_event_type type,
		enum iio_event_direction dir, enum iio_event_info info, int val,
		int val2)
{
	struct vcnl4035_data *data = iio_priv(indio_dev);
	int ret;

	switch (info) {
	case IIO_EV_INFO_VALUE:
		/* 16 bit threshold range */
		if (val < 0 || val > 65535)
			return -EINVAL;
		if (dir == IIO_EV_DIR_RISING) {
			if (val < data->als_thresh_low)
				return -EINVAL;
			mutex_lock(&data->lock);
			ret = regmap_write(data->regmap, VCNL4035_ALS_THDH,
					   val);
			mutex_unlock(&data->lock);
			if (ret)
				return ret;
			data->als_thresh_high = val;
		} else {
			if (val > data->als_thresh_high)
				return -EINVAL;
			mutex_lock(&data->lock);
			ret = regmap_write(data->regmap, VCNL4035_ALS_THDL,
					   val);
			mutex_unlock(&data->lock);
			if (ret)
				return ret;
			data->als_thresh_low = val;
		}
		break;
	case IIO_EV_INFO_PERIOD:
		/* allow only 1 2 4 8 as persistence value */
		if (val < 0 || val > 8 || (__sw_hweight8(val) != 1))
			return -EINVAL;
		mutex_lock(&data->lock);
		ret = regmap_update_bits(data->regmap, VCNL4035_ALS_CONF,
					 VCNL4035_ALS_PERS_MASK, val);
		mutex_unlock(&data->lock);
		if (ret)
			return ret;
		data->als_persistence = val;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static IIO_CONST_ATTR(als_available_integration_time, "50 100 200 400 800");
static IIO_CONST_ATTR(als_available_persistence, "1 2 4 8");
static IIO_CONST_ATTR(als_available_threshold_range, "0 65535");

static struct attribute *vcnl4035_attributes[] = {
	&iio_const_attr_als_available_integration_time.dev_attr.attr,
	&iio_const_attr_als_available_threshold_range.dev_attr.attr,
	&iio_const_attr_als_available_persistence.dev_attr.attr,
	NULL,
};

static const struct attribute_group vcnl4035_attribute_group = {
	.attrs = vcnl4035_attributes,
};

static const struct iio_info vcnl4035_info = {
	.read_raw		= vcnl4035_read_raw,
	.write_raw		= vcnl4035_write_raw,
	.read_event_value	= vcnl4035_read_thresh,
	.write_event_value	= vcnl4035_write_thresh,
	.attrs			= &vcnl4035_attribute_group,
};

enum vcnl4035_scan_index_order {
	VCNL4035_CHAN_INDEX_LIGHT,
};

static const unsigned long vcnl4035_available_scan_masks[] = {
	BIT(VCNL4035_CHAN_INDEX_LIGHT), 0
};

static const struct iio_event_spec vcnl4035_event_spec[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	}, {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	}, {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_EITHER,
		.mask_separate = BIT(IIO_EV_INFO_PERIOD),
	},
};

static const struct iio_chan_spec vcnl4035_channels[] = {
	{
		.type = IIO_INTENSITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				BIT(IIO_CHAN_INFO_INT_TIME) |
				BIT(IIO_CHAN_INFO_SCALE),
		.event_spec = vcnl4035_event_spec,
		.num_event_specs = ARRAY_SIZE(vcnl4035_event_spec),
		.scan_index = VCNL4035_CHAN_INDEX_LIGHT,
		.scan_type = {
			.sign = 'u',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_LE,
		},
	},
};

static int vcnl4035_set_als_power_state(struct vcnl4035_data *data, u8 status)
{
	return regmap_update_bits(data->regmap, VCNL4035_ALS_CONF,
					VCNL4035_MODE_ALS_MASK,
					status);
}

static int vcnl4035_init(struct vcnl4035_data *data)
{
	int ret;
	int id;

	ret = regmap_read(data->regmap, VCNL4035_DEV_ID, &id);
	if (ret < 0) {
		dev_err(&data->client->dev, "Failed to read DEV_ID register\n");
		return ret;
	}

	if (id != VCNL4035_DEV_ID_VAL) {
		dev_err(&data->client->dev, "Wrong id, got %x, expected %x\n",
			id, VCNL4035_DEV_ID_VAL);
		return -ENODEV;
	}

#ifndef CONFIG_PM
	ret = vcnl4035_set_als_power_state(data, VCNL4035_MODE_ALS_ENABLE);
	if (ret < 0)
		return ret;
#endif
	/* set default integration time - 100 ms for ALS */
	ret = regmap_update_bits(data->regmap, VCNL4035_ALS_CONF,
				 VCNL4035_ALS_IT_MASK,
				 VCNL4035_ALS_IT_DEFAULT);
	if (ret) {
		pr_err("regmap_update_bits default ALS IT returned %d\n", ret);
		return ret;
	}
	data->als_it_val = VCNL4035_ALS_IT_DEFAULT;

	/* set default persistence time - 1 for ALS */
	ret = regmap_update_bits(data->regmap, VCNL4035_ALS_CONF,
				 VCNL4035_ALS_PERS_MASK,
				 VCNL4035_ALS_PERS_DEFAULT);
	if (ret) {
		pr_err("regmap_update_bits default PERS returned %d\n", ret);
		return ret;
	}
	data->als_persistence = VCNL4035_ALS_PERS_DEFAULT;

	/* set default HIGH threshold for ALS */
	ret = regmap_write(data->regmap, VCNL4035_ALS_THDH,
				VCNL4035_ALS_THDH_DEFAULT);
	if (ret) {
		pr_err("regmap_write default THDH returned %d\n", ret);
		return ret;
	}
	data->als_thresh_high = VCNL4035_ALS_THDH_DEFAULT;

	/* set default LOW threshold for ALS */
	ret = regmap_write(data->regmap, VCNL4035_ALS_THDL,
				VCNL4035_ALS_THDL_DEFAULT);
	if (ret) {
		pr_err("regmap_write default THDL returned %d\n", ret);
		return ret;
	}
	data->als_thresh_low = VCNL4035_ALS_THDL_DEFAULT;

	return 0;
}

static bool vcnl4035_is_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case VCNL4035_ALS_CONF:
	case VCNL4035_DEV_ID:
		return false;
	default:
		return true;
	}
}

static const struct regmap_config vcnl4035_regmap_config = {
	.name		= VCNL4035_REGMAP_NAME,
	.reg_bits	= 8,
	.val_bits	= 16,
	.max_register	= VCNL4035_DEV_ID,
	.cache_type	= REGCACHE_RBTREE,
	.volatile_reg	= vcnl4035_is_volatile_reg,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};

static int vcnl4035_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct vcnl4035_data *data;
	struct iio_dev *indio_dev;
	struct regmap *regmap;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	regmap = devm_regmap_init_i2c(client, &vcnl4035_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&client->dev, "regmap_init failed!\n");
		return PTR_ERR(regmap);
	}

	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	data->client = client;
	data->regmap = regmap;
	mutex_init(&data->lock);

	indio_dev->dev.parent = &client->dev;
	indio_dev->info = &vcnl4035_info;
	indio_dev->name = VCNL4035_DRV_NAME;
	indio_dev->channels = vcnl4035_channels;
	indio_dev->num_channels = ARRAY_SIZE(vcnl4035_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = vcnl4035_init(data);
	if (ret < 0) {
		dev_err(&client->dev, "vcnl4035 chip init failed\n");
		return ret;
	}

	ret = pm_runtime_set_active(&client->dev);
	if (ret < 0)
		goto fail_poweroff;

	pm_runtime_enable(&client->dev);
	pm_runtime_set_autosuspend_delay(&client->dev, VCNL4035_SLEEP_DELAY_MS);
	pm_runtime_use_autosuspend(&client->dev);

	if (client->irq) {
		data->drdy_trigger0 = devm_iio_trigger_alloc(
			indio_dev->dev.parent,
			"%s-dev%d", indio_dev->name, indio_dev->id);
		if (!data->drdy_trigger0) {
			ret = -ENOMEM;
			goto fail_pm_disable;
		}
		data->drdy_trigger0->dev.parent = indio_dev->dev.parent;
		data->drdy_trigger0->ops = &vcnl4035_trigger_ops;
		indio_dev->available_scan_masks = vcnl4035_available_scan_masks;
		iio_trigger_set_drvdata(data->drdy_trigger0, indio_dev);

		/* IRQ to trigger mapping */
		ret = devm_request_threaded_irq(&client->dev, client->irq,
			vcnl4035_drdy_irq_handler, vcnl4035_drdy_irq_thread,
			IRQF_TRIGGER_LOW | IRQF_ONESHOT,
			VCNL4035_IRQ_NAME, indio_dev);
		if (ret < 0) {
			dev_err(&client->dev, "request irq %d for trigger0 failed\n",
				client->irq);
			goto fail_pm_disable;
			}

		ret = devm_iio_trigger_register(indio_dev->dev.parent,
						data->drdy_trigger0);
		if (ret) {
			dev_err(&client->dev, "iio trigger register failed\n");
			goto fail_pm_disable;
		}

		/* Trigger setup */
		ret = devm_iio_triggered_buffer_setup(indio_dev->dev.parent,
			indio_dev,
			vcnl4035_trigger_consumer_store_time,
			vcnl4035_trigger_consumer_handler,
			NULL);
		if (ret < 0) {
			dev_err(&client->dev, "iio triggered buffer setup failed\n");
			goto fail_pm_disable;
		}
	}

	ret = iio_device_register(indio_dev);
	if (ret)
		goto fail_pm_disable;
	dev_info(&client->dev, "%s Ambient light/proximity sensor\n",
						VCNL4035_DRV_NAME);
	return 0;

fail_pm_disable:
	pm_runtime_disable(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	pm_runtime_put_noidle(&client->dev);
fail_poweroff:
	return vcnl4035_set_als_power_state(data, VCNL4035_MODE_ALS_DISABLE);
}

static int vcnl4035_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);

	iio_device_unregister(indio_dev);

	pm_runtime_disable(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	pm_runtime_put_noidle(&client->dev);

	return vcnl4035_set_als_power_state(iio_priv(indio_dev),
					VCNL4035_MODE_ALS_DISABLE);
}

#ifdef CONFIG_PM
static int vcnl4035_runtime_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct vcnl4035_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	ret = vcnl4035_set_als_power_state(data, VCNL4035_MODE_ALS_DISABLE);
	regcache_mark_dirty(data->regmap);
	mutex_unlock(&data->lock);

	return ret;
}

static int vcnl4035_runtime_resume(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct vcnl4035_data *data = iio_priv(indio_dev);
	int ret;

	regcache_sync(data->regmap);
	ret = vcnl4035_set_als_power_state(data, VCNL4035_MODE_ALS_ENABLE);
	if (ret < 0)
		return ret;
	/* wait for 1 ALS integration cycle */
	msleep(data->als_it_val * 100);

	return 0;
}
#endif

static const struct dev_pm_ops vcnl4035_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(vcnl4035_runtime_suspend,
			   vcnl4035_runtime_resume, NULL)
};

static const struct of_device_id vcnl4035_of_match[] = {
	{ .compatible = "vishay,vcnl4035", },
	{ },
};
MODULE_DEVICE_TABLE(of, vcnl4035_of_match);

static const struct i2c_device_id vcnl4035_id[] = {
	{ "vcnl4035", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, vcnl4035_id);

static struct i2c_driver vcnl4035_driver = {
	.driver = {
		.name   = VCNL4035_DRV_NAME,
		.pm	= &vcnl4035_pm_ops,
		.of_match_table = of_match_ptr(vcnl4035_of_match),
	},
	.probe  = vcnl4035_probe,
	.remove	= vcnl4035_remove,
	.id_table = vcnl4035_id,
};

module_i2c_driver(vcnl4035_driver);

MODULE_AUTHOR("Parthiban Nallathambi <pn@denx.de>");
MODULE_DESCRIPTION("VCNL4035 Ambient Light Sensor driver");
MODULE_LICENSE("GPL v2");
