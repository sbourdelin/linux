/*
 * drivers/misc/goldfish_audio.c
 *
 * Copyright (C) 2007 Google, Inc.
 * Copyright (C) 2012 Intel, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/sched.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/goldfish.h>
#include <linux/acpi.h>

MODULE_AUTHOR("Google, Inc.");
MODULE_DESCRIPTION("Android QEMU Audio Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");

struct goldfish_audio {
	char __iomem *reg_base;
	int irq;

	/* lock protects access to buffer_status and to device registers */
	spinlock_t lock;
	wait_queue_head_t wait;

	char *buffer_virt;		/* combined buffer virtual address */
	unsigned long buffer_phys;	/* combined buffer physical address */

	char *write_buffer1;		/* write buffer 1 virtual address */
	char *write_buffer2;		/* write buffer 2 virtual address */
	char *read_buffer;		/* read buffer virtual address */
	int buffer_status;
	int read_supported;	/* true if we have audio input support */

	int open_count;
	struct mutex mutex;	/* protects open/read/write/release calls */
};

/*
 *  We will allocate two read buffers and two write buffers.
 *  Having two read buffers facilitate stereo -> mono conversion.
 *  Having two write buffers facilitate interleaved IO.
 */
#define READ_BUFFER_SIZE	16384
#define WRITE_BUFFER_SIZE	16384
#define COMBINED_BUFFER_SIZE	((2 * READ_BUFFER_SIZE) + \
					(2 * WRITE_BUFFER_SIZE))

enum {
	/* audio status register */
	AUDIO_INT_STATUS	= 0x00,
	/* set this to enable IRQ */
	AUDIO_INT_ENABLE	= 0x04,
	/* set these to specify buffer addresses */
	AUDIO_SET_WRITE_BUFFER_1 = 0x08,
	AUDIO_SET_WRITE_BUFFER_2 = 0x0C,
	/* set number of bytes in buffer to write */
	AUDIO_WRITE_BUFFER_1  = 0x10,
	AUDIO_WRITE_BUFFER_2  = 0x14,
	AUDIO_SET_WRITE_BUFFER_1_HIGH = 0x28,
	AUDIO_SET_WRITE_BUFFER_2_HIGH = 0x30,

	/* true if audio input is supported */
	AUDIO_READ_SUPPORTED = 0x18,
	/* buffer to use for audio input */
	AUDIO_SET_READ_BUFFER = 0x1C,
	AUDIO_SET_READ_BUFFER_HIGH = 0x34,

	/* driver writes number of bytes to read */
	AUDIO_START_READ  = 0x20,

	/* number of bytes available in read buffer */
	AUDIO_READ_BUFFER_AVAILABLE  = 0x24,

	/* AUDIO_INT_STATUS bits */

	/* this bit set when it is safe to write more bytes to the buffer */
	AUDIO_INT_WRITE_BUFFER_1_EMPTY	= 1U << 0,
	AUDIO_INT_WRITE_BUFFER_2_EMPTY	= 1U << 1,
	AUDIO_INT_READ_BUFFER_FULL	= 1U << 2,

	AUDIO_INT_MASK			= AUDIO_INT_WRITE_BUFFER_1_EMPTY |
					  AUDIO_INT_WRITE_BUFFER_2_EMPTY |
					  AUDIO_INT_READ_BUFFER_FULL,
};

static unsigned int audio_read(const struct goldfish_audio *data, int addr)
{
	return readl(data->reg_base + addr);
}

static void audio_write(const struct goldfish_audio *data,
			int addr, unsigned int x)
{
	writel(x, data->reg_base + addr);
}

static void audio_write64(const struct goldfish_audio *data,
			  int addr_lo, int addr_hi, unsigned int x)
{
	char __iomem *reg_base = data->reg_base;

	gf_write_dma_addr(x, reg_base + addr_lo, reg_base + addr_hi);
}

static ssize_t goldfish_audio_read(struct file *fp, char __user *buf,
				   size_t count, loff_t *pos)
{
	struct goldfish_audio *audio = fp->private_data;
	unsigned long irq_flags;
	ssize_t result = 0;

	if (!audio)
		return -ENODEV;

	if (!audio->read_supported)
		return -ENODEV;

	while (count > 0) {
		unsigned int length = min_t(size_t, count, READ_BUFFER_SIZE);

		audio_write(audio, AUDIO_START_READ, length);
		wait_event_interruptible(audio->wait,
					 audio->buffer_status &
						AUDIO_INT_READ_BUFFER_FULL);

		spin_lock_irqsave(&audio->lock, irq_flags);
		audio->buffer_status &= ~AUDIO_INT_READ_BUFFER_FULL;
		spin_unlock_irqrestore(&audio->lock, irq_flags);

		length = audio_read(audio, AUDIO_READ_BUFFER_AVAILABLE);

		/* copy data to user space */
		if (copy_to_user(buf, audio->read_buffer, length))
			return -EFAULT;

		result += length;
		buf += length;
		count -= length;
	}
	return result;
}

static ssize_t goldfish_audio_write(struct file *fp, const char __user *buf,
				    size_t count, loff_t *pos)
{
	struct goldfish_audio *audio = fp->private_data;
	unsigned long irq_flags;
	ssize_t result = 0;
	char *kbuf;

	if (!audio)
		return -ENODEV;

	while (count > 0) {
		unsigned int length = min_t(size_t, count, WRITE_BUFFER_SIZE);

		wait_event_interruptible(audio->wait,
					 audio->buffer_status &
					 (AUDIO_INT_WRITE_BUFFER_1_EMPTY |
					  AUDIO_INT_WRITE_BUFFER_2_EMPTY));

		if ((audio->buffer_status & AUDIO_INT_WRITE_BUFFER_1_EMPTY) != 0)
			kbuf = audio->write_buffer1;
		else
			kbuf = audio->write_buffer2;

		/* copy from user space to the appropriate buffer */
		if (copy_from_user(kbuf, buf, length)) {
			result = -EFAULT;
			break;
		}

		spin_lock_irqsave(&audio->lock, irq_flags);
		/*
		 *  clear the buffer empty flag, and signal the emulator
		 *  to start writing the buffer
		 */
		if (kbuf == audio->write_buffer1) {
			audio->buffer_status &= ~AUDIO_INT_WRITE_BUFFER_1_EMPTY;
			audio_write(audio, AUDIO_WRITE_BUFFER_1, length);
		} else {
			audio->buffer_status &= ~AUDIO_INT_WRITE_BUFFER_2_EMPTY;
			audio_write(audio, AUDIO_WRITE_BUFFER_2, length);
		}
		spin_unlock_irqrestore(&audio->lock, irq_flags);

		buf += length;
		result += length;
		count -= length;
	}
	return result;
}

static int goldfish_audio_open(struct inode *ip, struct file *fp)
{
	struct goldfish_audio *audio = fp->private_data;
	int status;

	if (!audio)
		return -ENODEV;

	status = mutex_lock_interruptible(&audio->mutex);
	if (status)
		return status;

	if (audio->open_count) {
		status = -EBUSY;
		goto done;
	}

	++audio->open_count;
	audio->buffer_status = (AUDIO_INT_WRITE_BUFFER_1_EMPTY |
				AUDIO_INT_WRITE_BUFFER_2_EMPTY);
	audio_write(audio, AUDIO_INT_ENABLE, AUDIO_INT_MASK);
	fp->private_data = audio;

done:
	mutex_unlock(&audio->mutex);
	return status;
}

static int goldfish_audio_release(struct inode *ip, struct file *fp)
{
	struct goldfish_audio *audio = fp->private_data;
	int status;

	if (!audio)
		return -ENODEV;

	status = mutex_lock_interruptible(&audio->mutex);
	if (status)
		return status;

	--audio->open_count;
	if (!audio->open_count)
		audio_write(audio, AUDIO_INT_ENABLE, 0);

	mutex_unlock(&audio->mutex);
	return 0;
}

static irqreturn_t goldfish_audio_interrupt(int irq, void *dev_id)
{
	unsigned long irq_flags;
	struct goldfish_audio *audio = dev_id;
	u32 status;

	spin_lock_irqsave(&audio->lock, irq_flags);

	/* read buffer status flags */
	status = audio_read(audio, AUDIO_INT_STATUS);
	status &= AUDIO_INT_MASK;

	/*
	 *  if buffers are newly empty, wake up blocked
	 *  goldfish_audio_write() call
	 */
	if (status) {
		audio->buffer_status = status;
		wake_up(&audio->wait);
	}

	spin_unlock_irqrestore(&audio->lock, irq_flags);
	return status ? IRQ_HANDLED : IRQ_NONE;
}

/* file operations for /dev/eac */
static const struct file_operations goldfish_audio_fops = {
	.owner = THIS_MODULE,
	.read = goldfish_audio_read,
	.write = goldfish_audio_write,
	.open = goldfish_audio_open,
	.release = goldfish_audio_release,
};

static struct miscdevice goldfish_audio_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "eac",
	.fops = &goldfish_audio_fops,
};

static int goldfish_audio_probe(struct platform_device *pdev)
{
	int ret;
	struct resource *r;
	struct goldfish_audio *audio;
	dma_addr_t buf_addr;

	audio = devm_kzalloc(&pdev->dev, sizeof(*audio), GFP_KERNEL);
	if (!audio)
		return -ENOMEM;

	spin_lock_init(&audio->lock);
	mutex_init(&audio->mutex);
	init_waitqueue_head(&audio->wait);
	platform_set_drvdata(pdev, audio);

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r) {
		dev_err(&pdev->dev, "platform_get_resource failed\n");
		return -ENODEV;
	}
	audio->reg_base = devm_ioremap(&pdev->dev, r->start, PAGE_SIZE);
	if (!audio->reg_base)
		return -ENOMEM;

	audio->irq = platform_get_irq(pdev, 0);
	if (audio->irq < 0) {
		dev_err(&pdev->dev, "platform_get_irq failed\n");
		return -ENODEV;
	}
	audio->buffer_virt = dmam_alloc_coherent(&pdev->dev,
						 COMBINED_BUFFER_SIZE,
						 &buf_addr, GFP_KERNEL);
	if (!audio->buffer_virt) {
		dev_err(&pdev->dev, "allocate buffer failed\n");
		return -ENOMEM;
	}
	audio->buffer_phys = buf_addr;
	audio->write_buffer1 = audio->buffer_virt;
	audio->write_buffer2 = audio->buffer_virt + WRITE_BUFFER_SIZE;
	audio->read_buffer = audio->buffer_virt + 2 * WRITE_BUFFER_SIZE;

	ret = devm_request_irq(&pdev->dev, audio->irq, goldfish_audio_interrupt,
			       IRQF_SHARED, pdev->name, audio);
	if (ret) {
		dev_err(&pdev->dev, "request_irq failed\n");
		return ret;
	}

	ret = misc_register(&goldfish_audio_device);
	if (ret) {
		dev_err(&pdev->dev,
			"misc_register returned %d in goldfish_audio_init\n",
								ret);
		return ret;
	}

	audio_write64(audio, AUDIO_SET_WRITE_BUFFER_1,
		      AUDIO_SET_WRITE_BUFFER_1_HIGH, buf_addr);
	buf_addr += WRITE_BUFFER_SIZE;

	audio_write64(audio, AUDIO_SET_WRITE_BUFFER_2,
		      AUDIO_SET_WRITE_BUFFER_2_HIGH, buf_addr);

	buf_addr += WRITE_BUFFER_SIZE;

	audio->read_supported = audio_read(audio, AUDIO_READ_SUPPORTED);
	if (audio->read_supported)
		audio_write64(audio, AUDIO_SET_READ_BUFFER,
			      AUDIO_SET_READ_BUFFER_HIGH, buf_addr);

	return 0;
}

static int goldfish_audio_remove(struct platform_device *pdev)
{
	misc_deregister(&goldfish_audio_device);
	return 0;
}

static const struct of_device_id goldfish_audio_of_match[] = {
	{ .compatible = "google,goldfish-audio", },
	{},
};
MODULE_DEVICE_TABLE(of, goldfish_audio_of_match);

#ifdef CONFIG_ACPI
static const struct acpi_device_id goldfish_audio_acpi_match[] = {
	{ "GFSH0005", 0 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, goldfish_audio_acpi_match);
#endif

static struct platform_driver goldfish_audio_driver = {
	.probe		= goldfish_audio_probe,
	.remove		= goldfish_audio_remove,
	.driver = {
		.name = "goldfish_audio",
		.of_match_table = goldfish_audio_of_match,
		.acpi_match_table = ACPI_PTR(goldfish_audio_acpi_match),
	}
};

module_platform_driver(goldfish_audio_driver);
