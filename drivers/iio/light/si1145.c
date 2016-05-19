/*
 * si1145.c - Support for Silabs SI1132 and SI1141/2/3/5/6/7 combined ambient
 * light, UV index and proximity sensors
 *
 * Copyright 2014 Peter Meerwald-Stadler <pmeerw@pmeerw.net>
 *
 * This file is subject to the terms and conditions of version 2 of
 * the GNU General Public License.  See the file COPYING in the main
 * directory of this archive for more details.
 *
 * SI1132 (7-bit I2C slave address 0x60)
 * SI1141/2/3 (7-bit I2C slave address 0x5a)
 * SI1145/6/6 (7-bit I2C slave address 0x60)
 *
 * TODO: sample freq, IRQ, power management
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/gpio.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/buffer.h>

#define SI1145_REG_PART_ID		0x00
#define SI1145_REG_REV_ID		0x01
#define SI1145_REG_SEQ_ID		0x02
#define SI1145_REG_INT_CFG		0x03
#define SI1145_REG_IRQ_ENABLE		0x04
#define SI1145_REG_IRQ_MODE		0x05
#define SI1145_REG_HW_KEY		0x07
#define SI1145_REG_MEAS_RATE		0x08
#define SI1145_REG_PS_LED21		0x0f
#define SI1145_REG_PS_LED3		0x10
#define SI1145_REG_PARAM_WR		0x17
#define SI1145_REG_COMMAND		0x18
#define SI1145_REG_RESPONSE		0x20
#define SI1145_REG_IRQ_STATUS		0x21
#define SI1145_REG_ALSVIS_DATA		0x22
#define SI1145_REG_ALSIR_DATA		0x24
#define SI1145_REG_PS1_DATA		0x26
#define SI1145_REG_PS2_DATA		0x28
#define SI1145_REG_PS3_DATA		0x2a
#define SI1145_REG_AUX_DATA		0x2c
#define SI1145_REG_PARAM_RD		0x2e
#define SI1145_REG_CHIP_STAT		0x30

/* Helper to figure out PS_LED register / shift per channel */
#define SI1145_PS_LED_REG(ch) \
	(((ch) == 2) ? SI1145_REG_PS_LED3 : SI1145_REG_PS_LED21)
#define SI1145_PS_LED_SHIFT(ch) \
	(((ch) == 1) ? 4 : 0)

/* Parameter offsets */
#define SI1145_PARAM_CHLIST		0x01
#define SI1145_PARAM_PSLED12_SELECT	0x02
#define SI1145_PARAM_PSLED3_SELECT	0x03
#define SI1145_PARAM_PS_ENCODING	0x05
#define SI1145_PARAM_ALS_ENCODING	0x06
#define SI1145_PARAM_PS1_ADC_MUX	0x07
#define SI1145_PARAM_PS2_ADC_MUX	0x08
#define SI1145_PARAM_PS3_ADC_MUX	0x09
#define SI1145_PARAM_PS_ADC_COUNTER	0x0a
#define SI1145_PARAM_PS_ADC_GAIN	0x0b
#define SI1145_PARAM_PS_ADC_MISC	0x0c
#define SI1145_PARAM_ALS_ADC_MUX	0x0d
#define SI1145_PARAM_ALSIR_ADC_MUX	0x0e
#define SI1145_PARAM_AUX_ADC_MUX	0x0f
#define SI1145_PARAM_ALSVIS_ADC_COUNTER	0x10
#define SI1145_PARAM_ALSVIS_ADC_GAIN	0x11
#define SI1145_PARAM_ALSVIS_ADC_MISC	0x12
#define SI1145_PARAM_LED_RECOVERY	0x1c
#define SI1145_PARAM_ALSIR_ADC_COUNTER	0x1d
#define SI1145_PARAM_ALSIR_ADC_GAIN	0x1e
#define SI1145_PARAM_ALSIR_ADC_MISC	0x1f

/* Channel enable masks for CHLIST parameter */
#define SI1145_CHLIST_EN_PS1		0x01
#define SI1145_CHLIST_EN_PS2		0x02
#define SI1145_CHLIST_EN_PS3		0x04
#define SI1145_CHLIST_EN_ALSVIS		0x10
#define SI1145_CHLIST_EN_ALSIR		0x20
#define SI1145_CHLIST_EN_AUX		0x40
#define SI1145_CHLIST_EN_UV		0x80

/* Signal range mask for ADC_MISC parameter */
#define SI1145_ADC_MISC_RANGE		0x20

/* Commands for REG_COMMAND */
#define SI1145_CMD_NOP			0x00
#define SI1145_CMD_RESET		0x01
#define SI1145_CMD_PS_FORCE		0x05
#define SI1145_CMD_ALS_FORCE		0x06
#define SI1145_CMD_PSALS_FORCE		0x07
#define SI1145_CMD_PS_PAUSE		0x09
#define SI1145_CMD_ALS_PAUSE		0x0a
#define SI1145_CMD_PSALS_PAUSE		0x0b
#define SI1145_CMD_PS_AUTO		0x0d
#define SI1145_CMD_ALS_AUTO		0x0e
#define SI1145_CMD_PSALS_AUTO		0x0f
#define SI1145_CMD_PARAM_QUERY		0x80
#define SI1145_CMD_PARAM_SET		0xa0

/* Interrupt configuration masks for INT_CFG register */
#define SI1145_INT_CFG_OE		0x01 /* enable interrupt */
#define SI1145_INT_CFG_MODE		0x02 /* auto reset interrupt pin */

/* Interrupt enable masks for IRQ_ENABLE register */
#define SI1145_PS3_IE			0x10
#define SI1145_PS2_IE			0x08
#define SI1145_PS1_IE			0x04
#define SI1145_ALS_IE			0x01

#define SI1145_MUX_TEMP			0x65
#define SI1145_MUX_VDD			0x75

enum {
	SI1132,
	SI1141,
	SI1142,
	SI1143,
	SI1145,
	SI1146,
	SI1147,
};

struct si1145_part_info {
	u8 part;
	const struct iio_chan_spec *channels;
	unsigned int num_channels;
	unsigned int num_leds;
	bool new;
};

/**
 * struct si1145_data - si1145 chip state data
 * @client:	I2C client
 * @lock:	mutex to protect multi-step I2C accesses and state changes
 * @part:	chip part number (0x45, 0x46, 0x47) for 1 to 3 LEDs version
 *
 **/
struct si1145_data {
	struct i2c_client *client;
	struct mutex lock;
	const struct si1145_part_info *part_info;
	unsigned long scan_mask;
};

/*
 * Helper function to operate on parameter values: op can be query or set
 * Function returns (modified) value and needs locking
 */
static int __si1145_param(struct si1145_data *data, u8 op, u8 param, u8 value)
{
	int ret;

	if (op != SI1145_CMD_PARAM_QUERY) {
		ret = i2c_smbus_write_byte_data(data->client,
			SI1145_REG_PARAM_WR, value);
		if (ret < 0)
			return ret;
	}

	ret = i2c_smbus_write_byte_data(data->client, SI1145_REG_COMMAND,
		op | (param & 0x1F));
	if (ret < 0)
		return ret;

	return i2c_smbus_read_byte_data(data->client, SI1145_REG_PARAM_RD);
}

static int si1145_param(struct si1145_data *data, u8 op, u8 param, u8 value)
{
	int ret;

	mutex_lock(&data->lock);
	ret = __si1145_param(data, op, param, value);
	mutex_unlock(&data->lock);

	return ret;
}

static irqreturn_t si1145_trigger_handler(int irq, void *private)
{
	struct iio_poll_func *pf = private;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct si1145_data *data = iio_priv(indio_dev);
	/*
	 * Maximum buffer size:
	 *   6*2 bytes channels data + 4 bytes alignment +
	 *   8 bytes timestamp
	 */
	u8 buffer[24];
	int i, j = 0;
	int ret;

	ret = i2c_smbus_write_byte_data(data->client,
		SI1145_REG_COMMAND, SI1145_CMD_PSALS_FORCE);
	if (ret < 0)
		goto done;
	msleep(10);

	for_each_set_bit(i, indio_dev->active_scan_mask,
		indio_dev->masklength) {
		int run = 1;

		while (i + run < indio_dev->masklength) {
			if (test_bit(i + run, indio_dev->active_scan_mask))
				run++;
			else
				break;
		}

		if (run > 1) {
			ret = i2c_smbus_read_i2c_block_data(data->client,
				indio_dev->channels[i].address,
				sizeof(u16) * run, &buffer[j]);
		} else {
			ret = i2c_smbus_read_word_data(data->client,
				indio_dev->channels[i].address);
			*(u16 *) &buffer[j] = ret;
		}
		if (ret < 0)
			goto done;
		j += run * sizeof(u16);
		i += run - 1;
	}

	iio_push_to_buffers_with_timestamp(indio_dev, buffer,
		iio_get_time_ns());

done:
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

static int si1145_set_chlist(struct iio_dev *indio_dev, unsigned long scan_mask)
{
	struct si1145_data *data = iio_priv(indio_dev);
	u8 reg = 0, mux;
	int ret;
	int i;

	/* channel list already set, no need to reprogram */
	if (data->scan_mask == scan_mask)
		return 0;

	for_each_set_bit(i, &scan_mask, indio_dev->masklength) {
		switch (indio_dev->channels[i].address) {
		case SI1145_REG_ALSVIS_DATA:
			reg |= SI1145_CHLIST_EN_ALSVIS;
			break;
		case SI1145_REG_ALSIR_DATA:
			reg |= SI1145_CHLIST_EN_ALSIR;
			break;
		case SI1145_REG_PS1_DATA:
			reg |= SI1145_CHLIST_EN_PS1;
			break;
		case SI1145_REG_PS2_DATA:
			reg |= SI1145_CHLIST_EN_PS2;
			break;
		case SI1145_REG_PS3_DATA:
			reg |= SI1145_CHLIST_EN_PS3;
			break;
		case SI1145_REG_AUX_DATA:
			switch (indio_dev->channels[i].type) {
			case IIO_UVINDEX:
				reg |= SI1145_CHLIST_EN_UV;
				break;
			default:
				reg |= SI1145_CHLIST_EN_AUX;
				if (indio_dev->channels[i].type == IIO_TEMP)
					mux = SI1145_MUX_TEMP;
				else
					mux = SI1145_MUX_VDD;
				ret = __si1145_param(data, SI1145_CMD_PARAM_SET,
					SI1145_PARAM_AUX_ADC_MUX, mux);
				if (ret < 0)
					return ret;

				break;
			}
		}
	}

	data->scan_mask = scan_mask;
	return __si1145_param(data, SI1145_CMD_PARAM_SET, SI1145_PARAM_CHLIST,
		reg);
}

static int si1145_measure(struct iio_dev *indio_dev,
			  struct iio_chan_spec const *chan)
{
	struct si1145_data *data = iio_priv(indio_dev);
	u8 cmd;
	int ret;

	ret = si1145_set_chlist(indio_dev, BIT(chan->scan_index));
	if (ret < 0)
		return ret;

	cmd = (chan->type == IIO_PROXIMITY) ? SI1145_CMD_PS_FORCE :
		SI1145_CMD_ALS_FORCE;
	ret = i2c_smbus_write_byte_data(data->client, SI1145_REG_COMMAND, cmd);
	if (ret < 0)
		return ret;

	msleep(20);

	return i2c_smbus_read_word_data(data->client, chan->address);
}

static int si1145_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask)
{
	struct si1145_data *data = iio_priv(indio_dev);
	int ret;
	u8 reg;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_INTENSITY:
		case IIO_PROXIMITY:
		case IIO_VOLTAGE:
		case IIO_TEMP:
			if (iio_buffer_enabled(indio_dev))
				return -EBUSY;

			mutex_lock(&data->lock);
			ret = si1145_measure(indio_dev, chan);
			mutex_unlock(&data->lock);
			if (ret < 0)
				return ret;

			*val = ret;

			return IIO_VAL_INT;
		case IIO_CURRENT:
			ret = i2c_smbus_read_byte_data(data->client,
				SI1145_PS_LED_REG(chan->channel));
			if (ret < 0)
				return ret;

			*val = (ret >> SI1145_PS_LED_SHIFT(chan->channel))
				& 0x0f;

			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_PROXIMITY:
			reg = SI1145_PARAM_PS_ADC_GAIN;
			break;
		case IIO_INTENSITY:
			if (chan->channel2 == IIO_MOD_LIGHT_IR)
				reg = SI1145_PARAM_ALSIR_ADC_GAIN;
			else
				reg = SI1145_PARAM_ALSVIS_ADC_GAIN;
			break;
		case IIO_TEMP:
			*val = 28;
			*val2 = 571429;
			return IIO_VAL_INT_PLUS_MICRO;
		default:
			return -EINVAL;
		}

		ret = si1145_param(data, SI1145_CMD_PARAM_QUERY, reg, 0);
		if (ret < 0)
			return ret;

		*val = ret & 0x07;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_OFFSET:
		/* -ADC offset - ADC counts @ 25°C - 35 * ADC counts / °C */
		*val = -256 - 11136 + 25 * 35;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int si1145_write_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan,
			       int val, int val2, long mask)
{
	struct si1145_data *data = iio_priv(indio_dev);
	u8 reg1, reg2, shift;
	int ret;

	if (iio_buffer_enabled(indio_dev))
		return -EBUSY;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_PROXIMITY:
			if (val < 0 || val > 5 || val2 != 0)
				return -EINVAL;
			reg1 = SI1145_PARAM_PS_ADC_GAIN;
			reg2 = SI1145_PARAM_PS_ADC_COUNTER;
			break;
		case IIO_INTENSITY:
			if (val < 0 || val > 7 || val2 != 0)
				return -EINVAL;
			if (chan->channel2 == IIO_MOD_LIGHT_IR) {
				reg1 = SI1145_PARAM_ALSIR_ADC_GAIN;
				reg2 = SI1145_PARAM_ALSIR_ADC_COUNTER;
			} else {
				reg1 = SI1145_PARAM_ALSVIS_ADC_GAIN;
				reg2 = SI1145_PARAM_ALSVIS_ADC_COUNTER;
			}
			break;
		default:
			return -EINVAL;
		}

		ret = si1145_param(data, SI1145_CMD_PARAM_SET, reg1, val);
		if (ret < 0)
			return ret;
		/* Set recovery period to one's complement of gain */
		return si1145_param(data, SI1145_CMD_PARAM_SET,
			reg2, (~val & 0x07) << 4);
	case IIO_CHAN_INFO_RAW:
		if (chan->type != IIO_CURRENT)
			return -EINVAL;

		if (val < 0 || val > 15 || val2 != 0)
			return -EINVAL;

		reg1 = SI1145_PS_LED_REG(chan->channel);
		shift = SI1145_PS_LED_SHIFT(chan->channel);
		ret = i2c_smbus_read_byte_data(data->client, reg1);
		if (ret < 0)
			return ret;
		return i2c_smbus_write_byte_data(data->client, reg1,
			(ret & ~(0x0f << shift)) |
			((val & 0x0f) << shift));
	}
	return -EINVAL;
}

#define SI1145_ST { \
	.sign = 'u', \
	.realbits = 16, \
	.storagebits = 16, \
	.endianness = IIO_LE, \
}

#define SI1145_INTENSITY_CHANNEL(_si) { \
	.type = IIO_INTENSITY, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
		BIT(IIO_CHAN_INFO_SCALE), \
	.scan_type = SI1145_ST, \
	.scan_index = _si, \
	.address = SI1145_REG_ALSVIS_DATA, \
}

#define SI1145_INTENSITY_IR_CHANNEL(_si) { \
	.type = IIO_INTENSITY, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
		BIT(IIO_CHAN_INFO_SCALE), \
	.modified = 1, \
	.channel2 = IIO_MOD_LIGHT_IR, \
	.scan_type = SI1145_ST, \
	.scan_index = _si, \
	.address = SI1145_REG_ALSIR_DATA, \
}

#define SI1145_TEMP_CHANNEL(_si) { \
	.type = IIO_TEMP, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
		BIT(IIO_CHAN_INFO_OFFSET) | BIT(IIO_CHAN_INFO_SCALE), \
	.scan_type = SI1145_ST, \
	.scan_index = _si, \
	.address = SI1145_REG_AUX_DATA, \
}

#define SI1145_UV_CHANNEL(_si) { \
	.type = IIO_UVINDEX, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.scan_type = SI1145_ST, \
	.scan_index = _si, \
	.address = SI1145_REG_AUX_DATA, \
}

#define SI1145_PROXIMITY_CHANNEL(_si, _ch) { \
	.type = IIO_PROXIMITY, \
	.indexed = 1, \
	.channel = _ch, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE), \
	.scan_type = SI1145_ST, \
	.scan_index = _si, \
	.address = SI1145_REG_PS1_DATA + _ch * 2, \
}

#define SI1145_VOLTAGE_CHANNEL(_si) { \
	.type = IIO_VOLTAGE, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.scan_type = SI1145_ST, \
	.scan_index = _si, \
	.address = SI1145_REG_AUX_DATA, \
}

#define SI1145_CURRENT_CHANNEL(_ch) { \
	.type = IIO_CURRENT, \
	.indexed = 1, \
	.channel = _ch, \
	.output = 1, \
	.scan_index = -1, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
}

static const struct iio_chan_spec si1132_channels[] = {
	SI1145_INTENSITY_CHANNEL(0),
	SI1145_INTENSITY_IR_CHANNEL(1),
	SI1145_TEMP_CHANNEL(2),
	SI1145_VOLTAGE_CHANNEL(3),
	SI1145_UV_CHANNEL(4),
	IIO_CHAN_SOFT_TIMESTAMP(6),
};

static const struct iio_chan_spec si1141_channels[] = {
	SI1145_INTENSITY_CHANNEL(0),
	SI1145_INTENSITY_IR_CHANNEL(1),
	SI1145_PROXIMITY_CHANNEL(2, 0),
	SI1145_TEMP_CHANNEL(3),
	SI1145_VOLTAGE_CHANNEL(4),
	IIO_CHAN_SOFT_TIMESTAMP(5),
	SI1145_CURRENT_CHANNEL(0),
};

static const struct iio_chan_spec si1142_channels[] = {
	SI1145_INTENSITY_CHANNEL(0),
	SI1145_INTENSITY_IR_CHANNEL(1),
	SI1145_PROXIMITY_CHANNEL(2, 0),
	SI1145_PROXIMITY_CHANNEL(3, 1),
	SI1145_TEMP_CHANNEL(4),
	SI1145_VOLTAGE_CHANNEL(5),
	IIO_CHAN_SOFT_TIMESTAMP(6),
	SI1145_CURRENT_CHANNEL(0),
	SI1145_CURRENT_CHANNEL(1),
};

static const struct iio_chan_spec si1143_channels[] = {
	SI1145_INTENSITY_CHANNEL(0),
	SI1145_INTENSITY_IR_CHANNEL(1),
	SI1145_PROXIMITY_CHANNEL(2, 0),
	SI1145_PROXIMITY_CHANNEL(3, 1),
	SI1145_PROXIMITY_CHANNEL(4, 2),
	SI1145_TEMP_CHANNEL(5),
	SI1145_VOLTAGE_CHANNEL(6),
	IIO_CHAN_SOFT_TIMESTAMP(7),
	SI1145_CURRENT_CHANNEL(0),
	SI1145_CURRENT_CHANNEL(1),
	SI1145_CURRENT_CHANNEL(2),
};

static const struct iio_chan_spec si1145_channels[] = {
	SI1145_INTENSITY_CHANNEL(0),
	SI1145_INTENSITY_IR_CHANNEL(1),
	SI1145_PROXIMITY_CHANNEL(2, 0),
	SI1145_TEMP_CHANNEL(3),
	SI1145_VOLTAGE_CHANNEL(4),
	SI1145_UV_CHANNEL(5),
	IIO_CHAN_SOFT_TIMESTAMP(6),
	SI1145_CURRENT_CHANNEL(0),
};

static const struct iio_chan_spec si1146_channels[] = {
	SI1145_INTENSITY_CHANNEL(0),
	SI1145_INTENSITY_IR_CHANNEL(1),
	SI1145_TEMP_CHANNEL(2),
	SI1145_VOLTAGE_CHANNEL(3),
	SI1145_UV_CHANNEL(4),
	SI1145_PROXIMITY_CHANNEL(5, 0),
	SI1145_PROXIMITY_CHANNEL(6, 1),
	IIO_CHAN_SOFT_TIMESTAMP(7),
	SI1145_CURRENT_CHANNEL(0),
	SI1145_CURRENT_CHANNEL(1),
};

static const struct iio_chan_spec si1147_channels[] = {
	SI1145_INTENSITY_CHANNEL(0),
	SI1145_INTENSITY_IR_CHANNEL(1),
	SI1145_PROXIMITY_CHANNEL(2, 0),
	SI1145_PROXIMITY_CHANNEL(3, 1),
	SI1145_PROXIMITY_CHANNEL(4, 2),
	SI1145_TEMP_CHANNEL(5),
	SI1145_VOLTAGE_CHANNEL(6),
	SI1145_UV_CHANNEL(7),
	IIO_CHAN_SOFT_TIMESTAMP(8),
	SI1145_CURRENT_CHANNEL(0),
	SI1145_CURRENT_CHANNEL(1),
	SI1145_CURRENT_CHANNEL(2),
};

#define SI1145_PART(id, chans, leds, new) \
	{id, chans, ARRAY_SIZE(chans), leds, new}

static const struct si1145_part_info si1145_part_info[] = {
	[SI1132] = SI1145_PART(0x32, si1132_channels, 0, true),
	[SI1141] = SI1145_PART(0x41, si1141_channels, 1, false),
	[SI1142] = SI1145_PART(0x42, si1142_channels, 2, false),
	[SI1143] = SI1145_PART(0x43, si1143_channels, 3, false),
	[SI1145] = SI1145_PART(0x45, si1145_channels, 1, true),
	[SI1146] = SI1145_PART(0x46, si1146_channels, 2, true),
	[SI1147] = SI1145_PART(0x47, si1147_channels, 3, true),
};

static int si1145_set_meas_rate(struct si1145_data *data, int interval)
{
	/* newer parts use a 16-bit register instead of compression */
	if (data->part_info->new)
		return i2c_smbus_write_word_data(data->client,
			SI1145_REG_MEAS_RATE, 0);
	else
		return i2c_smbus_write_byte_data(data->client,
			SI1145_REG_MEAS_RATE, 0);
}

static int si1145_initialize(struct si1145_data *data)
{
	struct i2c_client *client = data->client;
	int ret;

	ret = i2c_smbus_write_byte_data(client, SI1145_REG_COMMAND,
		SI1145_CMD_RESET);
	if (ret < 0)
		return ret;
	msleep(20);

	/* Hardware key, magic value */
	ret = i2c_smbus_write_byte_data(client, SI1145_REG_HW_KEY, 0x17);
	if (ret < 0)
		return ret;
	msleep(20);

	/* Turn off autonomous mode */
	ret = si1145_set_meas_rate(data, 0);
	if (ret < 0)
		return ret;

	/* Set LED currents to 45 mA */
	switch (data->part_info->num_leds) {
	case 1:
		ret = i2c_smbus_write_byte_data(client,
			SI1145_REG_PS_LED21, 0x03);
		break;
	case 2:
		ret = i2c_smbus_write_byte_data(client,
			SI1145_REG_PS_LED21, 0x43);
		break;
	case 3:
		ret = i2c_smbus_write_byte_data(client,
			SI1145_REG_PS_LED3, 0x03);
		if (ret < 0)
			return ret;
		ret = i2c_smbus_write_byte_data(client,
			SI1145_REG_PS_LED21, 0x43);
		break;
	default:
		ret = 0;
		break;
	}
	if (ret < 0)
		return ret;

	/* Set normal proximity measurement mode */
	ret = si1145_param(data, SI1145_CMD_PARAM_SET,
		SI1145_PARAM_PS_ADC_MISC, 0x04);
	if (ret < 0)
		return ret;

	ret = si1145_param(data, SI1145_CMD_PARAM_SET,
		SI1145_PARAM_PS_ADC_GAIN, 0x01);
	if (ret < 0)
		return ret;

	ret = si1145_param(data, SI1145_CMD_PARAM_SET,
		SI1145_PARAM_PS_ADC_COUNTER, 0x03 << 4);
	if (ret < 0)
		return ret;

	/* Set ALS visible measurement mode */
	ret = si1145_param(data, SI1145_CMD_PARAM_SET,
		SI1145_PARAM_ALSVIS_ADC_MISC, SI1145_ADC_MISC_RANGE);
	if (ret < 0)
		return ret;

	ret = si1145_param(data, SI1145_CMD_PARAM_SET,
		SI1145_PARAM_ALSVIS_ADC_GAIN, 0x03);
	if (ret < 0)
		return ret;

	ret = si1145_param(data, SI1145_CMD_PARAM_SET,
		SI1145_PARAM_ALSVIS_ADC_COUNTER, 0x04 << 4);
	if (ret < 0)
		return ret;

	/* Set ALS IR measurement mode */
	ret = si1145_param(data, SI1145_CMD_PARAM_SET,
		SI1145_PARAM_ALSIR_ADC_MISC, SI1145_ADC_MISC_RANGE);
	if (ret < 0)
		return ret;

	ret = si1145_param(data, SI1145_CMD_PARAM_SET,
		SI1145_PARAM_ALSIR_ADC_GAIN, 0x01);
	if (ret < 0)
		return ret;

	ret = si1145_param(data, SI1145_CMD_PARAM_SET,
		SI1145_PARAM_ALSIR_ADC_COUNTER, 0x00 << 4);
	if (ret < 0)
		return ret;

	return 0;
}

static const struct iio_info si1145_info = {
	.read_raw = si1145_read_raw,
	.write_raw = si1145_write_raw,
	.driver_module = THIS_MODULE,
};

static int si1145_buffer_preenable(struct iio_dev *indio_dev)
{
	return si1145_set_chlist(indio_dev, *indio_dev->active_scan_mask);
}

bool si1145_validate_scan_mask(struct iio_dev *indio_dev,
			       const unsigned long *scan_mask)
{
	struct si1145_data *data = iio_priv(indio_dev);
	unsigned int count = 0;
	int i;

	/* Check that at most one AUX channel is enabled */
	for_each_set_bit(i, scan_mask, data->part_info->num_channels) {
		if (indio_dev->channels[i].address == SI1145_REG_AUX_DATA)
			count++;
	}

	return count <= 1;
}

static const struct iio_buffer_setup_ops si1145_buffer_setup_ops = {
	.preenable = si1145_buffer_preenable,
	.postenable = iio_triggered_buffer_postenable,
	.predisable = iio_triggered_buffer_predisable,
	.validate_scan_mask = si1145_validate_scan_mask,
};

static int si1145_probe(struct i2c_client *client,
					const struct i2c_device_id *id)
{
	struct si1145_data *data;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	data->client = client;
	data->part_info = &si1145_part_info[id->driver_data];

	ret = i2c_smbus_read_byte_data(data->client, SI1145_REG_PART_ID);
	if (ret < 0)
		return ret;
	if (ret != data->part_info->part)
		return -ENODEV;

	indio_dev->dev.parent = &client->dev;
	indio_dev->name = id->name;
	indio_dev->channels = data->part_info->channels;
	indio_dev->num_channels = data->part_info->num_channels;
	indio_dev->info = &si1145_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	mutex_init(&data->lock);

	ret = si1145_initialize(data);
	if (ret < 0)
		return ret;

	ret = iio_triggered_buffer_setup(indio_dev, NULL,
		si1145_trigger_handler, &si1145_buffer_setup_ops);
	if (ret < 0)
		return ret;

	ret = iio_device_register(indio_dev);
	if (ret < 0)
		goto error_free_buffer;

	return 0;

error_free_buffer:
	iio_triggered_buffer_cleanup(indio_dev);
	return ret;
}

static const struct i2c_device_id si1145_ids[] = {
	{ "si1132", SI1132 },
	{ "si1141", SI1141 },
	{ "si1142", SI1142 },
	{ "si1143", SI1143 },
	{ "si1145", SI1145 },
	{ "si1146", SI1146 },
	{ "si1147", SI1147 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, si1145_ids);

static int si1145_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);

	iio_device_unregister(indio_dev);
	iio_triggered_buffer_cleanup(indio_dev);

	return 0;
}

static struct i2c_driver si1145_driver = {
	.driver = {
		.name   = "si1145",
		.owner  = THIS_MODULE,
	},
	.probe  = si1145_probe,
	.remove = si1145_remove,
	.id_table = si1145_ids,
};

module_i2c_driver(si1145_driver);

MODULE_AUTHOR("Peter Meerwald-Stadler <pmeerw@pmeerw.net>");
MODULE_DESCRIPTION("Silabs SI1132 and SI1141/2/3/5/6/7 proximity, ambient light and UV index sensor driver");
MODULE_LICENSE("GPL");
