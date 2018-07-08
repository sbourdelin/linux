// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx AXIS FIFO: interface to the Xilinx AXI-Stream FIFO IP core
 *
 * Copyright (C) 2018 Jacob Feder
 *
 * Authors:  Jacob Feder <jacobsfeder@gmail.com>
 *
 * See Xilinx PG080 document for IP details
 */

/* ----------------------------
 *           includes
 * ----------------------------
 */

#include "axis-fifo.h"

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/moduleparam.h>
#include <linux/interrupt.h>
#include <linux/param.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/jiffies.h>

#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>

/* ----------------------------
 *           globals
 * ----------------------------
 */

static unsigned int num_fifo_devices; /* number of initialized devices */
static struct mutex num_fifo_devices_mutex; /* mutex for num_fifo_devices */

static int read_timeout = 1000; /* ms to wait before read() times out */
static int write_timeout = 1000; /* ms to wait before write() times out */

/* ----------------------------
 * module command-line arguments
 * ----------------------------
 */

module_param(read_timeout, int, 0444);
MODULE_PARM_DESC(read_timeout, "ms to wait before blocking read() timing out; set to -1 for no timeout");
module_param(write_timeout, int, 0444);
MODULE_PARM_DESC(write_timeout, "ms to wait before blocking write() timing out; set to -1 for no timeout");

/* ----------------------------
 *         sysfs entries
 * ----------------------------
 */

static ssize_t sysfs_write(struct device *dev, const char *buf,
			   size_t count, unsigned int addr_offset)
{
	struct axis_fifo_local *device_wrapper = dev_get_drvdata(dev);

	if (!mutex_trylock(&device_wrapper->write_mutex)) {
		dev_err(device_wrapper->os_device,
			"couldn't acquire write lock\n");
		return -EBUSY;
	}

	if (!mutex_trylock(&device_wrapper->read_mutex)) {
		dev_err(device_wrapper->os_device,
			"couldn't acquire read lock\n");
		mutex_unlock(&device_wrapper->write_mutex);
		dev_dbg(device_wrapper->os_device, "released write lock\n");
		return -EBUSY;
	}

	dev_dbg(device_wrapper->os_device, "acquired locks\n");

	if (count != 4) {
		dev_err(device_wrapper->os_device,
			"error, sysfs write to address 0x%x expected 4 bytes\n",
			addr_offset);
		mutex_unlock(&device_wrapper->write_mutex);
		mutex_unlock(&device_wrapper->read_mutex);
		return -EINVAL;
	}

	dev_dbg(device_wrapper->os_device,
		"writing 0x%x to sysfs address 0x%x\n",
		*(unsigned int *)buf, addr_offset);
	iowrite32(*(unsigned int __force *)buf,
		  device_wrapper->base_addr + addr_offset);
	mutex_unlock(&device_wrapper->write_mutex);
	mutex_unlock(&device_wrapper->read_mutex);
	dev_dbg(device_wrapper->os_device, "released locks\n");

	return 4;
}

static ssize_t sysfs_read(struct device *dev, char *buf,
			  unsigned int addr_offset)
{
	struct axis_fifo_local *device_wrapper = dev_get_drvdata(dev);
	unsigned int read_val;

	if (!mutex_trylock(&device_wrapper->write_mutex)) {
		dev_err(device_wrapper->os_device,
			"couldn't acquire write lock\n");
		return -EBUSY;
	}

	if (!mutex_trylock(&device_wrapper->read_mutex)) {
		dev_err(device_wrapper->os_device,
			"couldn't acquire read lock\n");
		mutex_unlock(&device_wrapper->write_mutex);
		dev_dbg(device_wrapper->os_device, "released write lock\n");
		return -EBUSY;
	}

	dev_dbg(device_wrapper->os_device, "acquired locks\n");
	read_val = ioread32(device_wrapper->base_addr + addr_offset);
	dev_dbg(device_wrapper->os_device,
		"read 0x%x from sysfs address 0x%x\n",
		read_val, addr_offset);
	*(unsigned int __force *)buf = read_val;
	mutex_unlock(&device_wrapper->write_mutex);
	mutex_unlock(&device_wrapper->read_mutex);
	dev_dbg(device_wrapper->os_device, "released locks\n");

	return 4;
}

static ssize_t isr_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	return sysfs_write(dev, buf, count, XLLF_ISR_OFFSET);
}

static ssize_t isr_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sysfs_read(dev, buf, XLLF_ISR_OFFSET);
}

static DEVICE_ATTR_RW(isr);

static ssize_t ier_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	return sysfs_write(dev, buf, count, XLLF_IER_OFFSET);
}

static ssize_t ier_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sysfs_read(dev, buf, XLLF_IER_OFFSET);
}

static DEVICE_ATTR_RW(ier);

static ssize_t tdfr_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	return sysfs_write(dev, buf, count, XLLF_TDFR_OFFSET);
}

static DEVICE_ATTR_WO(tdfr);

static ssize_t tdfv_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	return sysfs_read(dev, buf, XLLF_TDFV_OFFSET);
}

static DEVICE_ATTR_RO(tdfv);

static ssize_t tdfd_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	return sysfs_write(dev, buf, count, XLLF_TDFD_OFFSET);
}

static DEVICE_ATTR_WO(tdfd);

static ssize_t tlr_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	return sysfs_write(dev, buf, count, XLLF_TLR_OFFSET);
}

static DEVICE_ATTR_WO(tlr);

static ssize_t rdfr_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	return sysfs_write(dev, buf, count, XLLF_RDFR_OFFSET);
}

static DEVICE_ATTR_WO(rdfr);

static ssize_t rdfo_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	return sysfs_read(dev, buf, XLLF_RDFO_OFFSET);
}

static DEVICE_ATTR_RO(rdfo);

static ssize_t rdfd_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	return sysfs_read(dev, buf, XLLF_RDFD_OFFSET);
}

static DEVICE_ATTR_RO(rdfd);

static ssize_t rlr_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sysfs_read(dev, buf, XLLF_RLR_OFFSET);
}

static DEVICE_ATTR_RO(rlr);

static ssize_t srr_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	return sysfs_write(dev, buf, count, XLLF_SRR_OFFSET);
}

static DEVICE_ATTR_WO(srr);

static ssize_t tdr_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	return sysfs_write(dev, buf, count, XLLF_TDR_OFFSET);
}

static DEVICE_ATTR_WO(tdr);

static ssize_t rdr_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return sysfs_read(dev, buf, XLLF_RDR_OFFSET);
}

static DEVICE_ATTR_RO(rdr);

static struct attribute *axis_fifo_attrs[] = {
	&dev_attr_isr.attr,
	&dev_attr_ier.attr,
	&dev_attr_tdfr.attr,
	&dev_attr_tdfv.attr,
	&dev_attr_tdfd.attr,
	&dev_attr_tlr.attr,
	&dev_attr_rdfr.attr,
	&dev_attr_rdfo.attr,
	&dev_attr_rdfd.attr,
	&dev_attr_rlr.attr,
	&dev_attr_srr.attr,
	&dev_attr_tdr.attr,
	&dev_attr_rdr.attr,
	NULL
};
ATTRIBUTE_GROUPS(axis_fifo);

/* ----------------------------
 *        implementation
 * ----------------------------
 */

static void reset_ip_core(struct axis_fifo_local *device_wrapper)
{
	iowrite32(XLLF_SRR_RESET_MASK,
		  device_wrapper->base_addr + XLLF_SRR_OFFSET);
	iowrite32(XLLF_TDFR_RESET_MASK,
		  device_wrapper->base_addr + XLLF_TDFR_OFFSET);
	iowrite32(XLLF_RDFR_RESET_MASK,
		  device_wrapper->base_addr + XLLF_RDFR_OFFSET);
	iowrite32(XLLF_INT_TC_MASK | XLLF_INT_RC_MASK | XLLF_INT_RPURE_MASK |
		  XLLF_INT_RPORE_MASK | XLLF_INT_RPUE_MASK |
		  XLLF_INT_TPOE_MASK | XLLF_INT_TSE_MASK,
		  device_wrapper->base_addr + XLLF_IER_OFFSET);
	iowrite32(XLLF_INT_ALL_MASK,
		  device_wrapper->base_addr + XLLF_ISR_OFFSET);
}

// reads a single packet from the fifo as dictated by the tlast signal
static ssize_t axis_fifo_read(struct file *device_file, char __user *buf,
			      size_t len, loff_t *off)
{
	struct axis_fifo_local *device_wrapper =
			(struct axis_fifo_local *)device_file->private_data;
	unsigned int bytes_available;
	unsigned int words_available;
	unsigned int word;
	unsigned int buff_word;
	int wait_ret;
	u32 read_buff[READ_BUFF_SIZE];

	if (device_wrapper->read_flags & O_NONBLOCK) {
		// opened in non-blocking mode
		// return if there are no packets available
		if (!ioread32(device_wrapper->base_addr + XLLF_RDFO_OFFSET))
			return -EAGAIN;
	} else {
		// opened in blocking mode

		// wait for a packet available interrupt (or timeout)
		// if nothing is currently available
		spin_lock_irq(&device_wrapper->read_queue_lock);
		if (read_timeout < 0) {
			wait_ret = wait_event_interruptible_lock_irq_timeout(
				device_wrapper->read_queue,
				ioread32(device_wrapper->base_addr +
					XLLF_RDFO_OFFSET),
				device_wrapper->read_queue_lock,
				MAX_SCHEDULE_TIMEOUT);
		} else {
			wait_ret = wait_event_interruptible_lock_irq_timeout(
				device_wrapper->read_queue,
				ioread32(device_wrapper->base_addr +
					XLLF_RDFO_OFFSET),
				device_wrapper->read_queue_lock,
				msecs_to_jiffies(read_timeout));
		}
		spin_unlock_irq(&device_wrapper->read_queue_lock);

		if (wait_ret == 0) {
			// timeout occurred
			dev_dbg(device_wrapper->os_device, "read timeout");
			return 0;
		} else if (wait_ret == -ERESTARTSYS) {
			// signal received
			return -ERESTARTSYS;
		} else if (wait_ret < 0) {
			dev_err(device_wrapper->os_device,
				"wait_event_interruptible_timeout() error in read (wait_ret=%i)\n",
				wait_ret);
			return wait_ret;
		}
	}

	bytes_available = ioread32(device_wrapper->base_addr + XLLF_RLR_OFFSET);
	if (!bytes_available) {
		dev_err(device_wrapper->os_device,
			"received a packet of length 0 - fifo core will be reset\n");
		reset_ip_core(device_wrapper);
		return -EIO;
	}

	if (bytes_available > len) {
		dev_err(device_wrapper->os_device,
			"user read buffer too small (available bytes=%u user buffer bytes=%u) - fifo core will be reset\n",
			bytes_available, len);
		reset_ip_core(device_wrapper);
		return -EINVAL;
	}

	if (bytes_available % 4) {
		// this probably can't happen unless IP
		// registers were previously mishandled
		dev_err(device_wrapper->os_device,
			"received a packet that isn't word-aligned - fifo core will be reset\n");
		reset_ip_core(device_wrapper);
		return -EIO;
	}

	words_available = bytes_available / 4;

	// read data into an intermediate buffer, copying the contents
	// to userspace when the buffer is full
	for (word = 0; word < words_available; word++) {
		buff_word = word % READ_BUFF_SIZE;
		read_buff[buff_word] = ioread32(device_wrapper->base_addr +
						XLLF_RDFD_OFFSET);
		if ((buff_word == READ_BUFF_SIZE - 1) ||
		    (word == words_available - 1)) {
			if (copy_to_user(buf + (word - buff_word) * 4,
					 read_buff,
					 (buff_word + 1) * 4)) {
				// this occurs if the user passes
				// an invalid pointer
				dev_err(device_wrapper->os_device,
					"couldn't copy data to userspace buffer - fifo core will be reset\n");
				reset_ip_core(device_wrapper);
				return -EFAULT;
			}
		}
	}

	return bytes_available;
}

static ssize_t axis_fifo_write(struct file *device_file, const char __user *buf,
			       size_t len, loff_t *off)
{
	struct axis_fifo_local *device_wrapper =
			(struct axis_fifo_local *)device_file->private_data;
	unsigned int words_to_write;
	unsigned int word;
	unsigned int buff_word;
	int wait_ret;
	u32 write_buff[WRITE_BUFF_SIZE];

	if (len % 4) {
		dev_err(device_wrapper->os_device,
			"tried to send a packet that isn't word-aligned\n");
		return -EINVAL;
	}

	words_to_write = len / 4;

	if (!words_to_write) {
		dev_err(device_wrapper->os_device,
			"tried to send a packet of length 0\n");
		return -EINVAL;
	}

	if (words_to_write > device_wrapper->tx_fifo_depth) {
		dev_err(device_wrapper->os_device, "tried to write more words [%u] than slots in the fifo buffer [%u]\n",
			words_to_write, device_wrapper->tx_fifo_depth);
		return -EINVAL;
	}

	if (device_wrapper->write_flags & O_NONBLOCK) {
		// opened in non-blocking mode
		// return if there is not enough room available in the fifo
		if (words_to_write > ioread32(device_wrapper->base_addr +
						XLLF_TDFV_OFFSET)) {
			return -EAGAIN;
		}
	} else {
		// opened in blocking mode

		// wait for an interrupt (or timeout) if there isn't
		// currently enough room in the fifo
		spin_lock_irq(&device_wrapper->write_queue_lock);
		if (write_timeout < 0) {
			wait_ret = wait_event_interruptible_lock_irq_timeout(
					device_wrapper->write_queue,
					ioread32(device_wrapper->base_addr +
					    XLLF_TDFV_OFFSET) >= words_to_write,
					device_wrapper->write_queue_lock,
					MAX_SCHEDULE_TIMEOUT);
		} else {
			wait_ret = wait_event_interruptible_lock_irq_timeout(
					device_wrapper->write_queue,
					ioread32(device_wrapper->base_addr +
					XLLF_TDFV_OFFSET) >= words_to_write,
					device_wrapper->write_queue_lock,
					msecs_to_jiffies(write_timeout));
		}
		spin_unlock_irq(&device_wrapper->write_queue_lock);

		if (wait_ret == 0) {
			// timeout occurred
			dev_dbg(device_wrapper->os_device, "write timeout\n");
			return 0;
		} else if (wait_ret == -ERESTARTSYS) {
			// signal received
			return -ERESTARTSYS;
		} else if (wait_ret < 0) {
			// unknown error
			dev_err(device_wrapper->os_device,
				"wait_event_interruptible_timeout() error in write (wait_ret=%i)\n",
				wait_ret);
			return wait_ret;
		}
	}

	// write data from an intermediate buffer into the fifo IP, refilling
	// the buffer with userspace data as needed
	for (word = 0; word < words_to_write; word++) {
		buff_word = word % WRITE_BUFF_SIZE;
		if (buff_word == 0) {
			if (copy_from_user(write_buff, buf + word * 4,
					   word <= words_to_write -
					   WRITE_BUFF_SIZE ?
					   WRITE_BUFF_SIZE * 4 :
					   (words_to_write % WRITE_BUFF_SIZE)
					   * 4)) {
				// this occurs if the user
				// passes an invalid pointer
				dev_err(device_wrapper->os_device,
					"couldn't copy data from userspace buffer - fifo core will be reset\n");
				reset_ip_core(device_wrapper);
				return -EFAULT;
			}
		}
		iowrite32(write_buff[buff_word],
			  device_wrapper->base_addr + XLLF_TDFD_OFFSET);
	}

	// write packet size to fifo
	iowrite32(words_to_write * 4,
		  device_wrapper->base_addr + XLLF_TLR_OFFSET);

	return (ssize_t)words_to_write * 4;
}

static irqreturn_t axis_fifo_irq(int irq, void *dw)
{
	struct axis_fifo_local *device_wrapper = (struct axis_fifo_local *)dw;
	unsigned int pending_interrupts;

	do {
		pending_interrupts = ioread32(device_wrapper->base_addr +
					      XLLF_IER_OFFSET) &
					      ioread32(device_wrapper->base_addr
					      + XLLF_ISR_OFFSET);
		if (pending_interrupts & XLLF_INT_RC_MASK) {
			// packet received

			// wake the reader process if it is waiting
			wake_up(&device_wrapper->read_queue);

			// clear interrupt
			iowrite32(XLLF_INT_RC_MASK & XLLF_INT_ALL_MASK,
				  device_wrapper->base_addr + XLLF_ISR_OFFSET);
		} else if (pending_interrupts & XLLF_INT_TC_MASK) {
			// packet sent

			// wake the writer process if it is waiting
			wake_up(&device_wrapper->write_queue);

			// clear interrupt
			iowrite32(XLLF_INT_TC_MASK & XLLF_INT_ALL_MASK,
				  device_wrapper->base_addr + XLLF_ISR_OFFSET);
		} else if (pending_interrupts & XLLF_INT_TFPF_MASK) {
			// transmit fifo programmable full

			// clear interrupt
			iowrite32(XLLF_INT_TFPF_MASK & XLLF_INT_ALL_MASK,
				  device_wrapper->base_addr + XLLF_ISR_OFFSET);
		} else if (pending_interrupts & XLLF_INT_TFPE_MASK) {
			// transmit fifo programmable empty

			// clear interrupt
			iowrite32(XLLF_INT_TFPE_MASK & XLLF_INT_ALL_MASK,
				  device_wrapper->base_addr + XLLF_ISR_OFFSET);
		} else if (pending_interrupts & XLLF_INT_RFPF_MASK) {
			// receive fifo programmable full

			// clear interrupt
			iowrite32(XLLF_INT_RFPF_MASK & XLLF_INT_ALL_MASK,
				  device_wrapper->base_addr + XLLF_ISR_OFFSET);
		} else if (pending_interrupts & XLLF_INT_RFPE_MASK) {
			// receive fifo programmable empty

			// clear interrupt
			iowrite32(XLLF_INT_RFPE_MASK & XLLF_INT_ALL_MASK,
				  device_wrapper->base_addr + XLLF_ISR_OFFSET);
		} else if (pending_interrupts & XLLF_INT_TRC_MASK) {
			// transmit reset complete interrupt

			// clear interrupt
			iowrite32(XLLF_INT_TRC_MASK & XLLF_INT_ALL_MASK,
				  device_wrapper->base_addr + XLLF_ISR_OFFSET);
		} else if (pending_interrupts & XLLF_INT_RRC_MASK) {
			// receive reset complete interrupt

			// clear interrupt
			iowrite32(XLLF_INT_RRC_MASK & XLLF_INT_ALL_MASK,
				  device_wrapper->base_addr + XLLF_ISR_OFFSET);
		} else if (pending_interrupts & XLLF_INT_RPURE_MASK) {
			// receive fifo under-read error interrupt
			dev_err(device_wrapper->os_device,
				"receive under-read interrupt\n");

			// clear interrupt
			iowrite32(XLLF_INT_RPURE_MASK & XLLF_INT_ALL_MASK,
				  device_wrapper->base_addr + XLLF_ISR_OFFSET);
		} else if (pending_interrupts & XLLF_INT_RPORE_MASK) {
			// receive over-read error interrupt
			dev_err(device_wrapper->os_device,
				"receive over-read interrupt\n");

			// clear interrupt
			iowrite32(XLLF_INT_RPORE_MASK & XLLF_INT_ALL_MASK,
				  device_wrapper->base_addr + XLLF_ISR_OFFSET);
		} else if (pending_interrupts & XLLF_INT_RPUE_MASK) {
			// receive underrun error interrupt
			dev_err(device_wrapper->os_device,
				"receive underrun error interrupt\n");

			// clear interrupt
			iowrite32(XLLF_INT_RPUE_MASK & XLLF_INT_ALL_MASK,
				  device_wrapper->base_addr + XLLF_ISR_OFFSET);
		} else if (pending_interrupts & XLLF_INT_TPOE_MASK) {
			// transmit overrun error interrupt
			dev_err(device_wrapper->os_device,
				"transmit overrun error interrupt\n");

			// clear interrupt
			iowrite32(XLLF_INT_TPOE_MASK & XLLF_INT_ALL_MASK,
				  device_wrapper->base_addr + XLLF_ISR_OFFSET);
		} else if (pending_interrupts & XLLF_INT_TSE_MASK) {
			// transmit length mismatch error interrupt
			dev_err(device_wrapper->os_device,
				"transmit length mismatch error interrupt\n");

			// clear interrupt
			iowrite32(XLLF_INT_TSE_MASK & XLLF_INT_ALL_MASK,
				  device_wrapper->base_addr + XLLF_ISR_OFFSET);
		} else if (pending_interrupts) {
			// unknown interrupt type
			dev_err(device_wrapper->os_device,
				"unknown interrupt(s) 0x%x\n",
				pending_interrupts);

			// clear interrupt
			iowrite32(XLLF_INT_ALL_MASK,
				  device_wrapper->base_addr + XLLF_ISR_OFFSET);
		}
	} while (pending_interrupts);

	return IRQ_HANDLED;
}

static int axis_fifo_open(struct inode *inod, struct file *device_file)
{
	struct axis_fifo_local *device_wrapper =
			(struct axis_fifo_local *)container_of(inod->i_cdev,
					struct axis_fifo_local, char_device);
	// set file attribute to our device wrapper so other
	// functions (e.g. read/write) can access it
	device_file->private_data = device_wrapper;

	dev_dbg(device_wrapper->os_device, "opening...\n");

	// lock write access
	if (((device_file->f_flags & O_ACCMODE) == O_WRONLY)) {
		if (!device_wrapper->has_tx_fifo) {
			dev_err(device_wrapper->os_device,
				"tried to open device for write but the transmit fifo is disabled\n");
			return -EPERM;
		}

		if (!mutex_trylock(&device_wrapper->write_mutex)) {
			dev_err(device_wrapper->os_device, "couldn't acquire write lock\n");
			return -EBUSY;
		}
		device_wrapper->write_flags = device_file->f_flags;

		dev_dbg(device_wrapper->os_device, "acquired write lock\n");
	}

	// lock read access
	if ((device_file->f_flags & O_ACCMODE) == O_RDONLY) {
		if (!device_wrapper->has_rx_fifo) {
			dev_err(device_wrapper->os_device,
				"tried to open device for read but the receive fifo is disabled\n");
			return -EPERM;
		}

		if (!mutex_trylock(&device_wrapper->read_mutex)) {
			dev_err(device_wrapper->os_device,
				"couldn't acquire read lock\n");
			return -EBUSY;
		}
		device_wrapper->read_flags = device_file->f_flags;

		dev_dbg(device_wrapper->os_device, "acquired read lock\n");
	}

	// lock read +  write access
	if ((device_file->f_flags & O_ACCMODE) == O_RDWR) {
		if (!device_wrapper->has_rx_fifo ||
		    !device_wrapper->has_tx_fifo) {
			dev_err(device_wrapper->os_device,
				"tried to open device for read/write but one or both of the receive/transmit fifos are disabled\n");
			return -EPERM;
		}

		if (!mutex_trylock(&device_wrapper->write_mutex)) {
			dev_err(device_wrapper->os_device,
				"couldn't acquire write lock\n");
			return -EBUSY;
		}
		if (!mutex_trylock(&device_wrapper->read_mutex)) {
			dev_err(device_wrapper->os_device,
				"couldn't acquire read lock\n");
			mutex_unlock(&device_wrapper->write_mutex);
			dev_dbg(device_wrapper->os_device,
				"released write lock\n");
			return -EBUSY;
		}
		device_wrapper->write_flags = device_file->f_flags;
		device_wrapper->read_flags = device_file->f_flags;

		dev_dbg(device_wrapper->os_device, "acquired write lock\n");
		dev_dbg(device_wrapper->os_device, "acquired read lock\n");
	}

	dev_dbg(device_wrapper->os_device, "opened\n");

	return 0;
}

static int axis_fifo_close(struct inode *inod, struct file *device_file)
{
	struct axis_fifo_local *device_wrapper = container_of(inod->i_cdev,
							struct axis_fifo_local,
							char_device);
	// unset file attribute
	device_file->private_data = NULL;

	dev_dbg(device_wrapper->os_device, "closing...\n");

	// unlock write access
	if (((device_file->f_flags & O_ACCMODE) == O_WRONLY) ||
	    ((device_file->f_flags & O_ACCMODE) == O_RDWR)) {
		mutex_unlock(&device_wrapper->write_mutex);
		dev_dbg(device_wrapper->os_device, "released write lock\n");
	}

	// unlock read access
	if (((device_file->f_flags & O_ACCMODE) == O_RDONLY) ||
	    ((device_file->f_flags & O_ACCMODE) == O_RDWR)) {
		mutex_unlock(&device_wrapper->read_mutex);
		dev_dbg(device_wrapper->os_device, "released read lock\n");
	}

	dev_dbg(device_wrapper->os_device, "closed\n");

	return 0;
}

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = axis_fifo_open,
	.release = axis_fifo_close,
	.read = axis_fifo_read,
	.write = axis_fifo_write
};

// read named property from the device tree
static int get_dts_property(struct axis_fifo_local *device_wrapper,
			    char *name, unsigned int *var)
{
	if (of_property_read_u32(device_wrapper->os_device->of_node,
				 name, var) < 0) {
		dev_err(device_wrapper->os_device,
			"couldn't read IP dts property '%s'", name);
		return -1;
	}
	dev_dbg(device_wrapper->os_device, "dts property '%s' = %u\n",
		name, *var);

	return 0;
}

static int axis_fifo_probe(struct platform_device *pdev)
{
	struct resource *r_irq; /* interrupt resources */
	struct resource *r_mem; /* IO mem resources */
	struct device *dev = &pdev->dev; /* OS device (from device tree) */
	struct axis_fifo_local *device_wrapper = NULL;

	char device_name[32];
	char class_name[32];

	int rc = 0; /* error return value */

	// IP properties from device tree
	unsigned int rxd_tdata_width;
	unsigned int txc_tdata_width;
	unsigned int txd_tdata_width;
	unsigned int tdest_width;
	unsigned int tid_width;
	unsigned int tuser_width;
	unsigned int data_interface_type;
	unsigned int has_tdest;
	unsigned int has_tid;
	unsigned int has_tkeep;
	unsigned int has_tstrb;
	unsigned int has_tuser;
	unsigned int rx_fifo_depth;
	unsigned int rx_programmable_empty_threshold;
	unsigned int rx_programmable_full_threshold;
	unsigned int axi_id_width;
	unsigned int axi4_data_width;
	unsigned int select_xpm;
	unsigned int tx_fifo_depth;
	unsigned int tx_programmable_empty_threshold;
	unsigned int tx_programmable_full_threshold;
	unsigned int use_rx_cut_through;
	unsigned int use_rx_data;
	unsigned int use_tx_control;
	unsigned int use_tx_cut_through;
	unsigned int use_tx_data;

	/* ----------------------------
	 *     init wrapper device
	 * ----------------------------
	 */

	// allocate device wrapper memory
	device_wrapper = devm_kmalloc(dev, sizeof(*device_wrapper), GFP_KERNEL);
	if (!device_wrapper)
		return -ENOMEM;

	dev_set_drvdata(dev, device_wrapper);
	device_wrapper->os_device = dev;

	// get unique device id
	mutex_lock(&num_fifo_devices_mutex);
	device_wrapper->id = num_fifo_devices;
	num_fifo_devices++;
	mutex_unlock(&num_fifo_devices_mutex);

	dev_dbg(device_wrapper->os_device, "acquired device number %i\n",
		device_wrapper->id);

	mutex_init(&device_wrapper->read_mutex);
	mutex_init(&device_wrapper->write_mutex);

	dev_dbg(device_wrapper->os_device, "initialized mutexes\n");

	init_waitqueue_head(&device_wrapper->read_queue);
	init_waitqueue_head(&device_wrapper->write_queue);

	dev_dbg(device_wrapper->os_device, "initialized queues\n");

	spin_lock_init(&device_wrapper->read_queue_lock);
	spin_lock_init(&device_wrapper->write_queue_lock);

	dev_dbg(device_wrapper->os_device, "initialized spinlocks\n");

	// create unique device and class names
	snprintf(device_name, 32, DRIVER_NAME "%i", device_wrapper->id);
	snprintf(class_name, 32, DRIVER_NAME "%i_class", device_wrapper->id);

	dev_dbg(device_wrapper->os_device, "device name [%s] class name [%s]\n",
		device_name, class_name);

	/* ----------------------------
	 *   init device memory space
	 * ----------------------------
	 */

	// get iospace for the device
	r_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r_mem) {
		dev_err(device_wrapper->os_device, "invalid address\n");
		rc = -ENODEV;
		goto err_initial;
	}

	device_wrapper->mem_start = r_mem->start;
	device_wrapper->mem_end = r_mem->end;

	// request physical memory
	if (!request_mem_region(device_wrapper->mem_start,
				device_wrapper->mem_end -
				device_wrapper->mem_start + 1,
				DRIVER_NAME)) {
		dev_err(device_wrapper->os_device,
			"couldn't lock memory region at %p\n",
			(void *)device_wrapper->mem_start);
		rc = -EBUSY;
		goto err_initial;
	}
	dev_dbg(device_wrapper->os_device,
		"got memory location [0x%x - 0x%x]\n",
		device_wrapper->mem_start, device_wrapper->mem_end);

	// map physical memory to kernel virtual address space
	device_wrapper->base_addr = ioremap(device_wrapper->mem_start,
					    device_wrapper->mem_end -
					    device_wrapper->mem_start + 1);

	if (!device_wrapper->base_addr) {
		dev_err(device_wrapper->os_device,
			"couldn't map physical memory\n");
		rc = -EIO;
		goto err_mem;
	}
	dev_dbg(device_wrapper->os_device, "remapped memory to 0x%x\n",
		(unsigned int)device_wrapper->base_addr);

	/* ----------------------------
	 *          init IP
	 * ----------------------------
	 */

	// retrieve device tree properties
	if (get_dts_property(device_wrapper, "xlnx,axi-str-rxd-tdata-width",
			     &rxd_tdata_width)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,axi-str-txc-tdata-width",
			     &txc_tdata_width)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,axi-str-txd-tdata-width",
			     &txd_tdata_width)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,axis-tdest-width",
			     &tdest_width)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,axis-tid-width",
			     &tid_width)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,axis-tuser-width",
			     &tuser_width)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,data-interface-type",
			     &data_interface_type)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,has-axis-tdest",
			     &has_tdest)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,has-axis-tid",
			     &has_tid)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,has-axis-tkeep",
			     &has_tkeep)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,has-axis-tstrb",
			     &has_tstrb)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,has-axis-tuser",
			     &has_tuser)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,rx-fifo-depth",
			     &rx_fifo_depth)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,rx-fifo-pe-threshold",
			     &rx_programmable_empty_threshold)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,rx-fifo-pf-threshold",
			     &rx_programmable_full_threshold)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,s-axi-id-width",
			     &axi_id_width)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,s-axi4-data-width",
			     &axi4_data_width)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,select-xpm",
			     &select_xpm)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,tx-fifo-depth",
			     &tx_fifo_depth)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,tx-fifo-pe-threshold",
			     &tx_programmable_empty_threshold)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,tx-fifo-pf-threshold",
			     &tx_programmable_full_threshold)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,use-rx-cut-through",
			     &use_rx_cut_through)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,use-rx-data",
			     &use_rx_data)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,use-tx-ctrl",
			     &use_tx_control)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,use-tx-cut-through",
			     &use_tx_cut_through)) {
		rc = -EIO;
		goto err_mem;
	}
	if (get_dts_property(device_wrapper, "xlnx,use-tx-data",
			     &use_tx_data)) {
		rc = -EIO;
		goto err_mem;
	}

	// check validity of device tree properties
	if (rxd_tdata_width != 32) {
		dev_err(device_wrapper->os_device,
			"rxd_tdata_width width [%u] unsupported\n",
			rxd_tdata_width);
		rc = -EIO;
		goto err_mem;
	}
	if (txd_tdata_width != 32) {
		dev_err(device_wrapper->os_device,
			"txd_tdata_width width [%u] unsupported\n",
			txd_tdata_width);
		rc = -EIO;
		goto err_mem;
	}
	if (has_tdest) {
		dev_err(device_wrapper->os_device, "tdest not supported\n");
		rc = -EIO;
		goto err_mem;
	}
	if (has_tid) {
		dev_err(device_wrapper->os_device, "tid not supported\n");
		rc = -EIO;
		goto err_mem;
	}
	if (has_tkeep) {
		dev_err(device_wrapper->os_device, "tkeep not supported\n");
		rc = -EIO;
		goto err_mem;
	}
	if (has_tstrb) {
		dev_err(device_wrapper->os_device, "tstrb not supported\n");
		rc = -EIO;
		goto err_mem;
	}
	if (has_tuser) {
		dev_err(device_wrapper->os_device, "tuser not supported\n");
		rc = -EIO;
		goto err_mem;
	}
	if (use_rx_cut_through) {
		dev_err(device_wrapper->os_device,
			"rx cut-through not supported\n");
		rc = -EIO;
		goto err_mem;
	}
	if (use_tx_cut_through) {
		dev_err(device_wrapper->os_device,
			"tx cut-through not supported\n");
		rc = -EIO;
		goto err_mem;
	}
	if (use_tx_control) {
		dev_err(device_wrapper->os_device,
			"tx control not supported\n");
		rc = -EIO;
		goto err_mem;
	}

	/* TODO
	 * these exist in the device tree but it's unclear what they do
	 * - select-xpm
	 * - data-interface-type
	 */

	// set device wrapper properties based on IP config
	device_wrapper->rx_fifo_depth = rx_fifo_depth;
	// IP sets TDFV to fifo depth - 4 so we will do the same
	device_wrapper->tx_fifo_depth = tx_fifo_depth - 4;
	device_wrapper->has_rx_fifo = use_rx_data;
	device_wrapper->has_tx_fifo = use_tx_data;

	reset_ip_core(device_wrapper);

	/* ----------------------------
	 *    init device interrupts
	 * ----------------------------
	 */

	// get IRQ resource
	r_irq = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!r_irq) {
		dev_err(device_wrapper->os_device,
			"no IRQ found at 0x%08x mapped to 0x%08x\n",
			(unsigned int __force)device_wrapper->mem_start,
			(unsigned int __force)device_wrapper->base_addr);
		rc = -EIO;
		goto err_mem;
	}
	dev_dbg(device_wrapper->os_device, "found IRQ\n");

	// request IRQ
	device_wrapper->irq = r_irq->start;
	rc = request_irq(device_wrapper->irq, &axis_fifo_irq, 0,
			 DRIVER_NAME, device_wrapper);
	if (rc) {
		dev_err(device_wrapper->os_device,
			"couldn't allocate interrupt %i\n",
			device_wrapper->irq);
		rc = -EIO;
		goto err_mem;
	}
	dev_dbg(device_wrapper->os_device,
		"initialized IRQ %i\n",
		device_wrapper->irq);

	/* ----------------------------
	 *      init char device
	 * ----------------------------
	 */

	// allocate device number
	if (alloc_chrdev_region(&device_wrapper->devt, 0, 1, DRIVER_NAME) < 0) {
		dev_err(device_wrapper->os_device, "couldn't allocate dev_t\n");
		rc = -EIO;
		goto err_irq;
	}
	dev_dbg(device_wrapper->os_device,
		"allocated device number major %i minor %i\n",
		MAJOR(device_wrapper->devt), MINOR(device_wrapper->devt));

	// create driver class
	device_wrapper->driver_class = NULL;
	device_wrapper->driver_class = class_create(THIS_MODULE, class_name);
	if (!device_wrapper->driver_class) {
		dev_err(device_wrapper->os_device,
			"couldn't create driver class\n");
		rc = -EIO;
		goto err_chrdev_region;
	}
	dev_dbg(device_wrapper->os_device, "created driver class\n");

	// create driver file
	device_wrapper->device = NULL;
	device_wrapper->device = device_create(device_wrapper->driver_class,
					       NULL, device_wrapper->devt,
					       NULL, device_name);
	if (!device_wrapper->device) {
		dev_err(device_wrapper->os_device,
			"couldn't create driver file\n");
		rc = -EIO;
		goto err_class;
	}
	dev_set_drvdata(device_wrapper->device, device_wrapper);
	dev_dbg(device_wrapper->os_device, "created device file\n");

	// create character device
	cdev_init(&device_wrapper->char_device, &fops);
	if (cdev_add(&device_wrapper->char_device,
		     device_wrapper->devt, 1) < 0) {
		dev_err(device_wrapper->os_device,
			"couldn't create character device\n");
		rc = -EIO;
		goto err_dev;
	}
	dev_dbg(device_wrapper->os_device, "created character device\n");

	dev_info(device_wrapper->os_device,
		 "axis-fifo created at 0x%08x mapped to 0x%08x, irq=%i, major=%i, minor=%i\n",
		 (unsigned int __force)device_wrapper->mem_start,
		 (unsigned int __force)device_wrapper->base_addr,
		 device_wrapper->irq, MAJOR(device_wrapper->devt),
		 MINOR(device_wrapper->devt));

	return 0;

err_dev:
	dev_set_drvdata(device_wrapper->device, NULL);
	device_destroy(device_wrapper->driver_class, device_wrapper->devt);
err_class:
	class_destroy(device_wrapper->driver_class);
err_chrdev_region:
	unregister_chrdev_region(device_wrapper->devt, 1);
err_irq:
	free_irq(device_wrapper->irq, device_wrapper);
err_mem:
	release_mem_region(device_wrapper->mem_start,
			   device_wrapper->mem_end -
			   device_wrapper->mem_start + 1);
err_initial:
	mutex_destroy(&device_wrapper->read_mutex);
	mutex_destroy(&device_wrapper->write_mutex);
	dev_set_drvdata(dev, NULL);
	return rc;
}

static int axis_fifo_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct axis_fifo_local *device_wrapper = dev_get_drvdata(dev);

	dev_info(dev, "removing\n");

	cdev_del(&device_wrapper->char_device);
	dev_set_drvdata(device_wrapper->device, NULL);
	device_destroy(device_wrapper->driver_class, device_wrapper->devt);
	class_destroy(device_wrapper->driver_class);
	unregister_chrdev_region(device_wrapper->devt, 1);
	free_irq(device_wrapper->irq, device_wrapper);
	release_mem_region(device_wrapper->mem_start,
			   device_wrapper->mem_end -
			   device_wrapper->mem_start + 1);
	mutex_destroy(&device_wrapper->read_mutex);
	mutex_destroy(&device_wrapper->write_mutex);
	dev_set_drvdata(dev, NULL);
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id axis_fifo_of_match[] = {
	{ .compatible = "xlnx,axi-fifo-mm-s-4.1", },
	{ },
};
MODULE_DEVICE_TABLE(of, axis_fifo_of_match);
#else
# define axis_fifo_of_match
#endif

static struct platform_driver axis_fifo_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table	= axis_fifo_of_match,
	},
	.probe		= axis_fifo_probe,
	.remove		= axis_fifo_remove,
};

static int __init axis_fifo_init(void)
{
	printk(KERN_INFO "axis-fifo driver loaded with parameters read_timeout = %i, write_timeout = %i\n",
	       read_timeout, write_timeout);
	mutex_init(&num_fifo_devices_mutex);
	num_fifo_devices = 0;
	return platform_driver_register(&axis_fifo_driver);
}

static void __exit axis_fifo_exit(void)
{
	platform_driver_unregister(&axis_fifo_driver);
	mutex_destroy(&num_fifo_devices_mutex);
	printk(KERN_INFO "axis-fifo driver exit\n");
}

module_init(axis_fifo_init);
module_exit(axis_fifo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jacob Feder <jacobsfeder@gmail.com>");
MODULE_DESCRIPTION("axis-fifo: Xilinx AXI-Stream FIFO v4.1 IP core driver\
\
This IP core has read and write AXI-Stream FIFOs, the contents of which can\
be accessed from the AXI4 memory-mapped interface. This is useful for\
transferring data from a processor into the FPGA fabric. The driver creates\
a character device that can be read/written to with standard\
open/read/write/close.\
\
See Xilinx PG080 document for IP details.\
\
Currently supports only store-forward mode with a 32-bit\
AXI4-Lite interface. DOES NOT support:\
	- cut-through mode\
	- AXI4 (non-lite)");
