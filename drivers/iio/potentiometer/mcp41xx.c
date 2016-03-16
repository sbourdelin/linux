/*
 * Industrial I/O driver for MCP414X/416X/424X/426X Digital POTs
 *
 * Datasheet: http://ww1.microchip.com/downloads/en/DeviceDoc/22059a.pdf
 *
 * Copyright (c) 2016 Slawomir Stepien
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/err.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define MCP41XX_WRITE		(0x00 << 2)
#define MCP41XX_INCR		(0x01 << 2)
#define MCP41XX_DECR		(0x02 << 2)
#define MCP41XX_READ		(0x03 << 2)

#define MCP41XX_FULL_SCALE(t)	(t[0] == 0xFF ? 256 : t[1])
#define MCP41XX_9BIT_VALUE(t)	((t[1] + (t[0] << 8)) & 0x1FF)

#define MCP41XX_MIN_ADDR	0x00
#define MCP41XX_MAX_ADDR	0x0F

#define MCP41XX_NV_OFFSET	0x02

#define MCP41XX_TCON_ADDR	0x04
#define MCP41XX_STATUS_ADDR	0x05

struct mcp41xx_cfg {
	unsigned long int devid;
	unsigned int wipers;
	unsigned int num_pos;
	unsigned int kohms;
};

enum mcp41xx_type {
	MCP413x_502,
	MCP413x_103,
	MCP413x_503,
	MCP413x_104,
	MCP414x_502,
	MCP414x_103,
	MCP414x_503,
	MCP414x_104,
	MCP415x_502,
	MCP415x_103,
	MCP415x_503,
	MCP415x_104,
	MCP416x_502,
	MCP416x_103,
	MCP416x_503,
	MCP416x_104,
	MCP423x_502,
	MCP423x_103,
	MCP423x_503,
	MCP423x_104,
	MCP424x_502,
	MCP424x_103,
	MCP424x_503,
	MCP424x_104,
	MCP425x_502,
	MCP425x_103,
	MCP425x_503,
	MCP425x_104,
	MCP426x_502,
	MCP426x_103,
	MCP426x_503,
	MCP426x_104,
};

static const struct mcp41xx_cfg mcp41xx_cfg[] = {
	[MCP413x_502] =	{ .wipers = 1, .num_pos = 129, .kohms =   5 },
	[MCP413x_103] = { .wipers = 1, .num_pos = 129, .kohms =  10 },
	[MCP413x_503] =	{ .wipers = 1, .num_pos = 129, .kohms =  50 },
	[MCP413x_104] =	{ .wipers = 1, .num_pos = 129, .kohms = 100 },
	[MCP414x_502] =	{ .wipers = 1, .num_pos = 129, .kohms =   5 },
	[MCP414x_103] =	{ .wipers = 1, .num_pos = 129, .kohms =  10 },
	[MCP414x_503] =	{ .wipers = 1, .num_pos = 129, .kohms =  50 },
	[MCP414x_104] =	{ .wipers = 1, .num_pos = 129, .kohms = 100 },
	[MCP415x_502] =	{ .wipers = 1, .num_pos = 257, .kohms =   5 },
	[MCP415x_103] =	{ .wipers = 1, .num_pos = 257, .kohms =  10 },
	[MCP415x_503] =	{ .wipers = 1, .num_pos = 257, .kohms =  50 },
	[MCP415x_104] =	{ .wipers = 1, .num_pos = 257, .kohms = 100 },
	[MCP416x_502] =	{ .wipers = 1, .num_pos = 257, .kohms =   5 },
	[MCP416x_103] =	{ .wipers = 1, .num_pos = 257, .kohms =  10 },
	[MCP416x_503] =	{ .wipers = 1, .num_pos = 257, .kohms =  50 },
	[MCP416x_104] =	{ .wipers = 1, .num_pos = 257, .kohms = 100 },
	[MCP423x_502] =	{ .wipers = 2, .num_pos = 129, .kohms =   5 },
	[MCP423x_103] =	{ .wipers = 2, .num_pos = 129, .kohms =  10 },
	[MCP423x_503] =	{ .wipers = 2, .num_pos = 129, .kohms =  50 },
	[MCP423x_104] =	{ .wipers = 2, .num_pos = 129, .kohms = 100 },
	[MCP424x_502] =	{ .wipers = 2, .num_pos = 129, .kohms =   5 },
	[MCP424x_103] =	{ .wipers = 2, .num_pos = 129, .kohms =  10 },
	[MCP424x_503] =	{ .wipers = 2, .num_pos = 129, .kohms =  50 },
	[MCP424x_104] =	{ .wipers = 2, .num_pos = 129, .kohms = 100 },
	[MCP425x_502] =	{ .wipers = 2, .num_pos = 257, .kohms =   5 },
	[MCP425x_103] =	{ .wipers = 2, .num_pos = 257, .kohms =  10 },
	[MCP425x_503] =	{ .wipers = 2, .num_pos = 257, .kohms =  50 },
	[MCP425x_104] =	{ .wipers = 2, .num_pos = 257, .kohms = 100 },
	[MCP426x_502] =	{ .wipers = 2, .num_pos = 257, .kohms =   5 },
	[MCP426x_103] =	{ .wipers = 2, .num_pos = 257, .kohms =  10 },
	[MCP426x_503] =	{ .wipers = 2, .num_pos = 257, .kohms =  50 },
	[MCP426x_104] =	{ .wipers = 2, .num_pos = 257, .kohms = 100 },
};

struct mcp41xx_data {
	struct spi_device *spi;
	struct mutex lock;
	unsigned long devid;
	u8 tx[2], rx[2];
	struct spi_transfer xfer;
	struct spi_message msg;
};

#define MCP41XX_CHANNEL(ch) {					\
	.type = IIO_RESISTANCE,					\
	.output = 1,						\
	.indexed = 1,						\
	.channel = (ch),					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
}

static const struct iio_chan_spec mcp41xx_channels[] = {
	MCP41XX_CHANNEL(0),
	MCP41XX_CHANNEL(1),
};

/* Function based on: arch/parisc/kernel/traps.c */
static int printbinary(char *buf, unsigned long x, int nbits)
{
	unsigned long mask = 1UL << (nbits - 2);

	while (mask != 0) {
		*buf++ = (mask & x ? '1' : '0');
		mask >>= 1;
	}
	*buf++ = '\n';
	*buf = '\0';

	return nbits;
}

static int mcp41xx_exec(struct mcp41xx_data *data,
		u8 addr, u8 cmd,
		int *trans, size_t n)
{
	int err;
	struct spi_device *spi = data->spi;

	/* Two bytes are needed for the response */
	if (n != 2)
		return -EINVAL;

	switch (cmd) {
	case MCP41XX_READ:
		data->xfer.len = 2; /* Two bytes transfer for this command */
		data->tx[0] = (addr << 4) | MCP41XX_READ;
		data->tx[1] = 0;
		break;

	case MCP41XX_WRITE:
		data->xfer.len = 2;
		/* Shift trans[0] to see if this is full scale value write */
		data->tx[0] = (addr << 4) | MCP41XX_WRITE | (trans[0] >> 8);
		data->tx[1] = (trans[0]) & 0xFF; /* 8 bits here */
		break;

	case MCP41XX_INCR:
		data->xfer.len = 1; /* One byte transfer for this command */
		data->tx[0] = (addr << 4) | MCP41XX_INCR;
		break;

	case MCP41XX_DECR:
		data->xfer.len = 1;
		data->tx[0] = (addr << 4) | MCP41XX_DECR;
		break;

	default:
		return -EINVAL;
	}

	dev_dbg(&spi->dev, "mcp41xx_exec: tx0: 0x%x tx1: 0x%x\n",
			data->tx[0], data->tx[1]);

	mutex_lock(&data->lock);
	spi_message_init(&data->msg);
	spi_message_add_tail(&data->xfer, &data->msg);

	err = spi_sync(spi, &data->msg);
	if (err) {
		dev_err(&spi->dev, "spi_sync(): %d\n", err);
		mutex_unlock(&data->lock);
		return err;
	}
	mutex_unlock(&data->lock);

	dev_dbg(&spi->dev, "mcp41xx_exec: rx0: 0x%x rx1: 0x%x\n",
			data->rx[0], data->rx[1]);

	/* Copy back the response */
	trans[0] = data->rx[0];
	trans[1] = data->rx[1];

	return 0;
}

static ssize_t mcp41xx_show_memory_map(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	ssize_t i = 0;
	int err;
	int addr;
	int trans[2];
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct mcp41xx_data *data = iio_priv(indio_dev);

	/* Read the whole memory */
	for (addr = MCP41XX_MIN_ADDR; addr <= MCP41XX_MAX_ADDR; addr++) {
		err = mcp41xx_exec(data,
				addr, MCP41XX_READ,
				trans, ARRAY_SIZE(trans));
		if (err)
			return i;

		/* First column is address, second is value */
		i += scnprintf(&buf[i],
				12, "0x%02x 0x%03x\n",
				addr, MCP41XX_9BIT_VALUE(trans));
	}

	return i;
}

static ssize_t mcp41xx_store_memory_map(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t len)
{
	int err;
	int addr;
	int trans[2];
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct mcp41xx_data *data = iio_priv(indio_dev);

	err = sscanf(buf, "0x%x 0x%x\n", &addr, &trans[0]);
	if (err != 2)
		return err;

	err = mcp41xx_exec(data, addr, MCP41XX_WRITE, trans, ARRAY_SIZE(trans));
	if (err)
		return err;

	return len;
}

static IIO_DEVICE_ATTR(memory_map, S_IRUGO | S_IWUSR,
		mcp41xx_show_memory_map,
		mcp41xx_store_memory_map,
		0);

static ssize_t mcp41xx_show_nv_wiper(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int err;
	int trans[2];
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct mcp41xx_data *data = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);

	err = mcp41xx_exec(data,
			this_attr->address + MCP41XX_NV_OFFSET, MCP41XX_READ,
			trans, ARRAY_SIZE(trans));
	if (err)
		return -EINVAL;

	return sprintf(buf, "%d\n", MCP41XX_FULL_SCALE(trans));
}

static ssize_t mcp41xx_store_nv_wiper(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t len)
{
	int err;
	int trans[2];
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct mcp41xx_data *data = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);

	err = kstrtoint(buf, 10, &trans[0]);
	if (err)
		return -EINVAL;

	err = mcp41xx_exec(data,
			this_attr->address + MCP41XX_NV_OFFSET, MCP41XX_WRITE,
			trans, ARRAY_SIZE(trans));
	if (err)
		return -EINVAL;

	return len;
}

#define MCP41XX_NV_WIPER(n) IIO_DEVICE_ATTR(nv_wiper##n, S_IRUGO | S_IWUSR, \
		mcp41xx_show_nv_wiper, mcp41xx_store_nv_wiper, n)

MCP41XX_NV_WIPER(0);
MCP41XX_NV_WIPER(1);

static ssize_t mcp41xx_store_incr_wiper(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t len)
{
	int err;
	int trans[2];
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct mcp41xx_data *data = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);

	err = mcp41xx_exec(data,
			this_attr->address, MCP41XX_INCR,
			trans, ARRAY_SIZE(trans));
	if (err)
		return -EINVAL;

	return len;
}

static ssize_t mcp41xx_store_decr_wiper(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t len)
{
	int err;
	int trans[2];
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct mcp41xx_data *data = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);

	err = mcp41xx_exec(data,
			this_attr->address, MCP41XX_DECR,
			trans, ARRAY_SIZE(trans));
	if (err)
		return -EINVAL;

	return len;
}

#define MCP41XX_INCR_ATTR(n) IIO_DEVICE_ATTR(incr_wiper##n, S_IWUSR,	\
		NULL, mcp41xx_store_incr_wiper, n)

MCP41XX_INCR_ATTR(0);
MCP41XX_INCR_ATTR(1);

#define MCP41XX_DECR_ATTR(n) IIO_DEVICE_ATTR(decr_wiper##n, S_IWUSR,	\
		NULL, mcp41xx_store_decr_wiper, n)

MCP41XX_DECR_ATTR(0);
MCP41XX_DECR_ATTR(1);

static ssize_t mcp41xx_show_status_register(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int err;
	int trans[2];
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct mcp41xx_data *data = iio_priv(indio_dev);
	unsigned long val;

	err = mcp41xx_exec(data,
			MCP41XX_STATUS_ADDR, MCP41XX_READ,
			trans, ARRAY_SIZE(trans));
	if (err)
		return -EINVAL;

	val = MCP41XX_9BIT_VALUE(trans);
	return printbinary(buf, val, 10); /* One extra for new line */
}

static IIO_DEVICE_ATTR(status_register, S_IRUGO,
		mcp41xx_show_status_register,
		NULL,
		0);

static ssize_t mcp41xx_show_tcon_register(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int err;
	int trans[2];
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct mcp41xx_data *data = iio_priv(indio_dev);
	unsigned long val;

	err = mcp41xx_exec(data,
			MCP41XX_TCON_ADDR, MCP41XX_READ,
			trans, ARRAY_SIZE(trans));
	if (err)
		return -EINVAL;

	val = MCP41XX_9BIT_VALUE(trans);
	return printbinary(buf, val, 10); /* One extra for new line */
}

static ssize_t mcp41xx_store_tcon_register(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t len)
{
	int err;
	int trans[2];
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct mcp41xx_data *data = iio_priv(indio_dev);

	err = kstrtoint(buf, 2, &trans[0]);
	if (err)
		return -EINVAL;

	err = mcp41xx_exec(data,
			MCP41XX_TCON_ADDR, MCP41XX_WRITE,
			trans, ARRAY_SIZE(trans));
	if (err)
		return -EINVAL;

	return len;
}

static IIO_DEVICE_ATTR(tcon_register, S_IRUGO | S_IWUSR,
		mcp41xx_show_tcon_register,
		mcp41xx_store_tcon_register,
		0);

/* All available attributes that can be created */
static struct iio_dev_attr *mcp41xx_all_attrs[] = {
	&iio_dev_attr_memory_map,
	&iio_dev_attr_nv_wiper0,
	&iio_dev_attr_nv_wiper1,
	&iio_dev_attr_incr_wiper0,
	&iio_dev_attr_incr_wiper1,
	&iio_dev_attr_decr_wiper0,
	&iio_dev_attr_decr_wiper1,
	&iio_dev_attr_status_register,
	&iio_dev_attr_tcon_register,
};

/* This will be filled dynamically in probe() */
static struct attribute **mcp41xx_attributes;
static struct attribute_group mcp41xx_attribute_group;

static int mcp41xx_read_raw(struct iio_dev *indio_dev,
		struct iio_chan_spec const *chan,
		int *val, int *val2, long mask)
{
	int err;
	int trans[2];
	struct mcp41xx_data *data = iio_priv(indio_dev);
	int address = chan->channel;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		err = mcp41xx_exec(data,
				address, MCP41XX_READ,
				trans, ARRAY_SIZE(trans));
		if (err)
			return err;

		*val = MCP41XX_FULL_SCALE(trans);

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = 1000 * mcp41xx_cfg[data->devid].kohms;
		*val2 = mcp41xx_cfg[data->devid].num_pos;

		return IIO_VAL_FRACTIONAL;
	}

	return -EINVAL;
}

static int mcp41xx_write_raw(struct iio_dev *indio_dev,
		struct iio_chan_spec const *chan,
		int val, int val2, long mask)
{
	int trans[2];
	struct mcp41xx_data *data = iio_priv(indio_dev);
	int address = chan->channel;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		trans[0] = val;

		return mcp41xx_exec(data,
				address, MCP41XX_WRITE,
				trans, ARRAY_SIZE(trans));
	}

	return -EINVAL;
}

static const struct iio_info mcp41xx_info = {
	.attrs = &mcp41xx_attribute_group,
	.read_raw = mcp41xx_read_raw,
	.write_raw = mcp41xx_write_raw,
	.driver_module = THIS_MODULE,
};

static int mcp41xx_probe(struct spi_device *spi)
{
	int l, i, addr;
	int err;
	size_t attrs_count;
	struct iio_dev *indio_dev;
	struct mcp41xx_data *data;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->spi = spi;
	data->devid = spi_get_device_id(spi)->driver_data;
	spi_set_drvdata(spi, indio_dev);

	data->xfer.tx_buf = data->tx;
	data->xfer.rx_buf = data->rx;

	mutex_init(&data->lock);

	indio_dev->dev.parent = &spi->dev;
	indio_dev->info = &mcp41xx_info;
	indio_dev->channels = mcp41xx_channels;
	indio_dev->num_channels = mcp41xx_cfg[data->devid].wipers;
	indio_dev->name = spi_get_device_id(spi)->name;

	/* Dynamic attributes */
	attrs_count = ARRAY_SIZE(mcp41xx_all_attrs);
	mcp41xx_attributes = kzalloc(
			sizeof(struct attribute *) * (attrs_count + 1),
			GFP_KERNEL);
	if (mcp41xx_attributes == NULL)
		return -ENOMEM;

	i = 0;
	for (l = 0; l < attrs_count; l++) {
		/* Attrs with address == 0 or address == wipers are added */
		addr = mcp41xx_all_attrs[l]->address;
		if (addr == 0 || addr == indio_dev->num_channels - 1) {
			mcp41xx_attributes[i] =
				&mcp41xx_all_attrs[l]->dev_attr.attr;
			i++;
		}
	}
	mcp41xx_attribute_group.attrs = mcp41xx_attributes;

	err = devm_iio_device_register(&spi->dev, indio_dev);
	if (err) {
		dev_info(&spi->dev, "Unable to register %s", indio_dev->name);
		return err;
	}

	dev_info(&spi->dev, "Registered %s", indio_dev->name);

	return 0;
}

static int mcp41xx_remove(struct spi_device *spi)
{
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct mcp41xx_data *data = iio_priv(indio_dev);

	mutex_destroy(&data->lock);
	devm_iio_device_unregister(&spi->dev, indio_dev);
	kfree(mcp41xx_attributes);

	dev_info(&spi->dev, "Unregistered %s", indio_dev->name);

	return 0;
}

#if defined CONFIG_OF
static const struct of_device_id mcp41xx_dt_ids[] = {
	{ .compatible = "microchip,mcp4113x-502",
		.data = &mcp41xx_cfg[MCP413x_502] },
	{ .compatible = "microchip,mcp4113x-103",
		.data = &mcp41xx_cfg[MCP413x_103] },
	{ .compatible = "microchip,mcp4113x-503",
		.data = &mcp41xx_cfg[MCP413x_503] },
	{ .compatible = "microchip,mcp4113x-104",
		.data = &mcp41xx_cfg[MCP413x_104] },
	{ .compatible = "microchip,mcp4114x-502",
		.data = &mcp41xx_cfg[MCP414x_502] },
	{ .compatible = "microchip,mcp4114x-103",
		.data = &mcp41xx_cfg[MCP414x_103] },
	{ .compatible = "microchip,mcp4114x-503",
		.data = &mcp41xx_cfg[MCP414x_503] },
	{ .compatible = "microchip,mcp4114x-104",
		.data = &mcp41xx_cfg[MCP414x_104] },
	{ .compatible = "microchip,mcp4115x-502",
		.data = &mcp41xx_cfg[MCP415x_502] },
	{ .compatible = "microchip,mcp4115x-103",
		.data = &mcp41xx_cfg[MCP415x_103] },
	{ .compatible = "microchip,mcp4115x-503",
		.data = &mcp41xx_cfg[MCP415x_503] },
	{ .compatible = "microchip,mcp4115x-104",
		.data = &mcp41xx_cfg[MCP415x_104] },
	{ .compatible = "microchip,mcp4116x-502",
		.data = &mcp41xx_cfg[MCP416x_502] },
	{ .compatible = "microchip,mcp4116x-103",
		.data = &mcp41xx_cfg[MCP416x_103] },
	{ .compatible = "microchip,mcp4116x-503",
		.data = &mcp41xx_cfg[MCP416x_503] },
	{ .compatible = "microchip,mcp4116x-104",
		.data = &mcp41xx_cfg[MCP416x_104] },
	{ .compatible = "microchip,mcp4123x-502",
		.data = &mcp41xx_cfg[MCP423x_502] },
	{ .compatible = "microchip,mcp4123x-103",
		.data = &mcp41xx_cfg[MCP423x_103] },
	{ .compatible = "microchip,mcp4123x-503",
		.data = &mcp41xx_cfg[MCP423x_503] },
	{ .compatible = "microchip,mcp4123x-104",
		.data = &mcp41xx_cfg[MCP423x_104] },
	{ .compatible = "microchip,mcp4124x-502",
		.data = &mcp41xx_cfg[MCP424x_502] },
	{ .compatible = "microchip,mcp4124x-103",
		.data = &mcp41xx_cfg[MCP424x_103] },
	{ .compatible = "microchip,mcp4124x-503",
		.data = &mcp41xx_cfg[MCP424x_503] },
	{ .compatible = "microchip,mcp4124x-104",
		.data = &mcp41xx_cfg[MCP424x_104] },
	{ .compatible = "microchip,mcp4125x-502",
		.data = &mcp41xx_cfg[MCP425x_502] },
	{ .compatible = "microchip,mcp4125x-103",
		.data = &mcp41xx_cfg[MCP425x_103] },
	{ .compatible = "microchip,mcp4125x-503",
		.data = &mcp41xx_cfg[MCP425x_503] },
	{ .compatible = "microchip,mcp4125x-104",
		.data = &mcp41xx_cfg[MCP425x_104] },
	{ .compatible = "microchip,mcp4126x-502",
		.data = &mcp41xx_cfg[MCP426x_502] },
	{ .compatible = "microchip,mcp4126x-103",
		.data = &mcp41xx_cfg[MCP426x_103] },
	{ .compatible = "microchip,mcp4126x-503",
		.data = &mcp41xx_cfg[MCP426x_503] },
	{ .compatible = "microchip,mcp4126x-104",
		.data = &mcp41xx_cfg[MCP426x_104] },
	{}
};
MODULE_DEVICE_TABLE(of, mcp41xx_dt_ids);
#endif

static const struct spi_device_id mcp41xx_id[] = {
	{ "mcp413x-502", MCP413x_502 },
	{ "mcp413x-103", MCP413x_103 },
	{ "mcp413x-503", MCP413x_503 },
	{ "mcp413x-104", MCP413x_104 },
	{ "mcp414x-502", MCP414x_502 },
	{ "mcp414x-103", MCP414x_103 },
	{ "mcp414x-503", MCP414x_503 },
	{ "mcp414x-104", MCP414x_104 },
	{ "mcp415x-502", MCP415x_502 },
	{ "mcp415x-103", MCP415x_103 },
	{ "mcp415x-503", MCP415x_503 },
	{ "mcp415x-104", MCP415x_104 },
	{ "mcp416x-502", MCP416x_502 },
	{ "mcp416x-103", MCP416x_103 },
	{ "mcp416x-503", MCP416x_503 },
	{ "mcp416x-104", MCP416x_104 },
	{ "mcp423x-502", MCP423x_502 },
	{ "mcp423x-103", MCP423x_103 },
	{ "mcp423x-503", MCP423x_503 },
	{ "mcp423x-104", MCP423x_104 },
	{ "mcp424x-502", MCP424x_502 },
	{ "mcp424x-103", MCP424x_103 },
	{ "mcp424x-503", MCP424x_503 },
	{ "mcp424x-104", MCP424x_104 },
	{ "mcp425x-502", MCP425x_502 },
	{ "mcp425x-103", MCP425x_103 },
	{ "mcp425x-503", MCP425x_503 },
	{ "mcp425x-104", MCP425x_104 },
	{ "mcp426x-502", MCP426x_502 },
	{ "mcp426x-103", MCP426x_103 },
	{ "mcp426x-503", MCP426x_503 },
	{ "mcp426x-104", MCP426x_104 },
	{}
};
MODULE_DEVICE_TABLE(spi, mcp41xx_id);

static struct spi_driver mcp41xx_driver = {
	.driver = {
		.name		= "mcp41xx",
		.of_match_table = of_match_ptr(mcp41xx_dt_ids),
	},
	.probe		= mcp41xx_probe,
	.remove		= mcp41xx_remove,
	.id_table	= mcp41xx_id,
};

module_spi_driver(mcp41xx_driver);

MODULE_AUTHOR("Slawomir Stepien <sst@poczta.fm>");
MODULE_DESCRIPTION("MCP41XX digital potentiometer");
MODULE_LICENSE("GPL v2");
