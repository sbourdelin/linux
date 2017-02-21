/* ir-lirc-codec.c - rc-core to classic lirc interface bridge
 *
 * Copyright (C) 2010 by Jarod Wilson <jarod@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <linux/sched.h>
#include <linux/wait.h>
#include <media/lirc.h>
#include <media/lirc_dev.h>
#include <media/rc-core.h>
#include "rc-core-priv.h"

/**
 * ir_lirc_raw_event() - Send raw IR data to lirc to be relayed to userspace
 *
 * @input_dev:	the struct rc_dev descriptor of the device
 * @duration:	the struct ir_raw_event descriptor of the pulse/space
 *
 * This function returns -EINVAL if the lirc interfaces aren't wired up.
 */
int ir_lirc_raw_event(struct rc_dev *dev, struct ir_raw_event ev)
{
	struct lirc_codec *lirc = &dev->raw->lirc;
	unsigned int sample;

	/* Packet start */
	if (ev.reset) {
		/* Userspace expects a long space event before the start of
		 * the signal to use as a sync.  This may be done with repeat
		 * packets and normal samples.  But if a reset has been sent
		 * then we assume that a long time has passed, so we send a
		 * space with the maximum time value. */
		sample = LIRC_SPACE(LIRC_VALUE_MASK);
		IR_dprintk(2, "delivering reset sync space to lirc_dev\n");

	/* Carrier reports */
	} else if (ev.carrier_report) {
		sample = LIRC_FREQUENCY(ev.carrier);
		IR_dprintk(2, "carrier report (freq: %d)\n", sample);

	/* Packet end */
	} else if (ev.timeout) {

		if (lirc->gap)
			return 0;

		lirc->gap_start = ktime_get();
		lirc->gap = true;
		lirc->gap_duration = ev.duration;

		if (!lirc->send_timeout_reports)
			return 0;

		sample = LIRC_TIMEOUT(ev.duration / 1000);
		IR_dprintk(2, "timeout report (duration: %d)\n", sample);

	/* Normal sample */
	} else {
		if (lirc->gap) {
			lirc->gap_duration += ktime_to_ns(ktime_sub(ktime_get(),
				lirc->gap_start));

			/* Convert to ms and cap by LIRC_VALUE_MASK */
			do_div(lirc->gap_duration, 1000);
			lirc->gap_duration = min(lirc->gap_duration,
							(u64)LIRC_VALUE_MASK);

			kfifo_put(&lirc->kfifo, LIRC_SPACE(lirc->gap_duration));
			lirc->gap = false;
		}

		sample = ev.pulse ? LIRC_PULSE(ev.duration / 1000) :
					LIRC_SPACE(ev.duration / 1000);
		IR_dprintk(2, "delivering %uus %s to lirc_dev\n",
			   TO_US(ev.duration), TO_STR(ev.pulse));
	}

	kfifo_put(&lirc->kfifo, sample);
	wake_up_poll(&lirc->wait_poll, POLLIN);

	return 0;
}

static ssize_t ir_lirc_transmit_ir(struct file *file, const char __user *buf,
				   size_t n, loff_t *ppos)
{
	struct lirc_codec *lirc;
	struct rc_dev *dev;
	unsigned int *txbuf = NULL; /* buffer with values to transmit */
	struct ir_raw_event *raw = NULL;
	ssize_t ret = -EINVAL;
	size_t count;
	ktime_t start;
	s64 towait;
	unsigned int duration = 0; /* signal duration in us */
	int i;

	start = ktime_get();

	lirc = lirc_get_pdata(file);
	if (!lirc)
		return -EFAULT;

	dev = lirc->dev;
	if (!dev)
		return -EFAULT;

	if (!dev->tx_ir)
		return -EINVAL;

	if (lirc->send_mode == LIRC_MODE_SCANCODE) {
		struct lirc_scancode scan;

		if (n != sizeof(scan))
			return -EINVAL;

		if (copy_from_user(&scan, buf, sizeof(scan)))
			return -EFAULT;

		if (scan.flags)
			return -EINVAL;

		raw = kmalloc_array(LIRCBUF_SIZE, sizeof(*raw), GFP_KERNEL);
		if (!raw)
			return -ENOMEM;

		ret = ir_raw_encode_scancode(scan.rc_type, scan.scancode,
					     raw, LIRCBUF_SIZE);
		if (ret < 0)
			goto out;

		count = ret;

		txbuf = kmalloc_array(count, sizeof(unsigned int), GFP_KERNEL);
		if (!txbuf) {
			ret = -ENOMEM;
			goto out;
		}

		for (i = 0; i < count; i++)
			/* Convert from NS to US */
			txbuf[i] = DIV_ROUND_UP(raw[i].duration, 1000);

		if (dev->s_tx_carrier) {
			int carrier = ir_raw_encode_carrier(scan.rc_type);

			if (carrier > 0)
				dev->s_tx_carrier(dev, carrier);
		}
	} else {
		if (n < sizeof(unsigned int) || n % sizeof(unsigned int))
			return -EINVAL;

		count = n / sizeof(unsigned int);
		if (count > LIRCBUF_SIZE || count % 2 == 0)
			return -EINVAL;

		txbuf = memdup_user(buf, n);
		if (IS_ERR(txbuf))
			return PTR_ERR(txbuf);
	}

	for (i = 0; i < count; i++) {
		if (txbuf[i] > IR_MAX_DURATION / 1000 - duration || !txbuf[i]) {
			ret = -EINVAL;
			goto out;
		}

		duration += txbuf[i];
	}

	ret = dev->tx_ir(dev, txbuf, count);
	if (ret < 0)
		goto out;

	for (duration = i = 0; i < ret; i++)
		duration += txbuf[i];

	if (lirc->send_mode == LIRC_MODE_SCANCODE)
		ret = n;
	else
		ret *= sizeof(unsigned int);

	/*
	 * The lircd gap calculation expects the write function to
	 * wait for the actual IR signal to be transmitted before
	 * returning.
	 */
	towait = ktime_us_delta(ktime_add_us(start, duration), ktime_get());
	if (towait > 0) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(usecs_to_jiffies(towait));
	}

out:
	kfree(txbuf);
	kfree(raw);
	return ret;
}

static long ir_lirc_ioctl(struct file *filep, unsigned int cmd,
			unsigned long arg)
{
	struct lirc_codec *lirc;
	struct rc_dev *dev;
	u32 __user *argp = (u32 __user *)(arg);
	int ret = 0;
	__u32 val = 0, tmp;

	lirc = lirc_get_pdata(filep);
	if (!lirc)
		return -EFAULT;

	dev = lirc->dev;
	if (!dev)
		return -EFAULT;

	if (_IOC_DIR(cmd) & _IOC_WRITE) {
		ret = get_user(val, argp);
		if (ret)
			return ret;
	}

	switch (cmd) {

	/* legacy support */
	case LIRC_GET_SEND_MODE:
		if (!dev->tx_ir)
			return -ENOTTY;

		val = lirc->send_mode;
		break;

	case LIRC_SET_SEND_MODE:
		if (!dev->tx_ir)
			return -ENOTTY;

		if (!(val == LIRC_MODE_PULSE || val == LIRC_MODE_SCANCODE))
			return -EINVAL;

		lirc->send_mode = val;
		return 0;

	/* TX settings */
	case LIRC_SET_TRANSMITTER_MASK:
		if (!dev->s_tx_mask)
			return -ENOTTY;

		return dev->s_tx_mask(dev, val);

	case LIRC_SET_SEND_CARRIER:
		if (!dev->s_tx_carrier)
			return -ENOTTY;

		return dev->s_tx_carrier(dev, val);

	case LIRC_SET_SEND_DUTY_CYCLE:
		if (!dev->s_tx_duty_cycle)
			return -ENOTTY;

		if (val <= 0 || val >= 100)
			return -EINVAL;

		return dev->s_tx_duty_cycle(dev, val);

	/* RX settings */
	case LIRC_SET_REC_CARRIER:
		if (!dev->s_rx_carrier_range)
			return -ENOTTY;

		if (val <= 0)
			return -EINVAL;

		return dev->s_rx_carrier_range(dev,
					       dev->raw->lirc.carrier_low,
					       val);

	case LIRC_SET_REC_CARRIER_RANGE:
		if (!dev->s_rx_carrier_range)
			return -ENOTTY;

		if (val <= 0)
			return -EINVAL;

		dev->raw->lirc.carrier_low = val;
		return 0;

	case LIRC_GET_REC_RESOLUTION:
		if (!dev->rx_resolution)
			return -ENOTTY;

		val = dev->rx_resolution;
		break;

	case LIRC_SET_WIDEBAND_RECEIVER:
		if (!dev->s_learning_mode)
			return -ENOTTY;

		return dev->s_learning_mode(dev, !!val);

	case LIRC_SET_MEASURE_CARRIER_MODE:
		if (!dev->s_carrier_report)
			return -ENOTTY;

		return dev->s_carrier_report(dev, !!val);

	/* Generic timeout support */
	case LIRC_GET_MIN_TIMEOUT:
		if (!dev->max_timeout)
			return -ENOTTY;
		val = DIV_ROUND_UP(dev->min_timeout, 1000);
		break;

	case LIRC_GET_MAX_TIMEOUT:
		if (!dev->max_timeout)
			return -ENOTTY;
		val = dev->max_timeout / 1000;
		break;

	case LIRC_SET_REC_TIMEOUT:
		if (!dev->max_timeout)
			return -ENOTTY;

		tmp = val * 1000;

		if (tmp < dev->min_timeout ||
		    tmp > dev->max_timeout)
				return -EINVAL;

		if (dev->s_timeout)
			ret = dev->s_timeout(dev, tmp);
		if (!ret)
			dev->timeout = tmp;
		break;

	case LIRC_SET_REC_TIMEOUT_REPORTS:
		if (!dev->timeout)
			return -ENOTTY;

		lirc->send_timeout_reports = !!val;
		break;

	default:
		return lirc_dev_fop_ioctl(filep, cmd, arg);
	}

	if (_IOC_DIR(cmd) & _IOC_READ)
		ret = put_user(val, argp);

	return ret;
}

static unsigned int ir_lirc_poll(struct file *filep,
				 struct poll_table_struct *wait)
{
	struct lirc_codec *lirc = lirc_get_pdata(filep);
	unsigned int events = 0;

	poll_wait(filep, &lirc->wait_poll, wait);

	if (!lirc->drv->attached)
		events = POLLERR;
	else if (!kfifo_is_empty(&lirc->kfifo))
		events = POLLIN | POLLRDNORM;

	return events;
}

static ssize_t ir_lirc_read(struct file *filep, char __user *buffer,
			    size_t length, loff_t *ppos)
{
	struct lirc_codec *lirc = lirc_get_pdata(filep);
	unsigned int copied;
	int ret;

	if (length % sizeof(unsigned int))
		return -EINVAL;

	if (!lirc->drv->attached)
		return -ENODEV;

	do {
		if (kfifo_is_empty(&lirc->kfifo)) {
			if (filep->f_flags & O_NONBLOCK)
				return -EAGAIN;

			ret = wait_event_interruptible(lirc->wait_poll,
					!kfifo_is_empty(&lirc->kfifo) ||
					!lirc->drv->attached);
			if (ret)
				return ret;
		}

		if (!lirc->drv->attached)
			return -ENODEV;

		ret = kfifo_to_user(&lirc->kfifo, buffer, length, &copied);
		if (ret)
			return ret;
	} while (copied == 0);

	return copied;
}

static int ir_lirc_open(void *data)
{
	struct lirc_codec *lirc = data;

	kfifo_reset_out(&lirc->kfifo);

	return 0;
}

static void ir_lirc_close(void *data)
{
	return;
}

static const struct file_operations lirc_fops = {
	.owner		= THIS_MODULE,
	.write		= ir_lirc_transmit_ir,
	.unlocked_ioctl	= ir_lirc_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ir_lirc_ioctl,
#endif
	.read		= ir_lirc_read,
	.poll		= ir_lirc_poll,
	.open		= lirc_dev_fop_open,
	.release	= lirc_dev_fop_close,
	.llseek		= no_llseek,
};

int ir_lirc_register(struct rc_dev *dev)
{
	struct lirc_driver *drv;
	int rc = -ENOMEM;
	unsigned long features = 0;

	drv = kzalloc(sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	if (dev->driver_type != RC_DRIVER_IR_RAW_TX) {
		features |= LIRC_CAN_REC_MODE2;
		if (dev->rx_resolution)
			features |= LIRC_CAN_GET_REC_RESOLUTION;
	}
	if (dev->tx_ir) {
		features |= LIRC_CAN_SEND_PULSE | LIRC_CAN_SEND_SCANCODE;
		if (dev->s_tx_mask)
			features |= LIRC_CAN_SET_TRANSMITTER_MASK;
		if (dev->s_tx_carrier)
			features |= LIRC_CAN_SET_SEND_CARRIER;
		if (dev->s_tx_duty_cycle)
			features |= LIRC_CAN_SET_SEND_DUTY_CYCLE;
	}

	if (dev->s_rx_carrier_range)
		features |= LIRC_CAN_SET_REC_CARRIER |
			LIRC_CAN_SET_REC_CARRIER_RANGE;

	if (dev->s_learning_mode)
		features |= LIRC_CAN_USE_WIDEBAND_RECEIVER;

	if (dev->s_carrier_report)
		features |= LIRC_CAN_MEASURE_CARRIER;

	if (dev->max_timeout)
		features |= LIRC_CAN_SET_REC_TIMEOUT;

	snprintf(drv->name, sizeof(drv->name), "ir-lirc-codec (%s)",
		 dev->driver_name);
	drv->minor = -1;
	drv->features = features;
	drv->data = &dev->raw->lirc;
	drv->set_use_inc = &ir_lirc_open;
	drv->set_use_dec = &ir_lirc_close;
	drv->code_length = sizeof(struct ir_raw_event) * 8;
	drv->fops = &lirc_fops;
	drv->dev.parent = &dev->dev;
	drv->rdev = dev;
	drv->owner = THIS_MODULE;
	INIT_KFIFO(dev->raw->lirc.kfifo);
	init_waitqueue_head(&dev->raw->lirc.wait_poll);

	drv->minor = lirc_register_driver(drv);
	if (drv->minor < 0) {
		rc = -ENODEV;
		goto lirc_register_failed;
	}

	dev->raw->lirc.drv = drv;
	dev->raw->lirc.dev = dev;
	return 0;

lirc_register_failed:
	kfree(drv);
	return rc;
}

void ir_lirc_unregister(struct rc_dev *dev)
{
	struct lirc_codec *lirc = &dev->raw->lirc;

	wake_up_poll(&lirc->wait_poll, POLLERR);
	lirc_unregister_driver(lirc->drv->minor);
}
