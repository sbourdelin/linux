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
#include <linux/sched/signal.h>
#include <linux/wait.h>
#include <linux/module.h>
#include <media/lirc.h>
#include <media/lirc_dev.h>
#include <media/rc-core.h>
#include "rc-core-priv.h"

#define LIRCBUF_SIZE 256
#define LOGHEAD	"lirc_dev (%s[%d]): "

/**
 * ir_lirc_decode() - Send raw IR data to lirc_dev to be relayed to the
 *		      lircd userspace daemon for decoding.
 * @input_dev:	the struct rc_dev descriptor of the device
 * @duration:	the struct ir_raw_event descriptor of the pulse/space
 *
 * This function returns -EINVAL if the lirc interfaces aren't wired up.
 */
static int ir_lirc_decode(struct rc_dev *dev, struct ir_raw_event ev)
{
	struct lirc_codec *lirc = &dev->raw->lirc;
	int sample;

	if (!dev->raw->lirc.ldev || !dev->raw->lirc.ldev->buf)
		return -EINVAL;

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
			int gap_sample;

			lirc->gap_duration += ktime_to_ns(ktime_sub(ktime_get(),
				lirc->gap_start));

			/* Convert to ms and cap by LIRC_VALUE_MASK */
			do_div(lirc->gap_duration, 1000);
			lirc->gap_duration = min(lirc->gap_duration,
							(u64)LIRC_VALUE_MASK);

			gap_sample = LIRC_SPACE(lirc->gap_duration);
			lirc_buffer_write(dev->raw->lirc.ldev->buf,
						(unsigned char *) &gap_sample);
			lirc->gap = false;
		}

		sample = ev.pulse ? LIRC_PULSE(ev.duration / 1000) :
					LIRC_SPACE(ev.duration / 1000);
		IR_dprintk(2, "delivering %uus %s to lirc_dev\n",
			   TO_US(ev.duration), TO_STR(ev.pulse));
	}

	lirc_buffer_write(dev->raw->lirc.ldev->buf,
			  (unsigned char *) &sample);
	wake_up(&dev->raw->lirc.ldev->buf->wait_poll);

	return 0;
}

static ssize_t ir_lirc_transmit_ir(struct file *file, const char __user *buf,
				   size_t n, loff_t *ppos)
{
	struct lirc_codec *lirc;
	struct rc_dev *dev;
	unsigned int *txbuf; /* buffer with values to transmit */
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

	if (n < sizeof(unsigned) || n % sizeof(unsigned))
		return -EINVAL;

	count = n / sizeof(unsigned);
	if (count > LIRCBUF_SIZE || count % 2 == 0)
		return -EINVAL;

	txbuf = memdup_user(buf, n);
	if (IS_ERR(txbuf))
		return PTR_ERR(txbuf);

	dev = lirc->dev;
	if (!dev) {
		ret = -EFAULT;
		goto out;
	}

	if (!dev->tx_ir) {
		ret = -EINVAL;
		goto out;
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
	return ret;
}

static long ir_lirc_ioctl(struct file *filep, unsigned int cmd,
			  unsigned long arg)
{
	struct lirc_codec *lirc;
	struct rc_dev *dev;
	struct lirc_dev *d;
	u32 __user *argp = (u32 __user *)(arg);
	int ret;
	__u32 val = 0, tmp;

	lirc = lirc_get_pdata(filep);
	if (!lirc)
		return -EFAULT;

	dev = lirc->dev;
	if (!dev)
		return -EFAULT;

	d = lirc->ldev;
	if (!d)
		return -EFAULT;

	ret = mutex_lock_interruptible(&d->mutex);
	if (ret)
		return ret;

	if (!d->attached) {
		ret = -ENODEV;
		goto out;
	}

	if (_IOC_DIR(cmd) & _IOC_WRITE) {
		ret = get_user(val, argp);
		if (ret)
			goto out;
	}

	switch (cmd) {

	/* legacy support */
	case LIRC_GET_SEND_MODE:
		if (!dev->tx_ir)
			ret = -ENOTTY;
		else
			val = LIRC_MODE_PULSE;
		break;

	case LIRC_SET_SEND_MODE:
		if (!dev->tx_ir)
			ret = -ENOTTY;
		else if (val != LIRC_MODE_PULSE)
			ret = -EINVAL;
		break;

	/* TX settings */
	case LIRC_SET_TRANSMITTER_MASK:
		if (!dev->s_tx_mask)
			ret = -ENOTTY;
		else
			ret = dev->s_tx_mask(dev, val);
		break;

	case LIRC_SET_SEND_CARRIER:
		if (!dev->s_tx_carrier)
			ret = -ENOTTY;
		else
			ret = dev->s_tx_carrier(dev, val);
		break;

	case LIRC_SET_SEND_DUTY_CYCLE:
		if (!dev->s_tx_duty_cycle)
			ret = -ENOTTY;
		else if (val <= 0 || val >= 100)
			ret = -EINVAL;
		else
			ret = dev->s_tx_duty_cycle(dev, val);
		break;

	/* RX settings */
	case LIRC_SET_REC_CARRIER:
		if (!dev->s_rx_carrier_range)
			ret = -ENOTTY;
		else if (val <= 0)
			ret = -EINVAL;
		else
			ret = dev->s_rx_carrier_range(dev,
						      dev->raw->lirc.carrier_low,
						      val);
		break;

	case LIRC_SET_REC_CARRIER_RANGE:
		if (!dev->s_rx_carrier_range)
			ret = -ENOTTY;
		else if (val <= 0)
			ret = -EINVAL;
		else
			dev->raw->lirc.carrier_low = val;
		break;

	case LIRC_GET_REC_RESOLUTION:
		if (!dev->rx_resolution)
			ret = -ENOTTY;
		else
			val = dev->rx_resolution;
		break;

	case LIRC_SET_WIDEBAND_RECEIVER:
		if (!dev->s_learning_mode)
			ret = -ENOTTY;
		else
			ret = dev->s_learning_mode(dev, !!val);
		break;

	case LIRC_SET_MEASURE_CARRIER_MODE:
		if (!dev->s_carrier_report)
			ret = -ENOTTY;
		else
			ret = dev->s_carrier_report(dev, !!val);
		break;

	/* Generic timeout support */
	case LIRC_GET_MIN_TIMEOUT:
		if (!dev->max_timeout)
			ret = -ENOTTY;
		else
			val = DIV_ROUND_UP(dev->min_timeout, 1000);
		break;

	case LIRC_GET_MAX_TIMEOUT:
		if (!dev->max_timeout)
			ret = -ENOTTY;
		else
			val = dev->max_timeout / 1000;
		break;

	case LIRC_SET_REC_TIMEOUT:
		tmp = val * 1000;

		if (!dev->max_timeout)
			ret = -ENOTTY;
		else if (tmp < dev->min_timeout)
			ret = -EINVAL;
		else if (tmp > dev->max_timeout)
			ret = -EINVAL;
		else if (dev->s_timeout)
			ret = dev->s_timeout(dev, tmp);

		if (!ret)
			dev->timeout = tmp;
		break;

	case LIRC_SET_REC_TIMEOUT_REPORTS:
		if (!dev->timeout)
			ret = -ENOTTY;
		else
			lirc->send_timeout_reports = !!val;
		break;

	case LIRC_GET_FEATURES:
		val = d->features;
		break;

	case LIRC_GET_REC_MODE:
		if (!LIRC_CAN_REC(d->features))
			ret = -ENOTTY;
		else
			val = LIRC_REC2MODE(d->features & LIRC_CAN_REC_MASK);
		break;

	case LIRC_SET_REC_MODE:
		if (!LIRC_CAN_REC(d->features))
			ret = -ENOTTY;
		else if (!(LIRC_MODE2REC(val) & d->features))
			ret = -EINVAL;
		break;

	case LIRC_GET_LENGTH:
		val = d->code_length;
		break;

	default:
		ret = -ENOTTY;
	}

	if (!ret && (_IOC_DIR(cmd) & _IOC_READ))
		ret = put_user(val, argp);

out:
	mutex_unlock(&d->mutex);
	return ret;
}

static ssize_t ir_lirc_read(struct file *file, char __user *buffer,
			    size_t length, loff_t *ppos)
{
	struct lirc_dev *d = file->private_data;
	unsigned char buf[d->buf->chunk_size];
	int ret, written = 0;
	DECLARE_WAITQUEUE(wait, current);

	dev_dbg(&d->dev, LOGHEAD "read called\n", d->name, d->minor);

	ret = mutex_lock_interruptible(&d->mutex);
	if (ret)
		return ret;

	if (!d->attached) {
		ret = -ENODEV;
		goto out_locked;
	}

	if (!LIRC_CAN_REC(d->features)) {
		ret = -EINVAL;
		goto out_locked;
	}

	if (length % d->buf->chunk_size) {
		ret = -EINVAL;
		goto out_locked;
	}

	/*
	 * we add ourselves to the task queue before buffer check
	 * to avoid losing scan code (in case when queue is awaken somewhere
	 * between while condition checking and scheduling)
	 */
	add_wait_queue(&d->buf->wait_poll, &wait);

	/*
	 * while we didn't provide 'length' bytes, device is opened in blocking
	 * mode and 'copy_to_user' is happy, wait for data.
	 */
	while (written < length && ret == 0) {
		if (lirc_buffer_empty(d->buf)) {
			/* According to the read(2) man page, 'written' can be
			 * returned as less than 'length', instead of blocking
			 * again, returning -EWOULDBLOCK, or returning
			 * -ERESTARTSYS
			 */
			if (written)
				break;
			if (file->f_flags & O_NONBLOCK) {
				ret = -EWOULDBLOCK;
				break;
			}
			if (signal_pending(current)) {
				ret = -ERESTARTSYS;
				break;
			}

			mutex_unlock(&d->mutex);
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
			set_current_state(TASK_RUNNING);

			ret = mutex_lock_interruptible(&d->mutex);
			if (ret) {
				remove_wait_queue(&d->buf->wait_poll, &wait);
				goto out_unlocked;
			}

			if (!d->attached) {
				ret = -ENODEV;
				goto out_locked;
			}
		} else {
			lirc_buffer_read(d->buf, buf);
			ret = copy_to_user((void __user *)buffer+written, buf,
					   d->buf->chunk_size);
			if (!ret)
				written += d->buf->chunk_size;
			else
				ret = -EFAULT;
		}
	}

	remove_wait_queue(&d->buf->wait_poll, &wait);

out_locked:
	mutex_unlock(&d->mutex);

out_unlocked:
	return ret ? ret : written;
}

static unsigned int ir_lirc_poll(struct file *file, poll_table *wait)
{
	struct lirc_dev *d = file->private_data;
	unsigned int ret;

	if (!d->attached)
		return POLLHUP | POLLERR;

	if (d->buf) {
		poll_wait(file, &d->buf->wait_poll, wait);

		if (lirc_buffer_empty(d->buf))
			ret = 0;
		else
			ret = POLLIN | POLLRDNORM;
	} else
		ret = POLLERR;

	dev_dbg(&d->dev, LOGHEAD "poll result = %d\n", d->name, d->minor, ret);

	return ret;
}

static int ir_lirc_open(struct inode *inode, struct file *file)
{
	struct lirc_dev *d = container_of(inode->i_cdev, struct lirc_dev, cdev);
	int retval;

	dev_dbg(&d->dev, LOGHEAD "open called\n", d->name, d->minor);

	retval = mutex_lock_interruptible(&d->mutex);
	if (retval)
		return retval;

	if (!d->attached) {
		retval = -ENODEV;
		goto out;
	}

	if (d->open) {
		retval = -EBUSY;
		goto out;
	}

	retval = rc_open(d->rdev);
	if (retval)
		goto out;

	if (d->buf)
		lirc_buffer_clear(d->buf);

	d->open++;

	lirc_init_pdata(inode, file);
	nonseekable_open(inode, file);
	mutex_unlock(&d->mutex);

	return 0;

out:
	mutex_unlock(&d->mutex);
	return retval;
}

static int ir_lirc_close(struct inode *inode, struct file *file)
{
	struct lirc_dev *d = file->private_data;

	mutex_lock(&d->mutex);

	rc_close(d->rdev);
	d->open--;

	mutex_unlock(&d->mutex);

	return 0;
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
	.open		= ir_lirc_open,
	.release	= ir_lirc_close,
	.llseek		= no_llseek,
};

static int ir_lirc_register(struct rc_dev *dev)
{
	struct lirc_dev *ldev;
	int rc = -ENOMEM;
	unsigned long features = 0;

	ldev = lirc_allocate_device();
	if (!ldev)
		return rc;

	if (dev->driver_type != RC_DRIVER_IR_RAW_TX) {
		features |= LIRC_CAN_REC_MODE2;
		if (dev->rx_resolution)
			features |= LIRC_CAN_GET_REC_RESOLUTION;
	}

	if (dev->tx_ir) {
		features |= LIRC_CAN_SEND_PULSE;
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

	snprintf(ldev->name, sizeof(ldev->name), "%s", dev->input_name);
	ldev->features = features;
	ldev->data = &dev->raw->lirc;
	ldev->buf = NULL;
	ldev->code_length = sizeof(struct ir_raw_event) * 8;
	ldev->chunk_size = sizeof(int);
	ldev->buffer_size = LIRCBUF_SIZE;
	ldev->fops = &lirc_fops;
	ldev->dev.parent = &dev->dev;
	ldev->rdev = dev;
	ldev->owner = THIS_MODULE;

	rc = lirc_register_device(ldev);
	if (rc < 0)
		goto out;

	dev->raw->lirc.ldev = ldev;
	dev->raw->lirc.dev = dev;
	return 0;

out:
	lirc_free_device(ldev);
	return rc;
}

static int ir_lirc_unregister(struct rc_dev *dev)
{
	struct lirc_codec *lirc = &dev->raw->lirc;

	lirc_unregister_device(lirc->ldev);
	lirc->ldev = NULL;

	return 0;
}

static struct ir_raw_handler lirc_handler = {
	.protocols	= 0,
	.decode		= ir_lirc_decode,
	.raw_register	= ir_lirc_register,
	.raw_unregister	= ir_lirc_unregister,
};

static int __init ir_lirc_codec_init(void)
{
	ir_raw_handler_register(&lirc_handler);

	printk(KERN_INFO "IR LIRC bridge handler initialized\n");
	return 0;
}

static void __exit ir_lirc_codec_exit(void)
{
	ir_raw_handler_unregister(&lirc_handler);
}

module_init(ir_lirc_codec_init);
module_exit(ir_lirc_codec_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jarod Wilson <jarod@redhat.com>");
MODULE_AUTHOR("Red Hat Inc. (http://www.redhat.com)");
MODULE_DESCRIPTION("LIRC IR handler bridge");
