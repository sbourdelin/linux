/*
 * Copyright (C) 2016 PrasannaKumar Muralidharan <prasannatsmkumar@gmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/printk.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/sched.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/goldfish.h>

#define READ_BUFFER_SIZE	16384
#define WRITE_BUFFER_SIZE	16384
#define COMBINED_BUFFER_SIZE	(2 * ((READ_BUFFER_SIZE) + (WRITE_BUFFER_SIZE)))

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
	AUDIO_INT_READ_BUFFER_FULL      = 1U << 2,

	AUDIO_INT_MASK                  = AUDIO_INT_WRITE_BUFFER_1_EMPTY |
					  AUDIO_INT_WRITE_BUFFER_2_EMPTY |
					  AUDIO_INT_READ_BUFFER_FULL,
};

#define AUDIO_READ(data, addr)		(readl(data->reg_base + addr))
#define AUDIO_WRITE(data, addr, x)	(writel(x, data->reg_base + addr))
#define AUDIO_WRITE64(data, addr, addr2, x)	\
	(gf_write_dma_addr((x), data->reg_base + addr, data->reg_base + addr2))

struct goldfish_audio {
	char __iomem *reg_base;
	int irq;

	/* lock protects access to buffer_status and to device registers */
	spinlock_t lock;
	wait_queue_head_t wait;
	struct task_struct *playback_task;

	int buffer_status;
	int read_supported;		/* true if audio input is supported */

	struct snd_card *card;
	int stream_data;
};

static struct snd_pcm_hardware goldfish_pcm_hw = {
	.info = (SNDRV_PCM_INFO_NONINTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER),
	/* Signed 16bit host endian format. TODO: Change accordingly */
#ifdef __LITTLE_ENDIAN
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
#else
	.formats = SNDRV_PCM_FMTBIT_S16_BE,
#endif
	.rates = SNDRV_PCM_RATE_44100,
	.rate_min = 44100,
	.rate_max = 44100,
	.channels_min = 1,
	.channels_max = 1,
	.buffer_bytes_max = 2 * 16384,
	.period_bytes_min = 1,
	.period_bytes_max = 16384,
	.periods_min = 1,
	.periods_max = 2,
};

static int playback_thread(void *data)
{
	struct goldfish_audio *const audio_data = data;
	unsigned long irq_flags;
	int status;

	while (!kthread_should_stop()) {
		/* bool period_elapsed = false; */
		wait_event_interruptible(audio_data->wait,
					 audio_data->stream_data &&
					 (audio_data->buffer_status &
					 (AUDIO_INT_WRITE_BUFFER_1_EMPTY |
					 AUDIO_INT_WRITE_BUFFER_2_EMPTY)));

		spin_lock_irqsave(&audio_data->lock, irq_flags);
		status = audio_data->buffer_status;
		/*
		 *  clear the buffer empty flag, and signal the emulator
		 *  to start writing the buffer
		 */
		if (status & AUDIO_INT_WRITE_BUFFER_1_EMPTY) {
			audio_data->buffer_status &=
				~AUDIO_INT_WRITE_BUFFER_1_EMPTY;
			AUDIO_WRITE(audio_data, AUDIO_WRITE_BUFFER_1, 16384);
		} else if (status & AUDIO_INT_WRITE_BUFFER_2_EMPTY) {
			audio_data->buffer_status &=
				~AUDIO_INT_WRITE_BUFFER_2_EMPTY;
			AUDIO_WRITE(audio_data, AUDIO_WRITE_BUFFER_2, 16384);
		}
		spin_unlock_irqrestore(&audio_data->lock, irq_flags);
	}

	return 0;
}

static int goldfish_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct goldfish_audio *audio_data = substream->private_data;
	unsigned long irq_flags;

	unsigned long buf_addr = runtime->dma_addr;

	runtime->hw = goldfish_pcm_hw;

	spin_lock_irqsave(&audio_data->lock, irq_flags);
	AUDIO_WRITE64(audio_data, AUDIO_SET_WRITE_BUFFER_1,
		      AUDIO_SET_WRITE_BUFFER_1_HIGH, buf_addr);
	buf_addr += WRITE_BUFFER_SIZE;

	AUDIO_WRITE64(audio_data, AUDIO_SET_WRITE_BUFFER_2,
		      AUDIO_SET_WRITE_BUFFER_2_HIGH, buf_addr);

#if 0
	buf_addr += WRITE_BUFFER_SIZE;

	audio_data->read_supported = AUDIO_READ(audio_data,
			AUDIO_READ_SUPPORTED);
	if (audio_data->read_supported)
		AUDIO_WRITE64(audio_data, AUDIO_SET_READ_BUFFER,
			      AUDIO_SET_READ_BUFFER_HIGH, buf_addr);
#endif
	audio_data->playback_task = kthread_run(playback_thread, audio_data,
						     "goldfish_audio_playback");
	spin_unlock_irqrestore(&audio_data->lock, irq_flags);
	return 0;
}

static int goldfish_pcm_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *hw_params)
{
	return snd_pcm_lib_alloc_vmalloc_buffer(substream,
						params_buffer_bytes(hw_params));
}

static int goldfish_pcm_prepare(struct snd_pcm_substream *substream)
{
	/* Nothing to do here */
	return 0;
}

static int goldfish_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct goldfish_audio *audio_data = substream->private_data;
	unsigned long irq_flags;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		spin_lock_irqsave(&audio_data->lock, irq_flags);
		audio_data->stream_data = true;
		wake_up_interruptible(&audio_data->wait);
		spin_unlock_irqrestore(&audio_data->lock, irq_flags);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		spin_lock_irqsave(&audio_data->lock, irq_flags);
		audio_data->stream_data = false;
		spin_unlock_irqrestore(&audio_data->lock, irq_flags);
		break;
	default:
		pr_info("goldfish_pcm_trigger(), invalid\n");
		return -EINVAL;
	}

	return 0;
}

static snd_pcm_uframes_t goldfish_pcm_pointer(struct snd_pcm_substream *s)
{
	struct goldfish_audio *audio_data = s->private_data;
	unsigned long irq_flags;
	snd_pcm_uframes_t pos = 0;

	spin_lock_irqsave(&audio_data->lock, irq_flags);
	/* TODO: Return correct pointer */
	if (audio_data->buffer_status & AUDIO_INT_WRITE_BUFFER_1_EMPTY)
		pos = 0;
	else if (audio_data->buffer_status & AUDIO_INT_WRITE_BUFFER_2_EMPTY)
		pos = 1;

	spin_unlock_irqrestore(&audio_data->lock, irq_flags);
	return pos;
}

static int goldfish_pcm_close(struct snd_pcm_substream *substream)
{
	struct goldfish_audio *audio_data = substream->private_data;
	unsigned long irq_flags;

	spin_lock_irqsave(&audio_data->lock, irq_flags);
	if (audio_data->playback_task) {
		kthread_stop(audio_data->playback_task);
		audio_data->playback_task = NULL;
	}

	spin_unlock_irqrestore(&audio_data->lock, irq_flags);
	return 0;
}

static struct snd_pcm_ops goldfish_pcm_ops = {
	.open       = goldfish_pcm_open,
	.close      = goldfish_pcm_close,
	.ioctl      = snd_pcm_lib_ioctl,
	.hw_params  = goldfish_pcm_hw_params,
	.hw_free    = snd_pcm_lib_free_vmalloc_buffer,
	.prepare    = goldfish_pcm_prepare,
	.trigger    = goldfish_pcm_trigger,
	.pointer    = goldfish_pcm_pointer,
	.page       = snd_pcm_lib_get_vmalloc_page,
	.mmap       = snd_pcm_lib_mmap_vmalloc,
};

static irqreturn_t goldfish_audio_interrupt(int irq, void *data)
{
	unsigned long irq_flags;
	struct goldfish_audio *audio_data = data;
	u32 status;

	spin_lock_irqsave(&audio_data->lock, irq_flags);

	/* read buffer status flags */
	status = AUDIO_READ(audio_data, AUDIO_INT_STATUS);
	status &= AUDIO_INT_MASK;
	/*
	 *  if buffers are newly empty, wake up blocked
	 *  goldfish_audio_write() call
	 */
	if (status) {
		audio_data->buffer_status = status;
		wake_up(&audio_data->wait);
	}

	spin_unlock_irqrestore(&audio_data->lock, irq_flags);
	return status ? IRQ_HANDLED : IRQ_NONE;
}

static int goldfish_audio_probe(struct platform_device *pdev)
{
	int ret;
	struct resource *r;
	struct goldfish_audio *data;
	struct snd_card *card;
	struct snd_pcm *pcm;

	ret = snd_card_new(NULL, -1, "eac", THIS_MODULE,
			   sizeof(*data), &card);
	if (ret < 0)
		return ret;

	data = card->private_data;
	data->card = card;
	spin_lock_init(&data->lock);
	init_waitqueue_head(&data->wait);
	platform_set_drvdata(pdev, data);

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r) {
		dev_err(&pdev->dev, "platform_get_resource failed\n");
		ret = -ENODEV;
		goto fail;
	}
	data->reg_base = devm_ioremap(&pdev->dev, r->start, PAGE_SIZE);
	if (!data->reg_base) {
		ret = -ENOMEM;
		goto fail;
	}

	data->irq = platform_get_irq(pdev, 0);
	if (data->irq < 0) {
		dev_err(&pdev->dev, "platform_get_irq failed\n");
		ret = -ENODEV;
		goto fail;
	}

	ret = devm_request_irq(&pdev->dev, data->irq, goldfish_audio_interrupt,
			       IRQF_SHARED, pdev->name, data);
	if (ret) {
		dev_err(&pdev->dev, "request_irq failed\n");
		goto fail;
	}

	snprintf(card->driver, sizeof(card->driver), "%s", "goldfish_audio");
	snprintf(card->shortname, sizeof(card->shortname), "EAC Goldfish:%d",
		 card->number);
	snprintf(card->longname, sizeof(card->longname), "%s", card->shortname);

	ret = snd_pcm_new(card, "eac", 0, 1, 0, &pcm);
	if (ret < 0)
		goto fail;

	pcm->private_data = data;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &goldfish_pcm_ops);
	ret = snd_card_register(card);
	if (ret < 0)
		goto fail;

	return 0;

fail:
	snd_card_free(card);
	return ret;
}

static int goldfish_audio_remove(struct platform_device *pdev)
{
	struct goldfish_audio *audio_data = platform_get_drvdata(pdev);
	unsigned long irq_flags;

	spin_lock_irqsave(&audio_data->lock, irq_flags);
	if (audio_data->playback_task) {
		kthread_stop(audio_data->playback_task);
		audio_data->playback_task = NULL;
	}
	snd_card_free(audio_data->card);

	spin_unlock_irqrestore(&audio_data->lock, irq_flags);
	return 0;
}

static struct platform_driver goldfish_audio_driver = {
	.probe		= goldfish_audio_probe,
	.remove		= goldfish_audio_remove,
	.driver = {
		.name = "goldfish_audio"
	}
};

module_platform_driver(goldfish_audio_driver);

MODULE_AUTHOR("PrasannaKumar Muralidharan <prasannatsmkumar@gmail.com>");
MODULE_DESCRIPTION("Android QEMU Audio Driver");
MODULE_LICENSE("GPL");
