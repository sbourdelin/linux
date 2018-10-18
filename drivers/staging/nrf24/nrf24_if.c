// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2017 Marcin Ciupak <marcin.s.ciupak@gmail.com>
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/spi/spi.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/list.h>

#include "nrf24_if.h"
#include "nrf24_sysfs.h"
#include "nrf24_hal.h"

#define N_NRF24_MINORS                  BIT(MINORBITS)

static dev_t nrf24_dev;
static DEFINE_IDA(nrf24_ida_pipe);
static DEFINE_IDA(nrf24_ida_dev);
static struct class *nrf24_class;

ATTRIBUTE_GROUPS(nrf24_pipe);
ATTRIBUTE_GROUPS(nrf24);

static bool nrf24_is_rx_active(struct nrf24_device *device)
{
	struct nrf24_pipe *pipe;

	bool active = false;

	list_for_each_entry(pipe, &device->pipes, list)
		active |= pipe->rx_size > 0;

	return active;
}

static void nrf24_ce_hi(struct nrf24_device *device)
{
	gpiod_set_value(device->ce, 1);
}

static void nrf24_ce_lo(struct nrf24_device *device)
{
	gpiod_set_value(device->ce, 0);
}

static struct nrf24_pipe *nrf24_find_pipe_id(struct nrf24_device *device, int id)
{
	struct nrf24_pipe *pipe;

	list_for_each_entry(pipe, &device->pipes, list)
		if (pipe->id == id)
			return pipe;

	return ERR_PTR(-ENODEV);
}

static int nrf24_tx_thread(void *data)
{
	struct nrf24_device *device = data;
	struct nrf24_pipe *p;
	u8 pload[PLOAD_MAX];
	int ret;
	ssize_t size;
	ssize_t pload_length;
	ssize_t sent = 0;
	u8 *buf;
	bool spl;
	bool dpl = false;

	while (true) {
		dev_dbg(&device->dev,
			"%s: waiting for new messages\n",
			__func__);
		wait_event_interruptible(device->tx_wait_queue,
					 kthread_should_stop() ||
					 (!nrf24_is_rx_active(device) && !kfifo_is_empty(&device->tx_fifo)));

		if (kthread_should_stop())
			return 0;

		device->tx_done = false;

		//fifo lock is needed as write to tx fifo may be done by 6 pipes
		mutex_lock(&device->tx_fifo_mutex);

		ret = kfifo_out(&device->tx_fifo, &p, sizeof(p));
		if (ret != sizeof(p)) {
			dev_dbg(&device->dev, "get pipe from fifo failed\n");
			mutex_unlock(&device->tx_fifo_mutex);
			continue;
		}

		ret = kfifo_out(&device->tx_fifo, &size, sizeof(size));
		if (ret != sizeof(size)) {
			dev_dbg(&device->dev, "get size from fifo failed\n");
			mutex_unlock(&device->tx_fifo_mutex);
			continue;
		}

		buf = kzalloc(size, GFP_KERNEL);
		if (!buf) {
			dev_dbg(&device->dev, "buf alloc failed\n");
			mutex_unlock(&device->tx_fifo_mutex);
			continue;
		}

		ret = kfifo_out(&device->tx_fifo, buf, size);
		if (ret != size) {
			dev_dbg(&device->dev, "get buf from fifo failed\n");
			mutex_unlock(&device->tx_fifo_mutex);
			goto next;
		}

		mutex_unlock(&device->tx_fifo_mutex);

		//enter Standby-I mode
		nrf24_ce_lo(device);

		ret = nrf24_set_mode(device->spi, NRF24_MODE_TX);
		if (ret < 0)
			goto next;

		//set PIPE0 address in order to receive ACK
		ret = nrf24_set_address(device->spi,
					NRF24_PIPE0,
					(u8 *)&p->cfg.address);
		if (ret < 0) {
			dev_dbg(&device->dev, "set PIPE0 address failed (%d)\n", ret);
			goto next;
		}

		ret = nrf24_set_address(device->spi,
					NRF24_TX,
					(u8 *)&p->cfg.address);
		if (ret < 0) {
			dev_dbg(&device->dev, "set TX address failed (%d)\n", ret);
			goto next;
		}

		//check if pipe uses static payload length
		spl = p->cfg.plw != 0;

		//check if dynamic payload length is enabled
		dpl = nrf24_get_dynamic_pl(device->spi);

		if (spl && dpl) {
			//disable dynamic payload if pipe
			//does not use dynamic payload
			//and dynamic paload is enabled
			ret = nrf24_disable_dynamic_pl(device->spi);
			if (ret < 0)
				goto next;
		}

		memset(pload, 0, PLOAD_MAX);
		memcpy(pload, &size, sizeof(size));

		//calculate payload length
		pload_length = spl ? p->cfg.plw : sizeof(size);

		//send size
		nrf24_write_tx_pload(device->spi, pload, pload_length);
		if (ret < 0) {
			dev_dbg(&device->dev, "write TX PLOAD failed (%d)\n", ret);
			goto next;
		}

		//enter TX MODE and start transmission
		nrf24_ce_hi(device);

		//wait for ACK
		wait_event_interruptible(device->tx_done_wait_queue,
					 (device->tx_done ||
					 kthread_should_stop()));

		if (kthread_should_stop())
			goto abort;

		sent = 0;

		while (size > 0) {
			pload_length = spl ? p->cfg.plw : min_t(ssize_t, size, PLOAD_MAX);

			dev_dbg(&device->dev, "tx %zd bytes\n", pload_length);

			memset(pload, 0, PLOAD_MAX);
			memcpy(pload, buf + sent, pload_length);

			ret = nrf24_write_tx_pload(device->spi, pload, pload_length);

			if (ret < 0) {
				dev_dbg(&device->dev,
					"write TX PLOAD failed (%d)\n",
					ret);
				goto next;
			}

			sent += pload_length;
			size -= pload_length;

			device->tx_done = false;

			//wait for ACK
			wait_event_interruptible(device->tx_done_wait_queue,
						 (device->tx_done ||
						 kthread_should_stop()));

			if (kthread_should_stop())
				goto abort;
		}
next:
		kfree(buf);

		//restore dynamic payload feature
		if (dpl)
			nrf24_enable_dynamic_pl(device->spi);

		//if all sent enter RX MODE and start receiving
		if (kfifo_is_empty(&device->tx_fifo)) {
			dev_dbg(&device->dev, "%s: NRF24_MODE_RX\n", __func__);

			//enter Standby-I
			nrf24_ce_lo(device);

			p = nrf24_find_pipe_id(device, NRF24_PIPE0);
			if (!IS_ERR(p)) {
				//restore PIPE0 address as it was corrupted
				nrf24_set_address(device->spi,
						  p->id,
						  (u8 *)&p->cfg.address);
			}

			nrf24_set_mode(device->spi, NRF24_MODE_RX);
			nrf24_ce_hi(device);
		}
	}
abort:
	kfree(buf);

	return 0;
}

static int nrf24_rx_thread(void *data)
{
	struct nrf24_device *device = data;
	ssize_t pipe;
	ssize_t length;
	u8 pload[PLOAD_MAX];
	struct nrf24_pipe *p;

	while (true) {
		wait_event_interruptible(device->rx_wait_queue,
					 (!nrf24_is_rx_fifo_empty(device->spi) ||
					 kthread_should_stop()));
		if (kthread_should_stop())
			return 0;

		pipe = nrf24_get_rx_data_source(device->spi);
		if (pipe < 0) {
			dev_dbg(&device->dev,
				"%s: get pipe failed (err: %zd)\n",
				__func__,
				pipe);
			continue;
		}

		if (pipe > NRF24_PIPE5) {
			dev_dbg(&device->dev,
				"%s: RX FIFO is empty!\n",
				__func__);
			continue;
		}

		p = nrf24_find_pipe_id(device, pipe);
		if (IS_ERR(p))
			continue;

		memset(pload, 0, PLOAD_MAX);
		length = nrf24_read_rx_pload(device->spi, pload);
		if (length < 0) {
			dev_dbg(&device->dev,
				"%s: could not read pload (err = %zd)\n",
				__func__,
				length);
			continue;
		}

		dev_dbg(p->dev, "rx %zd bytes\n", length);
		if (p->rx_size <= 0) {
			memcpy(&p->rx_size, pload, sizeof(p->rx_size));
			dev_dbg(p->dev, "RX active\n");
		} else {
			length = p->rx_size < p->cfg.plw ? p->rx_size : length;

			p->rx_size -= kfifo_in(&p->rx_fifo, &pload, length);

			if (p->rx_size <= 0) {
				dev_dbg(p->dev, "RX done\n");
				wake_up_interruptible(&p->poll_wait_queue);
			}
		}

		//start tx if all rx done and tx requested during active rx
		if (!nrf24_is_rx_active(device) && !kfifo_is_empty(&device->tx_fifo)) {
			dev_dbg(&device->dev, "wake up TX...\n");
			wake_up_interruptible(&device->tx_wait_queue);
		}
	}
}

static void nrf24_isr_work_handler(struct work_struct *work)
{
	struct nrf24_device *device;
	ssize_t status;

	device = container_of(work, struct nrf24_device, isr_work);

	status = nrf24_get_status(device->spi);
	if (status < 0)
		return;

	if (status & RX_DR) {
		dev_dbg(&device->dev, "%s: RX_DR\n", __func__);
		nrf24_clear_irq(device->spi, RX_DR);
		wake_up_interruptible(&device->rx_wait_queue);
	}

	if (status & TX_DS) {
		dev_dbg(&device->dev, "%s: TX_DS\n", __func__);
		nrf24_clear_irq(device->spi, TX_DS);
		device->tx_done = true;
		wake_up_interruptible(&device->tx_done_wait_queue);
	}

	if (status & MAX_RT) {
		nrf24_ce_lo(device);
		dev_dbg_ratelimited(&device->dev, "%s: MAX_RT\n", __func__);
		nrf24_clear_irq(device->spi, MAX_RT);
		nrf24_reuse_tx_pl(device->spi);
		nrf24_ce_hi(device);
	}
}

static irqreturn_t nrf24_isr(int irq, void *dev_id)
{
	unsigned long flags;
	struct nrf24_device *device = dev_id;

	spin_lock_irqsave(&device->lock, flags);

	schedule_work(&device->isr_work);

	spin_unlock_irqrestore(&device->lock, flags);

	return IRQ_HANDLED;
}

static ssize_t nrf24_read(struct file *filp,
			  char __user *buf,
			  size_t size,
			  loff_t *f_pos)
{
	struct nrf24_pipe *p;
	unsigned int copied;
	ssize_t n;

	p = filp->private_data;

	if (kfifo_is_empty(&p->rx_fifo) && (filp->f_flags & O_NONBLOCK))
		return -EAGAIN;

	n = kfifo_to_user(&p->rx_fifo, buf, size, &copied);
	if (n)
		return n;
	return copied;
}

static ssize_t nrf24_write(struct file *filp,
			   const char __user *buf,
			   size_t size,
			   loff_t *f_pos)
{
	struct nrf24_device *device;
	struct nrf24_pipe *p;
	ssize_t n;
	unsigned int copied;

	p = filp->private_data;
	device = to_nrf24_device(p->dev->parent);

	dev_dbg(p->dev, "write (%zd)\n", size);

	mutex_lock(&device->tx_fifo_mutex);

	n = kfifo_in(&device->tx_fifo, &p, sizeof(p));
	if (n != sizeof(p))
		goto err_kfifo_reset;

	n = kfifo_in(&device->tx_fifo, &size, sizeof(size));
	if (n != sizeof(size))
		goto err_kfifo_reset;

	n = kfifo_from_user(&device->tx_fifo,
			    buf,
			    size,
			    &copied);
	if (n || size != copied)
		goto err_kfifo_reset;

	mutex_unlock(&device->tx_fifo_mutex);

	wake_up_interruptible(&device->tx_wait_queue);

	return copied;
err_kfifo_reset:
	kfifo_reset(&device->tx_fifo);
	mutex_unlock(&device->tx_fifo_mutex);
	return -EAGAIN;
}

static int nrf24_open(struct inode *inode, struct file *filp)
{
	struct nrf24_pipe *pipe;

	pipe = container_of(inode->i_cdev, struct nrf24_pipe, cdev);

	if (!pipe) {
		pr_err("device: minor %d unknown.\n", iminor(inode));
		return -ENODEV;
	}

	filp->private_data = pipe;
	nonseekable_open(inode, filp);

	return 0;
}

static int nrf24_release(struct inode *inode, struct file *filp)
{
	filp->private_data = NULL;

	return 0;
}

static unsigned int nrf24_poll(struct file *filp,
			       struct poll_table_struct *wait)
{
	struct nrf24_device *device;
	struct nrf24_pipe *p;

	p = filp->private_data;
	device = to_nrf24_device(p->dev->parent);

	dev_dbg(p->dev, "%s: waiting...\n", __func__);
	poll_wait(filp, &p->poll_wait_queue, wait);
	if (!kfifo_is_empty(&p->rx_fifo)) {
		dev_dbg(p->dev, "%s: got data!\n", __func__);
		return POLLIN | POLLRDNORM;
	}
	dev_dbg(p->dev, "%s: no data!\n", __func__);
	return 0;
}

static void nrf24_destroy_devices(struct nrf24_device *device)
{
	struct nrf24_pipe *pipe, *temp;

	list_for_each_entry_safe(pipe, temp, &device->pipes, list) {
		cdev_del(&pipe->cdev);
		device_destroy(nrf24_class, pipe->devt);
		ida_simple_remove(&nrf24_ida_pipe, MINOR(pipe->devt));
		list_del(&pipe->list);
		kfree(pipe);
	}
}

static const struct file_operations nrf24_fops = {
	.owner = THIS_MODULE,
	.open = nrf24_open,
	.release = nrf24_release,
	.read = nrf24_read,
	.write = nrf24_write,
	.llseek = no_llseek,
	.poll = nrf24_poll,
};

static struct nrf24_pipe *nrf24_create_pipe(struct nrf24_device *device, int id)
{
	int ret;
	struct nrf24_pipe *p;

	//sets flags to false as well
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p) {
		ret = -ENOMEM;
		goto err_return;
	}

	ret = ida_simple_get(&nrf24_ida_pipe, 0, 0, GFP_KERNEL);
	if (ret < 0) {
		dev_err(&device->dev, "%s: get_minor failed\n", __func__);
		goto err_free_mem;
	}

	p->devt = MKDEV(MAJOR(nrf24_dev), ret);
	p->id = id;

	INIT_KFIFO(p->rx_fifo);
	init_waitqueue_head(&p->poll_wait_queue);

	p->dev = device_create_with_groups(nrf24_class,
					   &device->dev,
					   p->devt,
					   p,
					   nrf24_pipe_groups,
					   "%s.%d",
					   dev_name(&device->dev),
					   id);

	if (IS_ERR(p->dev)) {
		dev_err(&device->dev,
			"%s: device_create of '%s' failed\n",
			__func__,
			dev_name(p->dev));
		ret = PTR_ERR(p->dev);
		goto err_ida_remove;
	}

	cdev_init(&p->cdev, &nrf24_fops);
	p->cdev.owner = THIS_MODULE;
	ret = cdev_add(&p->cdev, p->devt, 1);
	if (ret < 0) {
		dev_err(&device->dev, "%s: cdev failed\n", __func__);
		goto err_dev_destroy;
	}

	dev_dbg(&device->dev,
		"%s: device created: major(%d), minor(%d)\n",
		__func__,
		MAJOR(p->devt),
		MINOR(p->devt));

	return p;

err_dev_destroy:
	device_destroy(nrf24_class, p->devt);
err_ida_remove:
	ida_simple_remove(&nrf24_ida_pipe, MINOR(p->devt));
err_free_mem:
	kfree(p);
err_return:
	return ERR_PTR(ret);
}

static void nrf24_gpio_free(struct nrf24_device *device)
{
	if (!IS_ERR(device->ce))
		gpiod_put(device->ce);

	free_irq(device->spi->irq, device);
}

static int nrf24_gpio_setup(struct nrf24_device *device)
{
	int ret;

	device->ce = gpiod_get(&device->spi->dev, "ce", 0);

	if (device->ce == ERR_PTR(-ENOENT))
		dev_dbg(&device->dev, "%s: no entry for CE\n", __func__);
	else if (device->ce == ERR_PTR(-EBUSY))
		dev_dbg(&device->dev, "%s: CE is busy\n", __func__);

	if (IS_ERR(device->ce)) {
		ret = PTR_ERR(device->ce);
		dev_err(&device->dev, "%s: CE gpio setup error\n", __func__);
		return ret;
	}

	nrf24_ce_lo(device);

	ret = request_irq(device->spi->irq,
			  nrf24_isr,
			  0,
			  dev_name(&device->dev),
			  device);
	if (ret < 0) {
		gpiod_put(device->ce);
		return ret;
	}

	return 0;
}

static void nrf24_dev_release(struct device *dev)
{
	struct nrf24_device *device = to_nrf24_device(dev);

	ida_simple_remove(&nrf24_ida_dev, device->id);
	kfree(device);
}

static struct device_type nrf24_dev_type = {
	.name = "nrf24_device",
	.release = nrf24_dev_release,
};

static struct nrf24_device *nrf24_dev_init(struct spi_device *spi)
{
	int ret;
	struct nrf24_device *device;
	int id;

	id = ida_simple_get(&nrf24_ida_dev, 0, 0, GFP_KERNEL);
	if (id < 0)
		return ERR_PTR(id);

	//sets flags to false as well
	device = kzalloc(sizeof(*device), GFP_KERNEL);
	if (!device) {
		ida_simple_remove(&nrf24_ida_dev, id);
		return ERR_PTR(-ENOMEM);
	}
	device->spi = spi;

	dev_set_name(&device->dev, "nrf%d", id);
	device->id = id;
	device->dev.parent = &spi->dev;
	device->dev.class = nrf24_class;
	device->dev.type = &nrf24_dev_type;
	device->dev.groups = nrf24_groups;
	ret = device_register(&device->dev);
	if (ret < 0) {
		put_device(&device->dev);
		ida_simple_remove(&nrf24_ida_dev, id);

		return ERR_PTR(ret);
	}

	init_waitqueue_head(&device->tx_wait_queue);
	init_waitqueue_head(&device->tx_done_wait_queue);
	init_waitqueue_head(&device->rx_wait_queue);

	INIT_WORK(&device->isr_work, nrf24_isr_work_handler);
	INIT_KFIFO(device->tx_fifo);
	spin_lock_init(&device->lock);
	mutex_init(&device->tx_fifo_mutex);

	INIT_LIST_HEAD(&device->pipes);

	return device;
}

static int nrf24_hal_init(struct nrf24_device *device)
{
	int ret;
	struct spi_device *spi = device->spi;
	struct nrf24_pipe *pipe;

	ret = nrf24_soft_reset(spi);
	if (ret < 0)
		return ret;

	list_for_each_entry(pipe, &device->pipes, list) {
		ret = nrf24_get_address(spi,
					pipe->id,
					(u8 *)&pipe->cfg.address);
		if (ret < 0)
			return ret;
		ret = nrf24_get_auto_ack(spi, pipe->id);
		if (ret < 0)
			return ret;
		pipe->cfg.ack = ret;

		//0 -> dynamic pload
		pipe->cfg.plw = 0;
		ret = nrf24_set_rx_pload_width(spi, pipe->id, 0);
		if (ret < 0)
			return ret;
	}

	ret = nrf24_flush_fifo(spi);
	if (ret < 0)
		return ret;
	ret = nrf24_open_pipe(spi, NRF24_PIPE_ALL);
	if (ret < 0)
		return ret;
	ret = nrf24_lock_unlock(spi);
	if (ret < 0)
		return ret;
	ret = nrf24_set_mode(spi, NRF24_MODE_RX);
	if (ret < 0)
		return ret;
	ret = nrf24_set_crc_mode(spi, NRF24_CRC_16BIT);
	if (ret < 0)
		return ret;
	ret = nrf24_set_auto_retr_count(spi, 15);
	if (ret < 0)
		return ret;
	ret = nrf24_set_auto_retr_delay(spi, 4000);
	if (ret < 0)
		return ret;
	ret = nrf24_set_rf_power(spi, NRF24_POWER_0DBM);
	if (ret < 0)
		return ret;
	ret = nrf24_set_datarate(spi, NRF24_DATARATE_2MBPS);
	if (ret < 0)
		return ret;
	ret = nrf24_power_up(spi);
	if (ret < 0)
		return ret;

	nrf24_ce_hi(device);

	return ret;
}

static int nrf24_probe(struct spi_device *spi)
{
	int ret;
	struct nrf24_device *device;
	struct nrf24_pipe *pipe;
	int i;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;

	ret = spi_setup(spi);
	if (ret < 0) {
		dev_err(&spi->dev, "%s: spi_setup failed\n", __func__);
		return ret;
	}

	device = nrf24_dev_init(spi);
	if (IS_ERR(device)) {
		dev_err(&device->spi->dev, "%s: dev_init failed\n", __func__);
		return PTR_ERR(device);
	}

	ret = nrf24_gpio_setup(device);
	if (ret < 0) {
		dev_err(&device->dev, "%s: gpio_setup failed\n", __func__);
		goto err_dev_unregister;
	}

	for (i = 0; i <= NRF24_PIPE5; i++) {
		pipe = nrf24_create_pipe(device, i);
		if (IS_ERR(pipe)) {
			ret = PTR_ERR(pipe);
			goto err_devs_destroy;
		}
		list_add(&pipe->list, &device->pipes);
	}

	ret = nrf24_hal_init(device);
	if (ret < 0)
		goto err_devs_destroy;

	device->rx_task_struct = kthread_run(nrf24_rx_thread,
					     device,
					     "nrf%d_rx_thread",
					     device->id);
	if (IS_ERR(device->rx_task_struct)) {
		dev_err(&device->dev, "start of tx thread failed\n");
		goto err_devs_destroy;
	}

	device->tx_task_struct = kthread_run(nrf24_tx_thread,
					     device,
					     "nrf%d_tx_thread",
					     device->id);
	if (IS_ERR(device->tx_task_struct)) {
		dev_err(&device->dev, "start of tx thread failed\n");
		goto err_kthread_stop;
	}

	spi_set_drvdata(spi, device);

	return 0;

err_kthread_stop:
	kthread_stop(device->rx_task_struct);
err_devs_destroy:
	nrf24_destroy_devices(device);
	nrf24_gpio_free(device);
err_dev_unregister:
	device_unregister(&device->dev);
	return ret;
}

static int nrf24_remove(struct spi_device *spi)
{
	struct nrf24_device *device = spi_get_drvdata(spi);

	nrf24_gpio_free(device);

	kthread_stop(device->tx_task_struct);
	kthread_stop(device->rx_task_struct);

	nrf24_destroy_devices(device);

	device_unregister(&device->dev);

	return 0;
}

static const struct of_device_id nrf24_dt_ids[] = {
	{ .compatible = "nordic,nrf24" },
	{},
};

MODULE_DEVICE_TABLE(of, nrf24_dt_ids);

static  struct spi_driver nrf24_spi_driver = {
	.driver = {
		.name = "nrf24",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(nrf24_dt_ids),
	},
	.probe = nrf24_probe,
	.remove = nrf24_remove,
};

static int __init nrf24_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&nrf24_dev, 0, N_NRF24_MINORS,
				  nrf24_spi_driver.driver.name);
	if (ret < 0) {
		pr_err("Unable to alloc chrdev region\n");
		goto err_ida_destroy;
	}

	nrf24_class = class_create(THIS_MODULE, nrf24_spi_driver.driver.name);
	if (IS_ERR(nrf24_class)) {
		pr_err("Unable to create class\n");
		ret = PTR_ERR(nrf24_class);
		goto err_unreg_chrdev;
	}

	ret = spi_register_driver(&nrf24_spi_driver);
	if (ret < 0) {
		pr_err("Unable to register spi driver\n");
		goto err_class_destroy;
	}

	return 0;

err_class_destroy:
	class_destroy(nrf24_class);
err_unreg_chrdev:
	unregister_chrdev(MAJOR(nrf24_dev), nrf24_spi_driver.driver.name);
err_ida_destroy:
	ida_destroy(&nrf24_ida_dev);
	ida_destroy(&nrf24_ida_pipe);

	return ret;
}
module_init(nrf24_init);

static void __exit nrf24_exit(void)
{
	spi_unregister_driver(&nrf24_spi_driver);
	class_destroy(nrf24_class);
	unregister_chrdev(MAJOR(nrf24_dev), nrf24_spi_driver.driver.name);
	ida_destroy(&nrf24_ida_dev);
	ida_destroy(&nrf24_ida_pipe);
}
module_exit(nrf24_exit);

MODULE_AUTHOR("Marcin Ciupak <marcin.s.ciupak@gmail.com>");
MODULE_DESCRIPTION("Driver for NRF24L01+");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:nrf24");

