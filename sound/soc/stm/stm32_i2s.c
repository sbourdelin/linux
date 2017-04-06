/*
 *  STM32 ALSA SoC Digital Audio Interface (I2S) driver.
 *
 * Copyright (C) 2017, STMicroelectronics - All Rights Reserved
 * Author(s): Olivier Moysan <olivier.moysan@st.com> for STMicroelectronics.
 *
 * License terms: GPL V2.0.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

#include <sound/dmaengine_pcm.h>
#include <sound/pcm_params.h>

#define STM32_I2S_CR1_REG	0x0
#define STM32_I2S_CFG1_REG	0x08
#define STM32_I2S_CFG2_REG	0x0C
#define STM32_I2S_IER_REG	0x10
#define STM32_I2S_SR_REG	0x14
#define STM32_I2S_IFCR_REG	0x18
#define STM32_I2S_TXDR_REG	0X20
#define STM32_I2S_RXDR_REG	0x30
#define STM32_I2S_CGFR_REG	0X50

/* Bit definition for SPI2S_CR1 register */
#define I2S_CR1_SPE		BIT(0)
#define I2S_CR1_CSTART		BIT(9)
#define I2S_CR1_CSUSP		BIT(10)
#define I2S_CR1_HDDIR		BIT(11)
#define I2S_CR1_SSI		BIT(12)
#define I2S_CR1_CRC33_17	BIT(13)
#define I2S_CR1_RCRCI		BIT(14)
#define I2S_CR1_TCRCI		BIT(15)

/* Bit definition for SPI_CFG2 register */
#define I2S_CFG2_IOSWP_SHIFT	15
#define I2S_CFG2_IOSWP		BIT(I2S_CFG2_IOSWP_SHIFT)
#define I2S_CFG2_LSBFRST	BIT(23)
#define I2S_CFG2_AFCNTR		BIT(31)

/* Bit definition for SPI_CFG1 register */
#define I2S_CFG1_FTHVL_SHIFT	5
#define I2S_CFG1_FTHVL_MASK	GENMASK(8, I2S_CFG1_FTHVL_SHIFT)
#define I2S_CFG1_FTHVL_SET(x)	((x) << I2S_CFG1_FTHVL_SHIFT)

#define I2S_CFG1_TXDMAEN	BIT(15)
#define I2S_CFG1_RXDMAEN	BIT(14)

/* Bit definition for SPI2S_IER register */
#define I2S_IER_RXPIE		BIT(0)
#define I2S_IER_TXPIE		BIT(1)
#define I2S_IER_DPXPIE		BIT(2)
#define I2S_IER_EOTIE		BIT(3)
#define I2S_IER_TXTFIE		BIT(4)
#define I2S_IER_UDRIE		BIT(5)
#define I2S_IER_OVRIE		BIT(6)
#define I2S_IER_CRCEIE		BIT(7)
#define I2S_IER_TIFREIE		BIT(8)
#define I2S_IER_MODFIE		BIT(9)
#define I2S_IER_TSERFIE		BIT(10)

/* Bit definition for SPI2S_SR register */
#define I2S_SR_RXP		BIT(0)
#define I2S_SR_TXP		BIT(1)
#define I2S_SR_DPXP		BIT(2)
#define I2S_SR_EOT		BIT(3)
#define I2S_SR_TXTF		BIT(4)
#define I2S_SR_UDR		BIT(5)
#define I2S_SR_OVR		BIT(6)
#define I2S_SR_CRCERR		BIT(7)
#define I2S_SR_TIFRE		BIT(8)
#define I2S_SR_MODF		BIT(9)
#define I2S_SR_TSERF		BIT(10)
#define I2S_SR_SUSP		BIT(11)
#define I2S_SR_TXC		BIT(12)
#define I2S_SR_RXPLVL		GENMASK(14, 13)
#define I2S_SR_RXWNE		BIT(15)

#define I2S_SR_MASK		GENMASK(15, 0)

/* Bit definition for SPI_IFCR register */
#define I2S_IFCR_EOTC		BIT(3)
#define I2S_IFCR_TXTFC		BIT(4)
#define I2S_IFCR_UDRC		BIT(5)
#define I2S_IFCR_OVRC		BIT(6)
#define I2S_IFCR_CRCEC		BIT(7)
#define I2S_IFCR_TIFREC		BIT(8)
#define I2S_IFCR_MODFC		BIT(9)
#define I2S_IFCR_TSERFC		BIT(10)
#define I2S_IFCR_SUSPC		BIT(11)

#define I2S_IFCR_MASK		GENMASK(11, 3)

/* Bit definition for SPI_I2SCGFR register */
#define I2S_CGFR_I2SMOD		BIT(0)

#define I2S_CGFR_I2SCFG_SHIFT	1
#define I2S_CGFR_I2SCFG_MASK	GENMASK(3, I2S_CGFR_I2SCFG_SHIFT)
#define I2S_CGFR_I2SCFG_SET(x)	((x) << I2S_CGFR_I2SCFG_SHIFT)

#define I2S_CGFR_I2SSTD_SHIFT	4
#define I2S_CGFR_I2SSTD_MASK	GENMASK(5, I2S_CGFR_I2SSTD_SHIFT)
#define I2S_CGFR_I2SSTD_SET(x)	((x) << I2S_CGFR_I2SSTD_SHIFT)

#define I2S_CGFR_PCMSYNC	BIT(7)

#define I2S_CGFR_DATLEN_SHIFT	8
#define I2S_CGFR_DATLEN_MASK	GENMASK(9, I2S_CGFR_DATLEN_SHIFT)
#define I2S_CGFR_DATLEN_SET(x)	((x) << I2S_CGFR_DATLEN_SHIFT)

#define I2S_CGFR_CHLEN_SHIFT	10
#define I2S_CGFR_CHLEN		BIT(I2S_CGFR_CHLEN_SHIFT)
#define I2S_CGFR_CKPOL		BIT(11)
#define I2S_CGFR_FIXCH		BIT(12)
#define I2S_CGFR_WSINV		BIT(13)
#define I2S_CGFR_DATFMT		BIT(14)

#define I2S_CGFR_I2SDIV_SHIFT	16
#define I2S_CGFR_I2SDIV_BIT_H	23
#define I2S_CGFR_I2SDIV_MASK	GENMASK(I2S_CGFR_I2SDIV_BIT_H,\
				I2S_CGFR_I2SDIV_SHIFT)
#define I2S_CGFR_I2SDIV_SET(x)	((x) << I2S_CGFR_I2SDIV_SHIFT)
#define	I2S_CGFR_I2SDIV_MAX	((1 << (I2S_CGFR_I2SDIV_BIT_H -\
				I2S_CGFR_I2SDIV_SHIFT)) - 1)

#define I2S_CGFR_ODD_SHIFT	24
#define I2S_CGFR_ODD		BIT(I2S_CGFR_ODD_SHIFT)
#define I2S_CGFR_MCKOE		BIT(25)

enum i2s_master_mode {
	I2S_MS_NOT_SET,
	I2S_MS_MASTER,
	I2S_MS_SLAVE,
};

enum i2s_mode {
	I2S_I2SMOD_TX_SLAVE,
	I2S_I2SMOD_RX_SLAVE,
	I2S_I2SMOD_TX_MASTER,
	I2S_I2SMOD_RX_MASTER,
	I2S_I2SMOD_FD_SLAVE,
	I2S_I2SMOD_FD_MASTER,
};

enum i2s_fifo_th {
	I2S_FIFO_TH_NONE,
	I2S_FIFO_TH_ONE_QUARTER,
	I2S_FIFO_TH_HALF,
	I2S_FIFO_TH_THREE_QUARTER,
	I2S_FIFO_TH_FULL,
};

enum i2s_std {
	I2S_STD_I2S,
	I2S_STD_LEFT_J,
	I2S_STD_RIGHT_J,
	I2S_STD_DSP,
};

enum i2s_dir {
	I2S_DIR_TX,
	I2S_DIR_RX,
	I2S_DIR_FD,
};

enum i2s_datlen {
	I2S_I2SMOD_DATLEN_16,
	I2S_I2SMOD_DATLEN_24,
	I2S_I2SMOD_DATLEN_32,
};

#define STM32_I2S_DAI_NAME_SIZE		20
#define STM32_I2S_DAIS_NB		3
#define STM32_I2S_IP_NAME_LENGTH	5
#define STM32_I2S_FIFO_SIZE		16

#define STM32_I2S_IS_MASTER(x)		((x)->ms_flg == I2S_MS_MASTER)
#define STM32_I2S_IS_SLAVE(x)		((x)->ms_flg == I2S_MS_SLAVE)

#define STM32_I2S_IS_PLAYBACK(x)	((x)->dir == I2S_DIR_TX)
#define STM32_I2S_IS_CAPTURE(x)		((x)->dir == I2S_DIR_RX)
#define STM32_I2S_IS_FULL_DUPLEX(x)	((x)->dir == I2S_DIR_FD)

/**
 * @regmap_conf: I2S register map configuration pointer
 * @egmap: I2S register map pointer
 * @pdev: device data pointer
 * @dai_drv: DAI driver pointer
 * @dma_data_tx: dma configuration data for tx channel
 * @dma_data_rx: dma configuration data for tx channel
 * @substream: PCM substream data pointer
 * @i2sclk: kernel clock feeding the I2S clock generator
 * @pclk: peripheral clock driving bus interface
 * @x8kclk: I2S parent clock for sampling frequencies multiple of 8kHz
 * @x11kclk: I2S parent clock for sampling frequencies multiple of 11kHz
 * @base:  mmio register base virtual address
 * @phys_addr: I2S registers physical base address
 * @lock_fd: lock to manage race conditions in full duplex mode
 * @ip_name: I2S name
 * @dais_name: playback, capture and fd DAI names
 * @fifo_th: fifo threshold setting
 * @mclk_rate: master clock frequency (Hz)
 * @fmt: DAI protocol
 * @refcount: keep count of opened streams on I2S
 * @startcount: keep count of started streams on I2S
 * @dir: I2S direction. tx, rx or full duplex.
 * @ms_flg: master mode flag.
 * @format: pcm stream width (16/32 bits), used for consistency check in fd mode
 * @rate: pcm stream rate, used for consistency check in fd mode
 */
struct stm32_i2s_data {
	const struct regmap_config *regmap_conf;
	struct regmap *regmap;
	struct platform_device *pdev;
	struct snd_soc_dai_driver *dai_drv;
	struct snd_dmaengine_dai_dma_data dma_data_tx;
	struct snd_dmaengine_dai_dma_data dma_data_rx;
	struct snd_pcm_substream *substream;
	struct clk *i2sclk;
	struct clk *pclk;
	struct clk *x8kclk;
	struct clk *x11kclk;
	void __iomem *base;
	dma_addr_t phys_addr;
	spinlock_t lock_fd; /* Manage race conditions for full duplex */
	const char *ip_name;
	char dais_name[STM32_I2S_DAIS_NB][STM32_I2S_DAI_NAME_SIZE];
	unsigned int fifo_th;
	unsigned int mclk_rate;
	unsigned int fmt;
	int refcount;
	int startcount;
	int dir;
	int ms_flg;
	int format;
	unsigned int rate;
};

static irqreturn_t stm32_i2s_isr(int irq, void *devid)
{
	struct stm32_i2s_data *i2s = (struct stm32_i2s_data *)devid;
	struct platform_device *pdev = i2s->pdev;
	u32 sr, ier;
	unsigned long flags;
	int err = 0;

	regmap_read(i2s->regmap, STM32_I2S_SR_REG, &sr);
	regmap_read(i2s->regmap, STM32_I2S_IER_REG, &ier);

	flags = sr & ier;
	if (!flags) {
		dev_dbg(&pdev->dev, "Spurious IT sr=0x%08x, ier=0x%08x\n",
			sr, ier);
		return IRQ_NONE;
	}

	/* Clear ITs */
	regmap_update_bits(i2s->regmap, STM32_I2S_IFCR_REG,
			   I2S_IFCR_MASK, flags);

	if (flags & I2S_SR_OVR) {
		dev_dbg(&pdev->dev, "Overrun: received value discarded\n");
		err = 1;
	}

	if (flags & I2S_SR_UDR) {
		dev_dbg(&pdev->dev, "Underrun\n");
		err = 1;
	}

	if (flags & I2S_SR_TIFRE)
		dev_dbg(&pdev->dev, "Frame error\n");

	if (err)
		snd_pcm_stop_xrun(i2s->substream);

	return IRQ_HANDLED;
}

static bool stm32_i2s_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case STM32_I2S_CR1_REG:
	case STM32_I2S_CFG1_REG:
	case STM32_I2S_CFG2_REG:
	case STM32_I2S_IER_REG:
	case STM32_I2S_SR_REG:
	case STM32_I2S_IFCR_REG:
	case STM32_I2S_TXDR_REG:
	case STM32_I2S_RXDR_REG:
	case STM32_I2S_CGFR_REG:
		return true;
	default:
		return false;
	}
}

static bool stm32_i2s_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case STM32_I2S_TXDR_REG:
	case STM32_I2S_RXDR_REG:
		return true;
	default:
		return false;
	}
}

static bool stm32_i2s_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case STM32_I2S_CR1_REG:
	case STM32_I2S_CFG1_REG:
	case STM32_I2S_CFG2_REG:
	case STM32_I2S_IER_REG:
	case STM32_I2S_IFCR_REG:
	case STM32_I2S_TXDR_REG:
	case STM32_I2S_CGFR_REG:
		return true;
	default:
		return false;
	}
}

static int stm32_i2s_set_dai_fmt(struct snd_soc_dai *cpu_dai, unsigned int fmt)
{
	struct stm32_i2s_data *i2s = snd_soc_dai_get_drvdata(cpu_dai);
	u32 cgfr;
	u32 cgfr_mask =  I2S_CGFR_I2SSTD_MASK | I2S_CGFR_CKPOL |
			 I2S_CGFR_WSINV | I2S_CGFR_I2SCFG_MASK;

	dev_dbg(cpu_dai->dev, "fmt %x\n", fmt);

	/*
	 * winv = 0 : default behavior (high/low) for all standards
	 * ckpol 0 for all standards.
	 */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		cgfr = I2S_CGFR_I2SSTD_SET(I2S_STD_I2S);
		break;
	case SND_SOC_DAIFMT_MSB:
		cgfr = I2S_CGFR_I2SSTD_SET(I2S_STD_LEFT_J);
		break;
	case SND_SOC_DAIFMT_LSB:
		cgfr = I2S_CGFR_I2SSTD_SET(I2S_STD_RIGHT_J);
		break;
	case SND_SOC_DAIFMT_DSP_A:
		cgfr = I2S_CGFR_I2SSTD_SET(I2S_STD_DSP);
		break;
	/* DSP_B not mapped on I2S PCM long format. 1 bit offset does not fit */
	default:
		dev_err(cpu_dai->dev, "Unsupported protocol %#x\n",
			fmt & SND_SOC_DAIFMT_FORMAT_MASK);
		return -EINVAL;
	}

	/* DAI clock strobing */
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_IB_NF:
		cgfr |= I2S_CGFR_CKPOL;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		cgfr |= I2S_CGFR_WSINV;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		cgfr |= I2S_CGFR_CKPOL;
		cgfr |= I2S_CGFR_WSINV;
		break;
	default:
		dev_err(cpu_dai->dev, "Unsupported strobing %#x\n",
			fmt & SND_SOC_DAIFMT_INV_MASK);
		return -EINVAL;
	}

	/* DAI clock master masks */
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		if (STM32_I2S_IS_MASTER(i2s)) {
			dev_err(cpu_dai->dev, "previous DAI set master mode\n");
			return -EINVAL;
		}
		i2s->ms_flg = I2S_MS_SLAVE;

		if (STM32_I2S_IS_FULL_DUPLEX(i2s))
			cgfr |= I2S_CGFR_I2SCFG_SET(I2S_I2SMOD_FD_SLAVE);
		else if (STM32_I2S_IS_PLAYBACK(i2s))
			cgfr |= I2S_CGFR_I2SCFG_SET(I2S_I2SMOD_TX_SLAVE);
		else
			cgfr |= I2S_CGFR_I2SCFG_SET(I2S_I2SMOD_RX_SLAVE);
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		if (STM32_I2S_IS_SLAVE(i2s)) {
			dev_err(cpu_dai->dev, "previous DAI set slave mode\n");
			return -EINVAL;
		}
		i2s->ms_flg = I2S_MS_MASTER;

		if (STM32_I2S_IS_FULL_DUPLEX(i2s))
			cgfr |= I2S_CGFR_I2SCFG_SET(I2S_I2SMOD_FD_MASTER);
		else if (STM32_I2S_IS_PLAYBACK(i2s))
			cgfr |= I2S_CGFR_I2SCFG_SET(I2S_I2SMOD_TX_MASTER);
		else
			cgfr |= I2S_CGFR_I2SCFG_SET(I2S_I2SMOD_RX_MASTER);
		break;
	default:
		dev_err(cpu_dai->dev, "Unsupported mode %#x\n",
			fmt & SND_SOC_DAIFMT_MASTER_MASK);
		return -EINVAL;
	}

	i2s->fmt = fmt;
	return regmap_update_bits(i2s->regmap, STM32_I2S_CGFR_REG,
				  cgfr_mask, cgfr);
}

static int stm32_i2s_set_sysclk(struct snd_soc_dai *cpu_dai,
				int clk_id, unsigned int freq, int dir)
{
	struct stm32_i2s_data *i2s = snd_soc_dai_get_drvdata(cpu_dai);

	dev_dbg(cpu_dai->dev, "I2S MCLK frequency is %uHz\n", freq);

	if ((dir == SND_SOC_CLOCK_OUT) && STM32_I2S_IS_MASTER(i2s)) {
		i2s->mclk_rate = freq;

		/* Enable master clock if master mode and mclk-fs are set */
		return regmap_update_bits(i2s->regmap, STM32_I2S_CGFR_REG,
					  I2S_CGFR_MCKOE, I2S_CGFR_MCKOE);
	}

	return 0;
}

static int stm32_i2s_configure_clock(struct snd_soc_dai *cpu_dai,
				     struct snd_pcm_hw_params *params)
{
	struct stm32_i2s_data *i2s = snd_soc_dai_get_drvdata(cpu_dai);
	unsigned long i2s_clock_rate;
	unsigned int tmp, div, real_div, nb_bits, frame_len;
	unsigned int rate = params_rate(params);
	int ret;
	u32 cgfr, cgfr_mask;
	bool odd;

	if (i2s->refcount > 1) {
		if (rate != i2s->rate) {
			dev_err(cpu_dai->dev,
				"rate not compatible with active stream");
			return -EINVAL;
		}
		return 0;
	}
	i2s->rate = rate;

	if (!(rate % 11025))
		clk_set_parent(i2s->i2sclk, i2s->x11kclk);
	else
		clk_set_parent(i2s->i2sclk, i2s->x8kclk);
	i2s_clock_rate = clk_get_rate(i2s->i2sclk);

	/*
	 * mckl = mclk_ratio x ws
	 *   i2s mode : mclk_ratio = 256
	 *   dsp mode : mclk_ratio = 128
	 *
	 * mclk on
	 *   i2s mode : div = i2s_clk / (mclk_ratio * ws)
	 *   dsp mode : div = i2s_clk / (mclk_ratio * ws)
	 * mclk off
	 *   i2s mode : div = i2s_clk / (nb_bits x ws)
	 *   dsp mode : div = i2s_clk / (nb_bits x ws)
	 */
	if (i2s->mclk_rate) {
		tmp = DIV_ROUND_CLOSEST(i2s_clock_rate, i2s->mclk_rate);
	} else {
		frame_len = 32;
		if ((i2s->fmt & SND_SOC_DAIFMT_FORMAT_MASK) ==
		    SND_SOC_DAIFMT_DSP_A)
			frame_len = 16;

		/* master clock not enabled */
		ret = regmap_read(i2s->regmap, STM32_I2S_CGFR_REG, &cgfr);
		if (ret < 0)
			return ret;

		nb_bits = frame_len * ((cgfr & I2S_CGFR_CHLEN) + 1);
		tmp = DIV_ROUND_CLOSEST(i2s_clock_rate, (nb_bits * rate));
	}

	/* Check the parity of the divider */
	odd = tmp & 0x1;

	/* Compute the div prescaler */
	div = tmp >> 1;

	cgfr = I2S_CGFR_I2SDIV_SET(div) | (odd << I2S_CGFR_ODD_SHIFT);
	cgfr_mask = I2S_CGFR_I2SDIV_MASK | I2S_CGFR_ODD;

	real_div = ((2 * div) + odd);
	dev_dbg(cpu_dai->dev, "I2S clk: %ld, SCLK: %d\n",
		i2s_clock_rate, rate);
	dev_dbg(cpu_dai->dev, "Divider: 2*%d(div)+%d(odd) = %d\n",
		div, odd, real_div);

	if (((div == 1) && odd) || (div > I2S_CGFR_I2SDIV_MAX)) {
		dev_err(cpu_dai->dev, "Wrong divider setting\n");
		return -EINVAL;
	}

	if (!div && !odd)
		dev_warn(cpu_dai->dev, "real divider forced to 1\n");

	ret = regmap_update_bits(i2s->regmap, STM32_I2S_CGFR_REG,
				 cgfr_mask, cgfr);
	if (ret < 0)
		return ret;

	/* Set bitclock and frameclock to their inactive state */
	return regmap_update_bits(i2s->regmap, STM32_I2S_CFG2_REG,
				  I2S_CFG2_AFCNTR, I2S_CFG2_AFCNTR);
}

static int stm32_i2s_configure_channel(struct snd_soc_dai *cpu_dai,
				       struct snd_pcm_hw_params *params)
{
	struct stm32_i2s_data *i2s = snd_soc_dai_get_drvdata(cpu_dai);
	int format = params_width(params);
	u32 cfgr, cfgr_mask;

	if (i2s->refcount > 1) {
		if (format != i2s->format) {
			dev_err(cpu_dai->dev,
				"format not compatible with active stream");
			return -EINVAL;
		}
		return 0;
	}
	i2s->format = format;

	switch (format) {
	case 16:
		cfgr = I2S_CGFR_DATLEN_SET(I2S_I2SMOD_DATLEN_16);
		cfgr_mask = I2S_CGFR_DATLEN_MASK;
		break;
	case 32:
		cfgr = I2S_CGFR_DATLEN_SET(I2S_I2SMOD_DATLEN_32) |
					   I2S_CGFR_CHLEN;
		cfgr_mask = I2S_CGFR_DATLEN_MASK | I2S_CGFR_CHLEN;
		break;
	default:
		dev_err(cpu_dai->dev, "Unexpected format %d", format);
		return -EINVAL;
	}

	if (STM32_I2S_IS_SLAVE(i2s)) {
		/* As data length is either 16 or 32 bits, fixch always set */
		cfgr |= I2S_CGFR_FIXCH;
		cfgr_mask |= I2S_CGFR_FIXCH;
	}

	return regmap_update_bits(i2s->regmap, STM32_I2S_CGFR_REG,
				  cfgr_mask, cfgr);
}

static int stm32_i2s_startup(struct snd_pcm_substream *substream,
			     struct snd_soc_dai *cpu_dai)
{
	struct stm32_i2s_data *i2s = snd_soc_dai_get_drvdata(cpu_dai);
	int ret, ier;

	i2s->substream = substream;

	spin_lock(&i2s->lock_fd);
	i2s->refcount++;
	spin_unlock(&i2s->lock_fd);

	ret = regmap_update_bits(i2s->regmap, STM32_I2S_IFCR_REG,
				 I2S_IFCR_MASK, 0);
	if (ret < 0)
		return ret;

	/* Enable ITs */
	ier = I2S_IER_OVRIE | I2S_IER_UDRIE;
	if (STM32_I2S_IS_SLAVE(i2s))
		ier |= I2S_IER_TIFREIE;

	return regmap_update_bits(i2s->regmap, STM32_I2S_IER_REG, ier, ier);
}

static int stm32_i2s_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params,
			       struct snd_soc_dai *cpu_dai)
{
	struct stm32_i2s_data *i2s = snd_soc_dai_get_drvdata(cpu_dai);
	int format = params_width(params);
	unsigned int fthlv;
	int ret;

	if ((params_channels(params) == 1) &&
	    ((i2s->fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_DSP_A)) {
		dev_err(cpu_dai->dev, "Mono mode supported only by DSP_A\n");
		return -EINVAL;
	}

	/*
	 * fthlv is fifo threshold expressed as sample number
	 * fthlv = fifo size * threshold_ratio / (format / 8)
	 * where threshold_ratio = fifo_th / 4
	 */
	fthlv = STM32_I2S_FIFO_SIZE * 8 * i2s->fifo_th / (format * 4);

	regmap_update_bits(i2s->regmap, STM32_I2S_CFG1_REG, I2S_CFG1_FTHVL_MASK,
			   I2S_CFG1_FTHVL_SET(fthlv - 1));

	spin_lock(&i2s->lock_fd);

	ret = stm32_i2s_configure_channel(cpu_dai, params);
	if (ret < 0) {
		spin_unlock(&i2s->lock_fd);
		return ret;
	}

	if (STM32_I2S_IS_MASTER(i2s)) {
		ret = stm32_i2s_configure_clock(cpu_dai, params);
		if (ret < 0) {
			spin_unlock(&i2s->lock_fd);
			return ret;
		}
	}

	spin_unlock(&i2s->lock_fd);

	return 0;
}

static int stm32_i2s_trigger(struct snd_pcm_substream *substream, int cmd,
			     struct snd_soc_dai *cpu_dai)
{
	struct stm32_i2s_data *i2s = snd_soc_dai_get_drvdata(cpu_dai);
	int ret;
	bool stream_is_playback = false;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		stream_is_playback = true;

	/* This lock protects both start flag and SPE bit accesses */
	spin_lock(&i2s->lock_fd);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (stream_is_playback) {
			ret = regmap_update_bits(i2s->regmap,
						 STM32_I2S_CFG1_REG,
						 I2S_CFG1_TXDMAEN,
						 I2S_CFG1_TXDMAEN);
		} else {
			ret = regmap_update_bits(i2s->regmap,
						 STM32_I2S_CFG1_REG,
						 I2S_CFG1_RXDMAEN,
						 I2S_CFG1_RXDMAEN);
		}
		if (ret < 0)
			goto err_trigger;

		i2s->startcount++;
		if (STM32_I2S_IS_FULL_DUPLEX(i2s) && (i2s->startcount < 2)) {
			dev_dbg(cpu_dai->dev,
				"Full duplex device waiting for %s stream",
				stream_is_playback ? "capture" : "playback");
			spin_unlock(&i2s->lock_fd);
			return 0;
		}

		/* Enable i2s */
		dev_dbg(cpu_dai->dev, "start I2S\n");

		ret = regmap_update_bits(i2s->regmap, STM32_I2S_CR1_REG,
					 I2S_CR1_SPE, I2S_CR1_SPE);
		if (ret < 0) {
			dev_err(cpu_dai->dev, "Error %d enabling I2S\n", ret);
			goto err_trigger;
		}

		ret = regmap_update_bits(i2s->regmap, STM32_I2S_CR1_REG,
					 I2S_CR1_CSTART, I2S_CR1_CSTART);
		if (ret < 0) {
			dev_err(cpu_dai->dev, "Error %d starting I2S\n", ret);
			goto err_trigger;
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		dev_dbg(cpu_dai->dev, "stop I2S\n");

		ret = regmap_update_bits(i2s->regmap, STM32_I2S_CR1_REG,
					 I2S_CR1_SPE, 0);
		if (ret < 0) {
			dev_err(cpu_dai->dev, "Error %d disabling I2S\n", ret);
			goto err_trigger;
		}
		i2s->startcount = 0;

		if (stream_is_playback) {
			ret = regmap_update_bits(i2s->regmap,
						 STM32_I2S_CFG1_REG,
						 I2S_CFG1_TXDMAEN, 0);
		} else {
			ret = regmap_update_bits(i2s->regmap,
						 STM32_I2S_CFG1_REG,
						 I2S_CFG1_RXDMAEN, 0);
		}
		if (ret < 0)
			goto err_trigger;

		break;
	default:
		return -EINVAL;
	}

	spin_unlock(&i2s->lock_fd);
	return 0;

err_trigger:
	spin_unlock(&i2s->lock_fd);
	return ret;
}

static void stm32_i2s_shutdown(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *cpu_dai)
{
	struct stm32_i2s_data *i2s = snd_soc_dai_get_drvdata(cpu_dai);

	i2s->substream = NULL;

	spin_lock(&i2s->lock_fd);
	if (i2s->refcount > 0)
		i2s->refcount--;
	spin_unlock(&i2s->lock_fd);

	regmap_update_bits(i2s->regmap, STM32_I2S_CGFR_REG,
			   I2S_CGFR_MCKOE, (unsigned int)~I2S_CGFR_MCKOE);
}

static int stm32_i2s_dai_probe(struct snd_soc_dai *cpu_dai)
{
	struct stm32_i2s_data *i2s = dev_get_drvdata(cpu_dai->dev);
	struct snd_dmaengine_dai_dma_data *dma_data_tx = &i2s->dma_data_tx;
	struct snd_dmaengine_dai_dma_data *dma_data_rx = &i2s->dma_data_rx;

	/* Buswidth will be set by framework */
	dma_data_tx->addr_width = DMA_SLAVE_BUSWIDTH_UNDEFINED;
	dma_data_tx->addr = (dma_addr_t)(i2s->phys_addr) + STM32_I2S_TXDR_REG;
	dma_data_tx->maxburst = 1;
	dma_data_rx->addr_width = DMA_SLAVE_BUSWIDTH_UNDEFINED;
	dma_data_rx->addr = (dma_addr_t)(i2s->phys_addr) + STM32_I2S_RXDR_REG;
	dma_data_rx->maxburst = 1;

	i2s->dir = cpu_dai->id;
	switch (i2s->dir) {
	case I2S_DIR_TX:
		i2s->fifo_th = I2S_FIFO_TH_FULL;
		snd_soc_dai_init_dma_data(cpu_dai, dma_data_tx, NULL);
		break;
	case I2S_DIR_RX:
		i2s->fifo_th = I2S_FIFO_TH_HALF;
		snd_soc_dai_init_dma_data(cpu_dai, NULL, dma_data_rx);
		break;
	case I2S_DIR_FD:
		i2s->fifo_th = I2S_FIFO_TH_NONE;
		snd_soc_dai_init_dma_data(cpu_dai, dma_data_tx, dma_data_rx);
		break;
	}

	return 0;
}

static const struct regmap_config stm32_h7_i2s_regmap_conf = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = STM32_I2S_CGFR_REG,
	.readable_reg = stm32_i2s_readable_reg,
	.volatile_reg = stm32_i2s_volatile_reg,
	.writeable_reg = stm32_i2s_writeable_reg,
	.fast_io = true,
};

static const struct snd_soc_dai_ops stm32_i2s_pcm_dai_ops = {
	.set_sysclk	= stm32_i2s_set_sysclk,
	.set_fmt	= stm32_i2s_set_dai_fmt,
	.startup	= stm32_i2s_startup,
	.hw_params	= stm32_i2s_hw_params,
	.trigger	= stm32_i2s_trigger,
	.shutdown	= stm32_i2s_shutdown,
};

static const struct snd_pcm_hardware stm32_i2s_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP,
	.buffer_bytes_max = 8 * PAGE_SIZE,
	.period_bytes_max = 2048,
	.periods_min = 2,
	.periods_max = 8,
};

static const struct snd_dmaengine_pcm_config stm32_i2s_pcm_config = {
	.pcm_hardware	= &stm32_i2s_pcm_hw,
	.prepare_slave_config = snd_dmaengine_pcm_prepare_slave_config,
	.prealloc_buffer_size = PAGE_SIZE * 8,
};

static const struct snd_soc_component_driver stm32_i2s_component = {
	.name = "stm32-i2s",
};

static void stm32_i2s_dai_init(struct snd_soc_pcm_stream *stream,
			       char *stream_name)
{
	stream->stream_name = stream_name;
	stream->channels_min = 1;
	stream->channels_max = 2;
	stream->rates = SNDRV_PCM_RATE_8000_192000;
	stream->formats = SNDRV_PCM_FMTBIT_S16_LE |
				   SNDRV_PCM_FMTBIT_S32_LE;
}

static int stm32_i2s_dais_init(struct platform_device *pdev,
			       struct stm32_i2s_data *i2s)
{
	struct snd_soc_dai_driver *dai_ptr;
	int i;

	dai_ptr = devm_kzalloc(&pdev->dev, STM32_I2S_DAIS_NB *
			       sizeof(struct snd_soc_dai_driver), GFP_KERNEL);
	if (!dai_ptr)
		return -ENOMEM;

	i2s->dai_drv = dai_ptr;
	for (i = 0; i < STM32_I2S_DAIS_NB; i++) {
		dai_ptr->probe = stm32_i2s_dai_probe;
		dai_ptr->ops = &stm32_i2s_pcm_dai_ops;
		if (i == 0) {
			snprintf(i2s->dais_name[i], STM32_I2S_DAI_NAME_SIZE,
				 "%s-playback", i2s->ip_name);
			dai_ptr->id = I2S_DIR_TX;
			stm32_i2s_dai_init(&dai_ptr->playback, "CPU-Playback");
		}
		if (i == 1) {
			snprintf(i2s->dais_name[i], STM32_I2S_DAI_NAME_SIZE,
				 "%s-capture", i2s->ip_name);
			dai_ptr->id = I2S_DIR_RX;
			stm32_i2s_dai_init(&dai_ptr->capture, "CPU-Capture");
		}
		if (i == 2) {
			snprintf(i2s->dais_name[i], STM32_I2S_DAI_NAME_SIZE,
				 "%s-full-duplex", i2s->ip_name);
			dai_ptr->id = I2S_DIR_FD;
			stm32_i2s_dai_init(&dai_ptr->playback,
					   "CPU-FD-Playback");
			stm32_i2s_dai_init(&dai_ptr->capture,
					   "CPU-FD-Capture");
		}
		dai_ptr->name = i2s->dais_name[i];
		dai_ptr++;
	}

	return 0;
}

static const struct of_device_id stm32_i2s_ids[] = {
	{
		.compatible = "st,stm32h7-i2s",
		.data = &stm32_h7_i2s_regmap_conf
	},
	{},
};

static int stm32_i2s_parse_dt(struct platform_device *pdev,
			      struct stm32_i2s_data *i2s)
{
	struct device_node *np = pdev->dev.of_node;
	const struct of_device_id *of_id;
	struct reset_control *rst;
	struct resource *res;
	int irq, ret;

	if (!np)
		return -ENODEV;

	of_id = of_match_device(stm32_i2s_ids, &pdev->dev);
	if (of_id)
		i2s->regmap_conf = (const struct regmap_config *)of_id->data;
	else
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	i2s->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(i2s->base))
		return PTR_ERR(i2s->base);

	i2s->phys_addr = res->start;

	/* Get clocks */
	i2s->pclk = devm_clk_get(&pdev->dev, "pclk");
	if (IS_ERR(i2s->pclk)) {
		dev_err(&pdev->dev, "Could not get pclk\n");
		return PTR_ERR(i2s->pclk);
	}

	i2s->i2sclk = devm_clk_get(&pdev->dev, "i2sclk");
	if (IS_ERR(i2s->i2sclk)) {
		dev_err(&pdev->dev, "Could not get i2sclk\n");
		return PTR_ERR(i2s->i2sclk);
	}

	i2s->x8kclk = devm_clk_get(&pdev->dev, "x8k");
	if (IS_ERR(i2s->x8kclk)) {
		dev_err(&pdev->dev, "missing x8k parent clock\n");
		return PTR_ERR(i2s->x8kclk);
	}

	i2s->x11kclk = devm_clk_get(&pdev->dev, "x11k");
	if (IS_ERR(i2s->x11kclk)) {
		dev_err(&pdev->dev, "missing x11k parent clock\n");
		return PTR_ERR(i2s->x11kclk);
	}

	/* Get irqs */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "no irq for node %s\n", pdev->name);
		return -ENOENT;
	}

	ret = devm_request_irq(&pdev->dev, irq, stm32_i2s_isr, IRQF_ONESHOT,
			       dev_name(&pdev->dev), i2s);
	if (ret) {
		dev_err(&pdev->dev, "irq request returned %d\n", ret);
		return ret;
	}

	/* Reset */
	rst = devm_reset_control_get(&pdev->dev, NULL);
	if (!IS_ERR(rst)) {
		reset_control_assert(rst);
		udelay(2);
		reset_control_deassert(rst);
	}

	return 0;
}

static int stm32_i2s_probe(struct platform_device *pdev)
{
	struct stm32_i2s_data *i2s;
	int ret;

	i2s = devm_kzalloc(&pdev->dev, sizeof(*i2s), GFP_KERNEL);
	if (!i2s)
		return -ENOMEM;

	i2s->pdev = pdev;
	i2s->ms_flg = I2S_MS_NOT_SET;
	i2s->ip_name = strstr(dev_name(&pdev->dev), ".") + 1;
	spin_lock_init(&i2s->lock_fd);
	platform_set_drvdata(pdev, i2s);

	ret = stm32_i2s_parse_dt(pdev, i2s);
	if (ret)
		return ret;

	ret = stm32_i2s_dais_init(pdev, i2s);
	if (ret)
		return ret;

	i2s->regmap = devm_regmap_init_mmio(&pdev->dev, i2s->base,
					    i2s->regmap_conf);
	if (IS_ERR(i2s->regmap)) {
		dev_err(&pdev->dev, "regmap init failed\n");
		return PTR_ERR(i2s->regmap);
	}

	ret = clk_prepare_enable(i2s->pclk);
	if (ret) {
		dev_err(&pdev->dev, "Enable pclk failed: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(i2s->i2sclk);
	if (ret) {
		dev_err(&pdev->dev, "Enable i2sclk failed: %d\n", ret);
		goto err_pclk_disable;
	}

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &stm32_i2s_component,
					      i2s->dai_drv, STM32_I2S_DAIS_NB);
	if (ret)
		goto err_clocks_disable;

	ret = devm_snd_dmaengine_pcm_register(&pdev->dev,
					      &stm32_i2s_pcm_config, 0);
	if (ret)
		goto err_clocks_disable;

	/* Set SPI/I2S in i2s mode */
	ret = regmap_update_bits(i2s->regmap, STM32_I2S_CGFR_REG,
				 I2S_CGFR_I2SMOD, I2S_CGFR_I2SMOD);
	if (ret)
		goto err_clocks_disable;

	return ret;

err_clocks_disable:
	clk_disable_unprepare(i2s->i2sclk);
err_pclk_disable:
	clk_disable_unprepare(i2s->pclk);

	return ret;
}

static int stm32_i2s_remove(struct platform_device *pdev)
{
	struct stm32_i2s_data *i2s = platform_get_drvdata(pdev);

	clk_disable_unprepare(i2s->i2sclk);
	clk_disable_unprepare(i2s->pclk);

	return 0;
}

MODULE_DEVICE_TABLE(of, stm32_i2s_ids);

static struct platform_driver stm32_i2s_driver = {
	.driver = {
		.name = "st,stm32-i2s",
		.of_match_table = stm32_i2s_ids,
	},
	.probe = stm32_i2s_probe,
	.remove = stm32_i2s_remove,
};

module_platform_driver(stm32_i2s_driver);

MODULE_DESCRIPTION("STM32 Soc i2s Interface");
MODULE_AUTHOR("Olivier Moysan, <olivier.moysan@st.com>");
MODULE_ALIAS("platform:stm32-i2s");
MODULE_LICENSE("GPL v2");
