/*
 * Michael Zoran <mzoran@crowfest.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2.
 *
 * bcm2835-analog-audio provides support for very simple analog
 * audio using the PWM hardware of the bcm2835.  It is assumed
 * that additional analog hardware is connected to the GPIO pins
 * to amplify the audio and provide basic analog filtering.
 *
 */

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/pcm_params.h>
#include <sound/dmaengine_pcm.h>
#include <linux/of_address.h>
#include <linux/dma-mapping.h>

/*
 *  PWM Register Offsets
 */
#define PWM_REG_CTR		0x00
#define PWM_REG_STA		0x04
#define PWM_REG_DMAC		0x08
#define PWM_REG_RNG1		0x10
#define PWM_REG_DAT1		0x14
#define PWM_REG_FIFO		0x18
#define PWM_REG_RNG2		0x20
#define PWM_REG_DAT2		0x24

#define PWM_CLOCK_FREQUENCY	100000000
#define PWM_SAMPLE_RATE		48000
#define PWM_SYMBOLS		(PWM_CLOCK_FREQUENCY / PWM_SAMPLE_RATE)
#define PWM_DC_OFFSET		(PWM_SYMBOLS / 2)

/*
 * The channel order that needs to be passed to the PWM FIFO is opposite the
 * order that is passed by the application.  So the order needs to be flipped
 * in software.
 */


struct bcm2835_hardware_frame {
	u32 right;
	u32 left;
};

struct bcm2835_software_frame {
	s16 left;
	s16 right;
};

#define HARDWARE_BUFFER_FRAMES_PER_PERIOD 720
#define HARDWARE_BUFFER_PERIODS_PER_BUFFER 2
#define HARDWARE_BUFFER_FRAMES_PER_BUFFER (HARDWARE_BUFFER_FRAMES_PER_PERIOD * \
					   HARDWARE_BUFFER_PERIODS_PER_BUFFER)
#define HARDWARE_BUFFER_PERIOD_BYTES (sizeof(struct bcm2835_hardware_frame) * \
				      HARDWARE_BUFFER_FRAMES_PER_PERIOD)
#define HARDWARE_BUFFER_BYTES (HARDWARE_BUFFER_PERIOD_BYTES * \
			       HARDWARE_BUFFER_PERIODS_PER_BUFFER)

struct bcm2835_chip;

struct bcm2835_chip_runtime {
	struct bcm2835_chip *chip;
	struct snd_pcm_substream *substream;
	spinlock_t spinlock;
	struct dma_slave_config dma_slave_config;
	struct dma_async_tx_descriptor *dma_desc;
	dma_cookie_t dma_cookie;
	struct bcm2835_hardware_frame *hardware_buffer;
	dma_addr_t hardware_buffer_dma;
	int hardware_period_number;
	bool is_playing;
	struct bcm2835_software_frame *playback_src_buffer;
	snd_pcm_uframes_t playback_src_pos;
	snd_pcm_uframes_t playback_src_frames_this_period;
};

struct bcm2835_chip {
	struct platform_device *pdev;
	struct device *dev;
	struct mutex lock;
	u32 dma_addr;
	void __iomem *base;
	struct clk *clk;
	int opencount;
	struct dma_chan *dma_channel;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct bcm2835_chip_runtime *runtime;
};

static u32 convert_audio_data(s16 input)
{
	s32 output;

	output = ((s32) input * (s32)(PWM_SYMBOLS / 2)) / (s32)32762;
	return (u32)(output + PWM_DC_OFFSET);
}

static void convert_dma_buffer(struct bcm2835_chip_runtime *chip_runtime)
{
	snd_pcm_uframes_t i;
	snd_pcm_uframes_t hardware_start_pos =
		chip_runtime->hardware_period_number *
		HARDWARE_BUFFER_FRAMES_PER_PERIOD;
	snd_pcm_uframes_t buffer_size =
		chip_runtime->substream->runtime->buffer_size;
	struct bcm2835_hardware_frame *hard_frame =
		chip_runtime->hardware_buffer + hardware_start_pos;

	for (i = 0; i < HARDWARE_BUFFER_FRAMES_PER_PERIOD; i++) {
		struct bcm2835_software_frame *soft_frame =
			chip_runtime->playback_src_buffer +
			chip_runtime->playback_src_pos;

		hard_frame->left = convert_audio_data(soft_frame->left);
		hard_frame->right = convert_audio_data(soft_frame->right);
		hard_frame++;

		if (chip_runtime->playback_src_pos >= buffer_size - 1)
			chip_runtime->playback_src_pos = 0;
		else
			chip_runtime->playback_src_pos++;
	}
}

static void fill_silence(struct bcm2835_chip_runtime *chip_runtime)
{
	snd_pcm_uframes_t i;
	snd_pcm_uframes_t hardware_start_pos =
		chip_runtime->hardware_period_number *
		HARDWARE_BUFFER_FRAMES_PER_PERIOD;
	struct bcm2835_hardware_frame *hard_frame =
		chip_runtime->hardware_buffer + hardware_start_pos;

	for (i = 0; i < HARDWARE_BUFFER_FRAMES_PER_PERIOD; i++) {
		hard_frame->left = PWM_DC_OFFSET;
		hard_frame->right = PWM_DC_OFFSET;
		hard_frame++;
	}
}

static void dma_complete(void *arg)
{
	struct bcm2835_chip_runtime *chip_runtime = arg;
	unsigned long flags;
	bool period_elapsed = false;

	spin_lock_irqsave(&chip_runtime->spinlock, flags);

	chip_runtime->hardware_period_number++;
	if (chip_runtime->hardware_period_number >=
		HARDWARE_BUFFER_PERIODS_PER_BUFFER)
		chip_runtime->hardware_period_number = 0;

	if (!chip_runtime->is_playing)
		fill_silence(chip_runtime);
	else {
		chip_runtime->playback_src_frames_this_period +=
			HARDWARE_BUFFER_FRAMES_PER_PERIOD;

		if (chip_runtime->playback_src_frames_this_period >=
			chip_runtime->substream->runtime->period_size) {
			chip_runtime->playback_src_frames_this_period = 0;
			period_elapsed = true;
		}

		convert_dma_buffer(chip_runtime);
	}

	spin_unlock_irqrestore(&chip_runtime->spinlock, flags);

	if (period_elapsed)
		snd_pcm_period_elapsed(chip_runtime->substream);
}

static void snd_bcm2835_cleanup_runtime(struct snd_pcm_substream *substream)
{
	struct bcm2835_chip *chip = snd_pcm_substream_chip(substream);
	struct bcm2835_chip_runtime *chip_runtime = chip->runtime;

	if (!chip_runtime)
		return;

	if (chip_runtime->dma_cookie)
		dmaengine_terminate_sync(chip->dma_channel);

	writel(0x00, chip->base + PWM_REG_CTR);
	writel(0x00, chip->base + PWM_REG_DMAC);

	if (chip_runtime->dma_desc)
		dmaengine_desc_free(chip_runtime->dma_desc);

	if (chip_runtime->hardware_buffer)
		dma_free_coherent(chip->dma_channel->device->dev,
			HARDWARE_BUFFER_BYTES,
			chip_runtime->hardware_buffer,
			chip_runtime->hardware_buffer_dma);

	chip->runtime = NULL;
	kfree(chip_runtime);
}

static int snd_bcm2835_init_runtime(struct snd_pcm_substream *substream)
{
	struct bcm2835_chip *chip = snd_pcm_substream_chip(substream);
	struct bcm2835_chip_runtime *chip_runtime;
	int err;
	int i;

	if (chip->runtime)
		return 0;

	chip_runtime = kzalloc(sizeof(*chip_runtime), GFP_KERNEL);

	if (!chip_runtime)
		return -ENOMEM;

	chip_runtime->chip = chip;
	chip_runtime->substream = substream;
	spin_lock_init(&chip_runtime->spinlock);
	chip->runtime = chip_runtime;

	chip_runtime->hardware_buffer =
		dma_alloc_coherent(chip->dma_channel->device->dev,
				   HARDWARE_BUFFER_BYTES,
				   &chip_runtime->hardware_buffer_dma,
				   GFP_KERNEL);

	if (!chip_runtime->hardware_buffer) {
		snd_bcm2835_cleanup_runtime(substream);
		return -ENOMEM;
	}

	for (i = 0; i < HARDWARE_BUFFER_FRAMES_PER_BUFFER; i++) {
		chip_runtime->hardware_buffer[i].left = PWM_DC_OFFSET;
		chip_runtime->hardware_buffer[i].right = PWM_DC_OFFSET;
	}

	chip_runtime->hardware_period_number =
		(HARDWARE_BUFFER_PERIODS_PER_BUFFER - 1);

	chip_runtime->dma_slave_config.direction	= DMA_MEM_TO_DEV;
	chip_runtime->dma_slave_config.dst_addr		= chip->dma_addr;
	chip_runtime->dma_slave_config.dst_maxburst	= 2;
	chip_runtime->dma_slave_config.dst_addr_width	= 4;
	chip_runtime->dma_slave_config.src_addr		=
		chip_runtime->hardware_buffer_dma;
	chip_runtime->dma_slave_config.src_maxburst	= 2;
	chip_runtime->dma_slave_config.src_addr_width	= 4;

	err = dmaengine_slave_config(chip->dma_channel,
				     &chip_runtime->dma_slave_config);

	if (err < 0) {
		snd_bcm2835_cleanup_runtime(substream);
		return err;
	}

	chip_runtime->dma_desc =
		dmaengine_prep_dma_cyclic(chip->dma_channel,
					  chip_runtime->hardware_buffer_dma,
					  HARDWARE_BUFFER_BYTES,
					  HARDWARE_BUFFER_PERIOD_BYTES,
					  DMA_MEM_TO_DEV,
					  DMA_CTRL_ACK | DMA_PREP_INTERRUPT);

	if (!chip_runtime->dma_desc) {
		snd_bcm2835_cleanup_runtime(substream);
		return -ENOMEM;
	}

	chip_runtime->dma_desc->callback = dma_complete;
	chip_runtime->dma_desc->callback_param = chip_runtime;

	writel(PWM_SYMBOLS,	chip->base + PWM_REG_RNG1);
	writel(PWM_SYMBOLS,	chip->base + PWM_REG_RNG2);
	writel(0xa1e1,		chip->base + PWM_REG_CTR);
	writel(0x80000E0E,	chip->base + PWM_REG_DMAC);

	chip_runtime->dma_cookie = dmaengine_submit(chip_runtime->dma_desc);
	dma_async_issue_pending(chip->dma_channel);

	return 0;

}
static struct snd_pcm_hardware snd_bcm2835_playback_hw = {
	.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_BLOCK_TRANSFER |
		 SNDRV_PCM_INFO_MMAP_VALID),
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 128 * 1024,
	.period_bytes_min = 4 * 1024,
	.period_bytes_max = 128 * 1024,
	.periods_min = 1,
	.periods_max = 128 / 4,
	.fifo_size = 0,
};

static void snd_bcm2835_playback_free(struct snd_pcm_runtime *runtime)
{
	runtime->private_data = NULL;
}

static int snd_bcm2835_playback_open(struct snd_pcm_substream *substream)
{
	struct bcm2835_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err;

	if (mutex_lock_interruptible(&chip->lock))
		return -EINTR;

	if (chip->opencount) {
		chip->opencount++;
		mutex_unlock(&chip->lock);
		return 0;
	}

	clk_set_rate(chip->clk, PWM_CLOCK_FREQUENCY);
	err = clk_prepare_enable(chip->clk);
	if (err)
		return err;

	err = snd_bcm2835_init_runtime(substream);
	if (err) {
		clk_disable_unprepare(chip->clk);
		mutex_unlock(&chip->lock);
		return err;
	}

	chip->opencount++;

	runtime->hw = snd_bcm2835_playback_hw;
	runtime->private_data = chip->runtime;
	runtime->private_free = snd_bcm2835_playback_free;

	mutex_unlock(&chip->lock);

	return 0;
}

static int snd_bcm2835_playback_close(struct snd_pcm_substream *substream)
{
	struct bcm2835_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	if (mutex_lock_interruptible(&chip->lock))
		return -EINTR;

	if (!chip->opencount) {
		mutex_unlock(&chip->lock);
		return 0;
	}

	chip->opencount--;
	if (chip->opencount) {
		mutex_unlock(&chip->lock);
		return 0;
	}

	snd_bcm2835_cleanup_runtime(substream);
	clk_disable_unprepare(chip->clk);

	runtime->private_data = NULL;
	runtime->private_free = NULL;

	mutex_unlock(&chip->lock);
	return 0;
}

static int snd_bcm2835_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return snd_pcm_lib_free_pages(substream);
}

static int snd_bcm2835_pcm_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params)
{
	struct bcm2835_chip *chip = snd_pcm_substream_chip(substream);
	struct bcm2835_chip_runtime *chip_runtime = chip->runtime;
	snd_pcm_uframes_t playback_src_buffer_frames;
	snd_pcm_uframes_t playback_src_period_frames;
	int err = 0;

	if (mutex_lock_interruptible(&chip->lock))
		return -EINTR;

	snd_bcm2835_pcm_hw_free(substream);

	playback_src_buffer_frames = params_buffer_bytes(params) / 4;
	playback_src_period_frames = params_period_bytes(params) / 4;

	err = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(params));
	if (err < 0) {
		snd_bcm2835_pcm_hw_free(substream);
		mutex_unlock(&chip->lock);
		return err;
	}

	chip_runtime->playback_src_buffer =
		(struct bcm2835_software_frame *)(substream->runtime->dma_area);
	chip_runtime->playback_src_pos = 0;
	chip_runtime->playback_src_frames_this_period = 0;

	mutex_unlock(&chip->lock);
	return 0;
}

static int snd_bcm2835_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct bcm2835_chip *chip = snd_pcm_substream_chip(substream);
	struct bcm2835_chip_runtime *chip_runtime = chip->runtime;

	if (mutex_lock_interruptible(&chip->lock))
		return -EINTR;

	chip_runtime->playback_src_buffer =
		(struct bcm2835_software_frame *)(substream->runtime->dma_area);
	chip_runtime->playback_src_pos = 0;
	chip_runtime->playback_src_frames_this_period = 0;

	memset(chip_runtime->playback_src_buffer, 0,
		substream->runtime->buffer_size *
		sizeof(*(chip_runtime->playback_src_buffer)));

	mutex_unlock(&chip->lock);
	return 0;
}

static int snd_bcm2835_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct bcm2835_chip *chip = snd_pcm_substream_chip(substream);
	struct bcm2835_chip_runtime *chip_runtime = chip->runtime;
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&chip_runtime->spinlock, flags);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		chip_runtime->is_playing = true;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		chip_runtime->is_playing = false;
		break;
	default:
		ret = -EINVAL;
	}

	spin_unlock_irqrestore(&chip_runtime->spinlock, flags);

	return ret;
}

static snd_pcm_uframes_t
snd_bcm2835_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct bcm2835_chip *chip = snd_pcm_substream_chip(substream);
	struct bcm2835_chip_runtime *chip_runtime = chip->runtime;
	snd_pcm_uframes_t ret;
	unsigned long flags;

	spin_lock_irqsave(&chip_runtime->spinlock, flags);
	ret = chip_runtime->playback_src_pos;
	spin_unlock_irqrestore(&chip_runtime->spinlock, flags);

	return ret;
}

static struct snd_pcm_ops snd_bcm2835_playback_ops = {
	.open = snd_bcm2835_playback_open,
	.close = snd_bcm2835_playback_close,
	.ioctl =  snd_pcm_lib_ioctl,
	.hw_params = snd_bcm2835_pcm_hw_params,
	.hw_free = snd_bcm2835_pcm_hw_free,
	.prepare = snd_bcm2835_pcm_prepare,
	.trigger = snd_bcm2835_pcm_trigger,
	.pointer = snd_bcm2835_pcm_pointer,
};

static int snd_bcm2835_new_pcm(struct bcm2835_chip *chip)
{
	struct snd_pcm *pcm;
	int err;

	err = snd_pcm_new(chip->card, "BCM2835 Analog", 0, 1, 0, &pcm);

	if (err < 0)
		return err;

	pcm->private_data = chip;
	strcpy(pcm->name, "BCM2835 Analog");
	chip->pcm = pcm;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
		&snd_bcm2835_playback_ops);

	/* pre-allocation of buffers */
	/* NOTE: this may fail */
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
		snd_dma_continuous_data(GFP_KERNEL),
		snd_bcm2835_playback_hw.buffer_bytes_max,
		snd_bcm2835_playback_hw.buffer_bytes_max);

	return 0;
}

static int snd_bcm2835_free(struct bcm2835_chip *chip)
{
	if (chip->dma_channel)
		dma_release_channel(chip->dma_channel);

	return 0;
}

static int snd_bcm2835_dev_free(struct snd_device *device)
{
	return snd_bcm2835_free(device->device_data);
}

static struct snd_device_ops snd_bcm2835_dev_ops = {
	.dev_free = snd_bcm2835_dev_free,
};

static int snd_bcm2835_create(struct snd_card *card,
	struct platform_device *pdev,
	struct bcm2835_chip **rchip)
{
	struct bcm2835_chip *chip;
	struct resource *res;
	int err;
	const __be32 *addr;

	*rchip = NULL;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->pdev = pdev;
	chip->dev = &pdev->dev;
	chip->card = card;

	mutex_init(&chip->lock);

	/*
	 * Get the physical address of the PWM FIFO. We need to retrieve
	 * the bus address specified in the DT, because the physical address
	 * (the one returned by platform_get_resource()) is not appropriate
	 * for DMA transfers.
	 */
	addr = of_get_address(chip->dev->of_node, 0, NULL, NULL);
	chip->dma_addr = be32_to_cpup(addr) + PWM_REG_FIFO;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	chip->base = devm_ioremap_resource(chip->dev, res);
	if (IS_ERR(chip->base))
		return PTR_ERR(chip->base);

	chip->clk = devm_clk_get(chip->dev, NULL);
	if (IS_ERR(chip->clk)) {
		dev_err(&pdev->dev, "clock not found: %ld\n",
			PTR_ERR(chip->clk));
		return PTR_ERR(chip->clk);
	}

	chip->dma_channel = dma_request_slave_channel(chip->dev, "tx");

	if (!chip->dma_channel)
		return -ENOMEM;

	err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, chip,
		&snd_bcm2835_dev_ops);

	if (err < 0) {
		snd_bcm2835_free(chip);
		return err;
	}

	*rchip = chip;
	return 0;
}

static int bcm2835_analog_audio_probe(struct platform_device *pdev)
{
	struct snd_card *card;
	int ret;
	struct device *dev = &pdev->dev;
	struct bcm2835_chip *chip;

	ret = snd_card_new(&pdev->dev, -1, NULL, THIS_MODULE, 0, &card);
	if (ret) {
		dev_err(dev, "Failed to create sound card structure\n");
		return ret;
	}

	ret = snd_bcm2835_create(card, pdev, &chip);
	if (ret < 0) {
		dev_err(dev, "Failed to create bcm2835 chip\n");
		return ret;
	}

	snd_card_set_dev(card, dev);
	strcpy(card->driver, "BCM2835 Analog");
	strcpy(card->shortname, "BCM2835 Analog");
	sprintf(card->longname, "%s", card->shortname);

	ret = snd_bcm2835_new_pcm(chip);
	if (ret < 0) {
		snd_card_free(card);
		return ret;
	}

	ret = snd_card_register(card);
	if (ret < 0) {
		snd_card_free(card);
		return ret;
	}

	platform_set_drvdata(pdev, card);

	dev_notice(dev, "BCM2835 Analog Audio Initialized\n");

	return 0;
}

static int bcm2835_analog_audio_remove(struct platform_device *pdev)
{
	struct snd_card *card;

	card = platform_get_drvdata(pdev);

	if (card)
		snd_card_free(card);

	return 0;
}

static const struct of_device_id bcm2835_analog_audio_of_match[] = {
	{ .compatible = "brcm,bcm2835-analog-audio",},
	{ /* sentinel */}
};
MODULE_DEVICE_TABLE(of, bcm2835_analog_audio_of_match);

static struct platform_driver bcm2835_analog_audio_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "bcm2835-analog-audio",
		.of_match_table = bcm2835_analog_audio_of_match,
	},
	.probe = bcm2835_analog_audio_probe,
	.remove = bcm2835_analog_audio_remove,
};
module_platform_driver(bcm2835_analog_audio_driver);

MODULE_AUTHOR("Michael Zoran");
MODULE_DESCRIPTION("Audio driver for analog output on the BCM2835 chip");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:brcm,bcm2835-analog-audio");
