// SPDX-License-Identifier: GPL-2.0
/*
 * Sensirion SPS30 Particulate Matter sensor driver
 *
 * Copyright (c) Tomasz Duszynski <tduszyns@gmail.com>
 *
 * I2C slave address: 0x69
 *
 * TODO:
 *  - support for turning on fan cleaning
 *  - support for reading/setting auto cleaning interval
 */

#define pr_fmt(fmt) "sps30: " fmt

#include <linux/crc8.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/module.h>

#define SPS30_CRC8_POLYNOMIAL 0x31

/* SPS30 commands */
#define SPS30_START_MEAS 0x0010
#define SPS30_STOP_MEAS 0x0104
#define SPS30_RESET 0xd304
#define SPS30_READ_DATA_READY_FLAG 0x0202
#define SPS30_READ_DATA 0x0300
#define SPS30_READ_SERIAL 0xD033

#define SPS30_CHAN(_index, _mod) { \
	.type = IIO_MASSCONCENTRATION, \
	.modified = 1, \
	.channel2 = IIO_MOD_ ## _mod, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED), \
	.scan_index = _index, \
	.scan_type = { \
		.sign = 'u', \
		.realbits = 12, \
		.storagebits = 32, \
		.endianness = IIO_CPU, \
	}, \
}

enum {
	PM1p0, /* just a placeholder */
	PM2p5,
	PM4p0, /* just a placeholder */
	PM10,
};

struct sps30_state {
	struct i2c_client *client;
	/* guards against concurrent access to sensor registers */
	struct mutex lock;
};

DECLARE_CRC8_TABLE(sps30_crc8_table);

static int sps30_write_then_read(struct sps30_state *state, u8 *buf,
				 int buf_size, u8 *data, int data_size)
{
	/* every two received data bytes are checksummed */
	u8 tmp[data_size + data_size / 2];
	int ret, i;

	/*
	 * Sensor does not support repeated start so instead of
	 * sending two i2c messages in a row we just send one by one.
	 */
	ret = i2c_master_send(state->client, buf, buf_size);
	if (ret != buf_size)
		return ret < 0 ? ret : -EIO;

	if (!data)
		return 0;

	ret = i2c_master_recv(state->client, tmp, sizeof(tmp));
	if (ret != sizeof(tmp))
		return ret < 0 ? ret : -EIO;

	for (i = 0; i < sizeof(tmp); i += 3) {
		u8 crc = crc8(sps30_crc8_table, &tmp[i], 2, CRC8_INIT_VALUE);

		if (crc != tmp[i + 2]) {
			dev_err(&state->client->dev,
				"data integrity check failed\n");
			return -EIO;
		}

		*data++ = tmp[i];
		*data++ = tmp[i + 1];
	}

	return 0;
}

static int sps30_do_cmd(struct sps30_state *state, u16 cmd, u8 *data, int size)
{
	/* depending on the command up to 3 bytes may be needed for argument */
	u8 buf[sizeof(cmd) + 3] = { cmd >> 8, cmd };

	switch (cmd) {
	case SPS30_START_MEAS:
		buf[2] = 0x03;
		buf[3] = 0x00;
		buf[4] = 0xac; /* precomputed crc */
		return sps30_write_then_read(state, buf, 5, NULL, 0);
	case SPS30_STOP_MEAS:
	case SPS30_RESET:
		return sps30_write_then_read(state, buf, 2, NULL, 0);
	case SPS30_READ_DATA_READY_FLAG:
	case SPS30_READ_DATA:
	case SPS30_READ_SERIAL:
		return sps30_write_then_read(state, buf, 2, data, size);
	default:
		return -EINVAL;
	};
}

static int sps30_ieee754_to_int(const u8 *data)
{
	u32 val = ((u32)data[0] << 24) | ((u32)data[1] << 16) |
		  ((u32)data[2] << 8) | (u32)data[3],
	    mantissa = (val << 9) >> 9;
	int exp = (val >> 23) - 127;

	if (!exp && !mantissa)
		return 0;

	if (exp < 0)
		return 0;

	return (1 << exp) + (mantissa >> (23 - exp));
}

static int sps30_do_meas(struct sps30_state *state, int *pm2p5, int *pm10)
{
	/*
	 * Internally sensor stores measurements in a following manner:
	 *
	 * PM1p0: upper two bytes, crc8, lower two bytes, crc8
	 * PM2p5: upper two bytes, crc8, lower two bytes, crc8
	 * PM4p0: upper two bytes, crc8, lower two bytes, crc8
	 * PM10:  upper two bytes, crc8, lower two bytes, crc8
	 *
	 * What follows next are number concentration measurements and
	 * typical particle size measurement.
	 *
	 * Once data is read from sensor crc bytes are stripped off
	 * hence we need 16 bytes of buffer space.
	 */
	int ret, tries = 5;
	u8 buf[16];

	while (tries--) {
		ret = sps30_do_cmd(state, SPS30_READ_DATA_READY_FLAG, buf, 2);
		if (ret)
			return -EIO;

		/* new measurements ready to be read */
		if (buf[1] == 1)
			break;

		usleep_range(300000, 400000);
	}

	if (!tries)
		return -ETIMEDOUT;

	ret = sps30_do_cmd(state, SPS30_READ_DATA, buf, sizeof(buf));
	if (ret)
		return ret;

	/*
	 * All measurements come in IEEE754 single precision floating point
	 * format but sensor itself is not precise enough (-+ 10% error)
	 * to take full advantage of it. Hence converting result to int
	 * to keep things simple.
	 */
	*pm2p5 = sps30_ieee754_to_int(&buf[PM2p5 * 4]);
	*pm10 = sps30_ieee754_to_int(&buf[PM10 * 4]);

	return 0;
}

static irqreturn_t sps30_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct sps30_state *state = iio_priv(indio_dev);
	u32 buf[4]; /* PM2p5, PM10, timestamp */
	int ret;

	mutex_lock(&state->lock);
	ret = sps30_do_meas(state, &buf[0], &buf[1]);
	mutex_unlock(&state->lock);
	if (ret < 0)
		goto err;

	iio_push_to_buffers_with_timestamp(indio_dev, buf,
					   iio_get_time_ns(indio_dev));
err:
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static int sps30_read_raw(struct iio_dev *indio_dev,
			  struct iio_chan_spec const *chan,
			  int *val, int *val2, long mask)
{
	struct sps30_state *state = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_PROCESSED:
		switch (chan->type) {
		case IIO_MASSCONCENTRATION:
			mutex_lock(&state->lock);
			switch (chan->channel2) {
			case IIO_MOD_PM2p5:
				ret = sps30_do_meas(state, val, val2);
				break;
			case IIO_MOD_PM10:
				ret = sps30_do_meas(state, val2, val);
				break;
			default:
				break;
			}
			mutex_unlock(&state->lock);
			if (ret)
				return ret;

			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}
}

static const struct iio_info sps30_info = {
	.read_raw = sps30_read_raw,
};

static const struct iio_chan_spec sps30_channels[] = {
	SPS30_CHAN(0, PM2p5),
	SPS30_CHAN(1, PM10),
	IIO_CHAN_SOFT_TIMESTAMP(2),
};

static const unsigned long sps30_scan_masks[] = { 0x03, 0x00 };

static int sps30_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct sps30_state *state;
	u8 buf[32] = { };
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*state));
	if (!indio_dev)
		return -ENOMEM;

	state = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	state->client = client;
	indio_dev->dev.parent = &client->dev;
	indio_dev->info = &sps30_info;
	indio_dev->name = client->name;
	indio_dev->channels = sps30_channels;
	indio_dev->num_channels = ARRAY_SIZE(sps30_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->available_scan_masks = sps30_scan_masks;

	mutex_init(&state->lock);
	crc8_populate_msb(sps30_crc8_table, SPS30_CRC8_POLYNOMIAL);

	/*
	 * Power-on-reset causes sensor to produce some glitch on i2c bus
	 * and some controllers end up in error state. Recover simply
	 * by placing something on the bus.
	 */
	ret = sps30_do_cmd(state, SPS30_RESET, NULL, 0);
	if (ret) {
		dev_err(&client->dev, "failed to reset device\n");
		return ret;
	}
	usleep_range(2500000, 3500000);
	sps30_do_cmd(state, SPS30_STOP_MEAS, NULL, 0);

	ret = sps30_do_cmd(state, SPS30_READ_SERIAL, buf, sizeof(buf));
	if (ret) {
		dev_err(&client->dev, "failed to read serial number\n");
		return ret;
	}
	dev_info(&client->dev, "serial number: %s\n", buf);

	ret = sps30_do_cmd(state, SPS30_START_MEAS, NULL, 0);
	if (ret) {
		dev_err(&client->dev, "failed to start measurement\n");
		return ret;
	}

	ret = devm_iio_triggered_buffer_setup(&client->dev, indio_dev, NULL,
					      sps30_trigger_handler, NULL);
	if (ret)
		return ret;

	return devm_iio_device_register(&client->dev, indio_dev);
}

static int sps30_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct sps30_state *state = iio_priv(indio_dev);

	sps30_do_cmd(state, SPS30_STOP_MEAS, NULL, 0);

	return 0;
}

static const struct i2c_device_id sps30_id[] = {
	{ "sps30" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sps30_id);

static const struct of_device_id sps30_of_match[] = {
	{ .compatible = "sensirion,sps30" },
	{ }
};
MODULE_DEVICE_TABLE(of, sps30_of_match);

static struct i2c_driver sps30_driver = {
	.driver = {
		.name = "sps30",
		.of_match_table = sps30_of_match,
	},
	.id_table = sps30_id,
	.probe_new = sps30_probe,
	.remove = sps30_remove,
};
module_i2c_driver(sps30_driver);

MODULE_AUTHOR("Tomasz Duszynski <tduszyns@gmail.com>");
MODULE_DESCRIPTION("Sensirion SPS30 particulate matter sensor driver");
MODULE_LICENSE("GPL v2");
