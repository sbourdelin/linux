/*
 * LIRC base driver
 *
 * by Artur Lipowski <alipowski@interia.pl>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/unistd.h>
#include <linux/kthread.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/cdev.h>

#include "rc-core-priv.h"
#include <media/lirc.h>
#include <media/lirc_dev.h>

#define IRCTL_DEV_NAME	"BaseRemoteCtl"
#define NOPLUG		-1
#define LOGHEAD		"lirc_dev (%s[%d]): "

static dev_t lirc_base_dev;

static DEFINE_MUTEX(lirc_dev_lock);

static struct lirc_driver *irctls[MAX_IRCTL_DEVICES];

/* Only used for sysfs but defined to void otherwise */
static struct class *lirc_class;

/*  helper function
 *  initializes the irctl structure
 */
static void lirc_irctl_init(struct lirc_driver *d)
{
	mutex_init(&d->irctl_lock);
	d->minor = NOPLUG;
}

static void lirc_release(struct device *ld)
{
	struct lirc_driver *d = container_of(ld, struct lirc_driver, dev);

	put_device(d->dev.parent);

	if (d->buf != d->rbuf) {
		lirc_buffer_free(d->buf);
		kfree(d->buf);
	}

	mutex_lock(&lirc_dev_lock);
	irctls[d->minor] = NULL;
	mutex_unlock(&lirc_dev_lock);
	kfree(d);
}

/*  helper function
 *  reads key codes from driver and puts them into buffer
 *  returns 0 on success
 */
static int lirc_add_to_buf(struct lirc_driver *d)
{
	int res;
	int got_data = -1;

	if (!d->add_to_buf)
		return 0;

	/*
	 * service the device as long as it is returning
	 * data and we have space
	 */
	do {
		got_data++;
		res = d->add_to_buf(d->data, d->buf);
	} while (!res);

	if (res == -ENODEV)
		kthread_stop(d->task);

	return got_data ? 0 : res;
}

/* main function of the polling thread
 */
static int lirc_thread(void *lirc_driver)
{
	struct lirc_driver *d = lirc_driver;

	do {
		if (d->open) {
			if (d->jiffies_to_wait) {
				set_current_state(TASK_INTERRUPTIBLE);
				schedule_timeout(d->jiffies_to_wait);
			}
			if (kthread_should_stop())
				break;
			if (!lirc_add_to_buf(d))
				wake_up_interruptible(&d->buf->wait_poll);
		} else {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
		}
	} while (!kthread_should_stop());

	return 0;
}

static const struct file_operations lirc_dev_fops = {
	.owner		= THIS_MODULE,
	.read		= lirc_dev_fop_read,
	.write		= lirc_dev_fop_write,
	.poll		= lirc_dev_fop_poll,
	.unlocked_ioctl	= lirc_dev_fop_ioctl,
	.open		= lirc_dev_fop_open,
	.release	= lirc_dev_fop_close,
	.llseek		= noop_llseek,
};

static int lirc_cdev_add(struct lirc_driver *d)
{
	struct cdev *cdev;
	int retval;

	cdev = &d->cdev;

	if (d->fops) {
		cdev_init(cdev, d->fops);
		cdev->owner = d->owner;
	} else {
		cdev_init(cdev, &lirc_dev_fops);
		cdev->owner = THIS_MODULE;
	}
	retval = kobject_set_name(&cdev->kobj, "lirc%d", d->minor);
	if (retval)
		return retval;

	cdev->kobj.parent = &d->dev.kobj;
	return cdev_add(cdev, d->dev.devt, 1);
}

static int lirc_allocate_buffer(struct lirc_driver *d)
{
	int err = 0;
	int bytes_in_key;
	unsigned int chunk_size;
	unsigned int buffer_size;

	mutex_lock(&lirc_dev_lock);

	bytes_in_key = BITS_TO_LONGS(d->code_length) +
						(d->code_length % 8 ? 1 : 0);
	buffer_size = d->buffer_size ? d->buffer_size : BUFLEN / bytes_in_key;
	chunk_size  = d->chunk_size  ? d->chunk_size  : bytes_in_key;

	if (d->rbuf) {
		d->buf = d->rbuf;
	} else {
		d->buf = kmalloc(sizeof(*d->buf), GFP_KERNEL);
		if (!d->buf) {
			err = -ENOMEM;
			goto out;
		}

		err = lirc_buffer_init(d->buf, chunk_size, buffer_size);
		if (err) {
			kfree(d->buf);
			goto out;
		}
	}
	d->chunk_size = d->buf->chunk_size;

out:
	mutex_unlock(&lirc_dev_lock);

	return err;
}

static int lirc_allocate_driver(struct lirc_driver *d)
{
	int minor;
	int err;

	if (!d) {
		pr_err("driver pointer must be not NULL!\n");
		return -EBADRQC;
	}

	if (!d->dev.parent) {
		pr_err("dev pointer not filled in!\n");
		return -EINVAL;
	}

	if (d->minor >= MAX_IRCTL_DEVICES) {
		dev_err(d->dev.parent, "minor must be between 0 and %d!\n",
			MAX_IRCTL_DEVICES - 1);
		return -EBADRQC;
	}

	if (d->code_length < 1 || d->code_length > (BUFLEN * 8)) {
		dev_err(d->dev.parent, "code length must be less than %d bits\n",
			BUFLEN * 8);
		return -EBADRQC;
	}

	if (d->sample_rate) {
		if (2 > d->sample_rate || HZ < d->sample_rate) {
			dev_err(d->dev.parent, "invalid %d sample rate\n",
				d->sample_rate);
			return -EBADRQC;
		}
		if (!d->add_to_buf) {
			dev_err(d->dev.parent, "add_to_buf not set\n");
			return -EBADRQC;
		}
	} else if (!d->rbuf && !(d->fops && d->fops->read &&
				d->fops->poll && d->fops->unlocked_ioctl)) {
		dev_err(d->dev.parent, "undefined read, poll, ioctl\n");
		return -EBADRQC;
	}

	mutex_lock(&lirc_dev_lock);

	minor = d->minor;

	if (minor < 0) {
		/* find first free slot for driver */
		for (minor = 0; minor < MAX_IRCTL_DEVICES; minor++)
			if (!irctls[minor])
				break;
		if (minor == MAX_IRCTL_DEVICES) {
			dev_err(d->dev.parent, "no free slots for drivers!\n");
			err = -ENOMEM;
			goto out_lock;
		}
	} else if (irctls[minor]) {
		dev_err(d->dev.parent, "minor (%d) just registered!\n", minor);
		err = -EBUSY;
		goto out_lock;
	}

	lirc_irctl_init(d);
	irctls[minor] = d;
	d->minor = minor;

	/* some safety check 8-) */
	d->name[sizeof(d->name)-1] = '\0';

	if (d->features == 0)
		d->features = LIRC_CAN_REC_LIRCCODE;

	d->dev.devt = MKDEV(MAJOR(lirc_base_dev), d->minor);
	d->dev.class = lirc_class;
	d->dev.release = lirc_release;
	dev_set_name(&d->dev, "lirc%d", d->minor);
	device_initialize(&d->dev);

	if (d->sample_rate) {
		d->jiffies_to_wait = HZ / d->sample_rate;

		/* try to fire up polling thread */
		d->task = kthread_run(lirc_thread, d, "lirc_dev");
		if (IS_ERR(d->task)) {
			dev_err(d->dev.parent, "cannot run thread for minor = %d\n",
				d->minor);
			err = -ECHILD;
			goto out_sysfs;
		}
	} else {
		/* it means - wait for external event in task queue */
		d->jiffies_to_wait = 0;
	}

	err = lirc_cdev_add(d);
	if (err)
		goto out_sysfs;

	d->attached = 1;

	err = device_add(&d->dev);
	if (err)
		goto out_cdev;

	mutex_unlock(&lirc_dev_lock);

	get_device(d->dev.parent);

	dev_info(d->dev.parent, "lirc_dev: driver %s registered at minor = %d\n",
		 d->name, d->minor);
	return minor;
out_cdev:
	cdev_del(&d->cdev);
out_sysfs:
	put_device(&d->dev);
out_lock:
	mutex_unlock(&lirc_dev_lock);

	return err;
}

int lirc_register_driver(struct lirc_driver *d)
{
	int minor, err = 0;

	minor = lirc_allocate_driver(d);
	if (minor < 0)
		return minor;

	if (LIRC_CAN_REC(d->features)) {
		err = lirc_allocate_buffer(irctls[minor]);
		if (err)
			lirc_unregister_driver(minor);
	}

	return err ? err : minor;
}
EXPORT_SYMBOL(lirc_register_driver);

int lirc_unregister_driver(int minor)
{
	struct lirc_driver *d;

	if (minor < 0 || minor >= MAX_IRCTL_DEVICES) {
		pr_err("minor (%d) must be between 0 and %d!\n",
					minor, MAX_IRCTL_DEVICES - 1);
		return -EBADRQC;
	}

	d = irctls[minor];
	if (!d) {
		pr_err("failed to get irctl\n");
		return -ENOENT;
	}

	mutex_lock(&lirc_dev_lock);

	if (d->minor != minor) {
		dev_err(d->dev.parent, "lirc_dev: minor %d device not registered\n",
			minor);
		mutex_unlock(&lirc_dev_lock);
		return -ENOENT;
	}

	/* end up polling thread */
	if (d->task)
		kthread_stop(d->task);

	dev_dbg(d->dev.parent, "lirc_dev: driver %s unregistered from minor = %d\n",
		d->name, d->minor);

	d->attached = 0;
	if (d->open) {
		dev_dbg(d->dev.parent, LOGHEAD "releasing opened driver\n",
			d->name, d->minor);
		wake_up_interruptible(&d->buf->wait_poll);
	}

	mutex_lock(&d->irctl_lock);

	if (d->set_use_dec)
		d->set_use_dec(d->data);

	mutex_unlock(&d->irctl_lock);
	mutex_unlock(&lirc_dev_lock);

	device_del(&d->dev);
	cdev_del(&d->cdev);
	put_device(&d->dev);

	return 0;
}
EXPORT_SYMBOL(lirc_unregister_driver);

int lirc_dev_fop_open(struct inode *inode, struct file *file)
{
	struct lirc_driver *d;
	int retval = 0;

	if (iminor(inode) >= MAX_IRCTL_DEVICES) {
		pr_err("open result for %d is -ENODEV\n", iminor(inode));
		return -ENODEV;
	}

	if (mutex_lock_interruptible(&lirc_dev_lock))
		return -ERESTARTSYS;

	d = irctls[iminor(inode)];
	if (!d) {
		retval = -ENODEV;
		goto error;
	}

	dev_dbg(d->dev.parent, LOGHEAD "open called\n", d->name, d->minor);

	if (d->minor == NOPLUG) {
		retval = -ENODEV;
		goto error;
	}

	if (d->open) {
		retval = -EBUSY;
		goto error;
	}

	if (d->rdev) {
		retval = rc_open(d->rdev);
		if (retval)
			goto error;
	}

	d->open++;
	if (d->set_use_inc)
		retval = d->set_use_inc(d->data);
	if (retval) {
		d->open--;
	} else {
		if (d->buf)
			lirc_buffer_clear(d->buf);
		if (d->task)
			wake_up_process(d->task);
	}

error:
	mutex_unlock(&lirc_dev_lock);

	nonseekable_open(inode, file);

	return retval;
}
EXPORT_SYMBOL(lirc_dev_fop_open);

int lirc_dev_fop_close(struct inode *inode, struct file *file)
{
	struct lirc_driver *d = irctls[iminor(inode)];
	int ret;

	if (!d) {
		pr_err("called with invalid irctl\n");
		return -EINVAL;
	}

	ret = mutex_lock_killable(&lirc_dev_lock);
	WARN_ON(ret);

	rc_close(d->rdev);

	d->open--;
	if (d->set_use_dec)
		d->set_use_dec(d->data);
	if (!ret)
		mutex_unlock(&lirc_dev_lock);

	return 0;
}
EXPORT_SYMBOL(lirc_dev_fop_close);

unsigned int lirc_dev_fop_poll(struct file *file, poll_table *wait)
{
	struct lirc_driver *d = irctls[iminor(file_inode(file))];
	unsigned int ret;

	if (!d) {
		pr_err("called with invalid irctl\n");
		return POLLERR;
	}

	if (!d->attached)
		return POLLERR;

	if (d->buf) {
		poll_wait(file, &d->buf->wait_poll, wait);

		if (lirc_buffer_empty(d->buf))
			ret = 0;
		else
			ret = POLLIN | POLLRDNORM;
	} else {
		ret = POLLERR;
	}

	dev_dbg(d->dev.parent, LOGHEAD "poll result = %d\n",
		d->name, d->minor, ret);

	return ret;
}
EXPORT_SYMBOL(lirc_dev_fop_poll);

long lirc_dev_fop_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	__u32 mode;
	int result = 0;
	struct lirc_driver *d = irctls[iminor(file_inode(file))];

	if (!d) {
		pr_err("no irctl found!\n");
		return -ENODEV;
	}

	dev_dbg(d->dev.parent, LOGHEAD "ioctl called (0x%x)\n",
		d->name, d->minor, cmd);

	if (d->minor == NOPLUG || !d->attached) {
		dev_err(d->dev.parent, LOGHEAD "ioctl result = -ENODEV\n",
			d->name, d->minor);
		return -ENODEV;
	}

	mutex_lock(&d->irctl_lock);

	switch (cmd) {
	case LIRC_GET_FEATURES:
		result = put_user(d->features, (__u32 __user *)arg);
		break;
	case LIRC_GET_REC_MODE:
		if (!LIRC_CAN_REC(d->features)) {
			result = -ENOTTY;
			break;
		}

		result = put_user(LIRC_REC2MODE
				  (d->features & LIRC_CAN_REC_MASK),
				  (__u32 __user *)arg);
		break;
	case LIRC_SET_REC_MODE:
		if (!LIRC_CAN_REC(d->features)) {
			result = -ENOTTY;
			break;
		}

		result = get_user(mode, (__u32 __user *)arg);
		if (!result && !(LIRC_MODE2REC(mode) & d->features))
			result = -EINVAL;
		/*
		 * FIXME: We should actually set the mode somehow but
		 * for now, lirc_serial doesn't support mode changing either
		 */
		break;
	case LIRC_GET_LENGTH:
		result = put_user(d->code_length, (__u32 __user *)arg);
		break;
	case LIRC_GET_MIN_TIMEOUT:
		if (!(d->features & LIRC_CAN_SET_REC_TIMEOUT) ||
		    d->min_timeout == 0) {
			result = -ENOTTY;
			break;
		}

		result = put_user(d->min_timeout, (__u32 __user *)arg);
		break;
	case LIRC_GET_MAX_TIMEOUT:
		if (!(d->features & LIRC_CAN_SET_REC_TIMEOUT) ||
		    d->max_timeout == 0) {
			result = -ENOTTY;
			break;
		}

		result = put_user(d->max_timeout, (__u32 __user *)arg);
		break;
	default:
		result = -ENOTTY;
	}

	mutex_unlock(&d->irctl_lock);

	return result;
}
EXPORT_SYMBOL(lirc_dev_fop_ioctl);

ssize_t lirc_dev_fop_read(struct file *file,
			  char __user *buffer,
			  size_t length,
			  loff_t *ppos)
{
	struct lirc_driver *d = irctls[iminor(file_inode(file))];
	unsigned char *buf;
	int ret = 0, written = 0;
	DECLARE_WAITQUEUE(wait, current);

	if (!d) {
		pr_err("called with invalid irctl\n");
		return -ENODEV;
	}

	if (!LIRC_CAN_REC(d->features))
		return -EINVAL;

	dev_dbg(d->dev.parent, LOGHEAD "read called\n", d->name, d->minor);

	buf = kzalloc(d->chunk_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (mutex_lock_interruptible(&d->irctl_lock)) {
		ret = -ERESTARTSYS;
		goto out_unlocked;
	}
	if (!d->attached) {
		ret = -ENODEV;
		goto out_locked;
	}

	if (length % d->chunk_size) {
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

			mutex_unlock(&d->irctl_lock);
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
			set_current_state(TASK_RUNNING);

			if (mutex_lock_interruptible(&d->irctl_lock)) {
				ret = -ERESTARTSYS;
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
	mutex_unlock(&d->irctl_lock);

out_unlocked:
	kfree(buf);

	return ret ? ret : written;
}
EXPORT_SYMBOL(lirc_dev_fop_read);

void *lirc_get_pdata(struct file *file)
{
	return irctls[iminor(file_inode(file))]->data;
}
EXPORT_SYMBOL(lirc_get_pdata);


ssize_t lirc_dev_fop_write(struct file *file, const char __user *buffer,
			   size_t length, loff_t *ppos)
{
	struct lirc_driver *d = irctls[iminor(file_inode(file))];

	if (!d) {
		pr_err("called with invalid irctl\n");
		return -ENODEV;
	}

	if (!d->attached)
		return -ENODEV;

	return -EINVAL;
}
EXPORT_SYMBOL(lirc_dev_fop_write);


int __init lirc_dev_init(void)
{
	int retval;

	lirc_class = class_create(THIS_MODULE, "lirc");
	if (IS_ERR(lirc_class)) {
		pr_err("class_create failed\n");
		return PTR_ERR(lirc_class);
	}

	retval = alloc_chrdev_region(&lirc_base_dev, 0, MAX_IRCTL_DEVICES,
				     IRCTL_DEV_NAME);
	if (retval) {
		class_destroy(lirc_class);
		pr_err("alloc_chrdev_region failed\n");
		return retval;
	}

	pr_info("IR Remote Control driver registered, major %d\n",
						MAJOR(lirc_base_dev));

	return 0;
}

void __exit lirc_dev_exit(void)
{
	class_destroy(lirc_class);
	unregister_chrdev_region(lirc_base_dev, MAX_IRCTL_DEVICES);
}
