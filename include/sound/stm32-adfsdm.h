/*
 * This file is part of STM32 DFSDM mfd driver API
 *
 * Copyright (C) 2017, STMicroelectronics - All Rights Reserved
 * Author(s): Arnaud Pouliquen <arnaud.pouliquen@st.com>.
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
#ifndef STM32_ADFSDM_H
#define STM32_ADFSDM_H

struct stm32_dfsdm_adc;

/**
 * struct stm32_dfsdm_hw_param - stm32 audio hardware params
 * @rate:		sampling rate
 * @sample_bits:	sample size in bits
 * @max_scaling:	effective scaling in bit computed by iio driver
 */
struct stm32_dfsdm_hw_param {
	unsigned int rate;
	unsigned int sample_bits;
	unsigned int *max_scaling;
};

/*
 * Potential improvement:
 * Following structure and functions could be generic and declared in
 * an asoc-iio.h
 */
struct stm32_adfsdm_codec_ops {
	/*
	 * Set the SPI or manchester input Frequency
	 * Optional: not use if DFSDM is master on SPI
	 */
	void (*set_sysclk)(struct stm32_dfsdm_adc *adc, unsigned int freq);

	/*
	 * Set expected audio sampling rate and format.
	 * Precision is returned to allow to rescale samples
	 */
	int (*set_hwparam)(struct stm32_dfsdm_adc *adc,
			   struct stm32_dfsdm_hw_param *params);

	/* Called when ASoC starts an audio stream setup. */
	int (*audio_startup)(struct stm32_dfsdm_adc *adc);

	/* Shuts down the audio stream. */
	void (*audio_shutdown)(struct stm32_dfsdm_adc *adc);

	/*
	 * Provides DMA source physicla addr to allow ALsa to handle DMA
	 * transfers.
	 */
	dma_addr_t (*get_dma_source)(struct stm32_dfsdm_adc *adc);

	/* Register callback to treat overrun issues */
	void (*register_xrun_cb)(struct stm32_dfsdm_adc *adc,
				 void (*overrun_cb)(void *context),
				 void *context);

};

/* stm32 dfsdm initalization data */
struct stm32_adfsdm_pdata {
	const struct stm32_adfsdm_codec_ops *ops;
	struct stm32_dfsdm_adc *adc;
};

#define STM32_ADFSDM_DRV_NAME "stm32-dfsdm-audio"
#endif
