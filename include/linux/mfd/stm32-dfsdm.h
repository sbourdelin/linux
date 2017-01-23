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
#ifndef MDF_STM32_DFSDM_H
#define MDF_STM32_DFSDM_H

/*
 * Channel definitions
 */
#define DFSDM_CHANNEL_0    BIT(0)
#define DFSDM_CHANNEL_1    BIT(1)
#define DFSDM_CHANNEL_2    BIT(2)
#define DFSDM_CHANNEL_3    BIT(3)
#define DFSDM_CHANNEL_4    BIT(4)
#define DFSDM_CHANNEL_5    BIT(5)
#define DFSDM_CHANNEL_6    BIT(6)
#define DFSDM_CHANNEL_7    BIT(7)

/* DFSDM channel input data packing */
enum stm32_dfsdm_data_packing {
	DFSDM_CHANNEL_STANDARD_MODE,    /* Standard data packing mode */
	DFSDM_CHANNEL_INTERLEAVED_MODE, /* Interleaved data packing mode */
	DFSDM_CHANNEL_DUAL_MODE         /* Dual data packing mode */
};

/* DFSDM channel input multiplexer */
enum stm32_dfsdm_input_multiplexer {
	DFSDM_CHANNEL_EXTERNAL_INPUTS,    /* Data taken from external inputs */
	DFSDM_CHANNEL_INTERNAL_ADC,       /* Data taken from internal ADC */
	DFSDM_CHANNEL_INTERNAL_REGISTER,  /* Data taken from register */
};

/* DFSDM channel serial interface type */
enum stm32_dfsdm_serial_in_type {
	DFSDM_CHANNEL_SPI_RISING,         /* SPI with rising edge */
	DFSDM_CHANNEL_SPI_FALLING,        /* SPI with falling edge */
	DFSDM_CHANNEL_MANCHESTER_RISING,  /* Manchester with rising edge */
	DFSDM_CHANNEL_MANCHESTER_FALLING, /* Manchester with falling edge */
};

/* DFSDM channel serial spi clock source */
enum stm32_dfsdm_spi_clk_src {
	/* External SPI clock */
	DFSDM_CHANNEL_SPI_CLOCK_EXTERNAL,
	/* Internal SPI clock */
	DFSDM_CHANNEL_SPI_CLOCK_INTERNAL,
	/* Internal SPI clock divided by 2, falling edge */
	DFSDM_CHANNEL_SPI_CLOCK_INTERNAL_DIV2_FALLING,
	/* Internal SPI clock divided by 2, rising edge */
	DFSDM_CHANNEL_SPI_CLOCK_INTERNAL_DIV2_RISING
};

/* DFSDM channel input pins */
enum stm32_dfsdm_serial_in_select {
	/* Serial input taken from pins of the same channel (y) */
	DFSDM_CHANNEL_SAME_CHANNEL_PINS,
	/* Serial input taken from pins of the following channel (y + 1)*/
	DFSDM_CHANNEL_NEXT_CHANNEL_PINS,
};

/**
 * struct stm32_dfsdm_input_type - DFSDM channel init structure definition.
 * @DataPacking: Standard, interleaved or dual mode for internal register.
 * @source: channel source: internal DAC, serial input or memory.
 */
struct stm32_dfsdm_input_type {
	enum stm32_dfsdm_data_packing DataPacking;
	enum stm32_dfsdm_input_multiplexer source;
};

/**
 * struct stm32_dfsdm_serial_if - DFSDM serial interface parameters.
 * @type:	Serial interface type.
 * @spi_clk:	SPI clock source.
 * @pins:	select serial interface associated to the channel
 */
struct stm32_dfsdm_serial_if {
	enum stm32_dfsdm_serial_in_type type;
	enum stm32_dfsdm_spi_clk_src spi_clk;
	enum stm32_dfsdm_serial_in_select pins;
};

/**
 * struct stm32_dfsdm_channel - DFSDM channel hardware parameters.
 * @id:		DFSDM channel identifier.
 * @type:	DFSDM channel input parameters.
 * @serial_if:	DFSDM channel serial interface parameters.
 *		Mandatory for DFSDM_CHANNEL_EXTERNAL_INPUTS.
 */
struct stm32_dfsdm_channel {
	unsigned int id;
	struct stm32_dfsdm_input_type type;
	struct stm32_dfsdm_serial_if serial_if;
};

/**
 * struct stm32_dfsdm_ch_cfg - DFSDM channel config.
 * @offset:		DFSDM channel 24 bit calibration offset.
 * @right_bit_shift:	DFSDM channel right bit shift of the data result.
 */
struct stm32_dfsdm_ch_cfg {
	unsigned int offset;
	unsigned int right_bit_shift;
};

/*
 * Filter definitions
 */

#define DFSDM_MIN_INT_OVERSAMPLING 1
#define DFSDM_MAX_INT_OVERSAMPLING 256
#define DFSDM_MIN_FL_OVERSAMPLING 1
#define DFSDM_MAX_FL_OVERSAMPLING 1024

enum stm32_dfsdm_events {
	DFSDM_EVENT_INJ_EOC =	BIT(0), /* Injected end of conversion event */
	DFSDM_EVENT_REG_EOC =	BIT(1), /* Regular end of conversion event */
	DFSDM_EVENT_INJ_XRUN =	BIT(2), /* Injected conversion overrun event */
	DFSDM_EVENT_REG_XRUN =	BIT(3), /* Regular conversion overrun event */
	DFSDM_EVENT_AWD =	BIT(4), /* Analog watchdog event */
	DFSDM_EVENT_SCD =	BIT(5), /* Short circuit detector event */
	DFSDM_EVENT_CKA =	BIT(6), /* Clock abscence detection event */
};

#define STM32_DFSDM_EVENT_MASK 0x3F

/* DFSDM filter order  */
enum stm32_dfsdm_sinc_order {
	DFSDM_FASTSINC_ORDER, /* FastSinc filter type */
	DFSDM_SINC1_ORDER,    /* Sinc 1 filter type */
	DFSDM_SINC2_ORDER,    /* Sinc 2 filter type */
	DFSDM_SINC3_ORDER,    /* Sinc 3 filter type */
	DFSDM_SINC4_ORDER,    /* Sinc 4 filter type (N.A. for watchdog) */
	DFSDM_SINC5_ORDER,    /* Sinc 5 filter type (N.A. for watchdog) */
	DFSDM_NB_SINC_ORDER,
};

/* DFSDM filter order */
enum stm32_dfsdm_state {
	DFSDM_DISABLE,
	DFSDM_ENABLE,
};

/**
 * struct stm32_dfsdm_sinc_filter - DFSDM Sinc filter structure definition
 * @order: DFSM filter order.
 * @oversampling: DFSDM filter oversampling:
 *		  post processing filter: min = 1, max = 1024.
 */
struct stm32_dfsdm_sinc_filter {
	enum stm32_dfsdm_sinc_order order;
	unsigned int oversampling;
};

/* DFSDM filter conversion trigger */
enum stm32_dfsdm_trigger {
	DFSDM_FILTER_SW_TRIGGER,   /* Software trigger */
	DFSDM_FILTER_SYNC_TRIGGER, /* Synchronous with DFSDM0 */
	DFSDM_FILTER_EXT_TRIGGER,  /* External trigger (only for injected) */
};

/* DFSDM filter external trigger polarity */
enum stm32_dfsdm_filter_ext_trigger_pol {
	DFSDM_FILTER_EXT_TRIG_NO_TRIG,      /* Trigger disable */
	DFSDM_FILTER_EXT_TRIG_RISING_EDGE,  /* Rising edge */
	DFSDM_FILTER_EXT_TRIG_FALLING_EDGE, /* Falling edge */
	DFSDM_FILTER_EXT_TRIG_BOTH_EDGES,   /* Rising and falling edges */
};

/* DFSDM filter conversion type */
enum stm32_dfsdm_conv_type {
	DFSDM_FILTER_REG_CONV,      /* Regular conversion */
	DFSDM_FILTER_SW_INJ_CONV,   /* Injected conversion */
	DFSDM_FILTER_TRIG_INJ_CONV, /* Injected conversion */
};

/* DFSDM filter regular synchronous mode */
enum stm32_dfsdm_conv_rsync {
	DFSDM_FILTER_RSYNC_OFF, /* regular conversion asynchronous */
	DFSDM_FILTER_RSYNC_ON,  /* regular conversion synchronous with filter0*/
};

/**
 * struct stm32_dfsdm_regular - DFSDM filter conversion parameters structure
 * @ch_src:	Channel source from 0 to 7.
 * @fast_mode:	Enable/disable fast mode for regular conversion.
 * @dma_mode:	Enable/disable dma mode.
 * @cont_mode	Enable/disable continuous conversion.
 * @sync_mode	Enable/disable synchro mode.
 */
struct stm32_dfsdm_regular {
	unsigned int ch_src;
	bool fast_mode;
	bool dma_mode;
	bool cont_mode;
	bool sync_mode;
};

/**
 * struct stm32_dfsdm_injected - DFSDM filter  conversion parameters structure
 * @trigger:	Trigger used to start injected conversion.
 * @trig_src:	External trigger, 0 to 30 (refer to datasheet for details).
 * @trig_pol:	External trigger edge: software, rising, falling or both.
 * @scan_mode:	Enable/disable scan mode for injected conversion.
 * @ch_group:	mask containing channels to scan ( set bit y to scan
 *		channel y).
 * @dma_mode:	DFSDM channel input parameters.
 */
struct stm32_dfsdm_injected {
	enum stm32_dfsdm_trigger trigger;
	unsigned int trig_src;
	enum stm32_dfsdm_filter_ext_trigger_pol trig_pol;
	bool scan_mode;
	unsigned int ch_group;
	bool dma_mode;
};

struct stm32_dfsdm;

/**
 * struct stm32_dfsdm_fl_event - DFSDM filters event
 * @cb:	User event callback with parameters. be carful this function
 *		is called under threaded IRQ context:
 *			struct stm32_dfsdm *dfsdm: dfsdm handle,
 *			unsigned int fl_id: filter id,
 *			num stm32_dfsdm_events flag: event,
 *			param: parameter associated to the event,
 *			void *context: user context provided on registration.
 * @context: User param to retrieve context.
 */
struct stm32_dfsdm_fl_event {
	void (*cb)(struct stm32_dfsdm *, int, enum stm32_dfsdm_events,
		   unsigned int, void *);
	void *context;
};

/**
 * struct stm32_dfsdm_filter - DFSDM filter  conversion parameters structure
 * @reg_params:		DFSDM regular conversion parameters.
 *			this param is optional and not taken into account if
 *			@inj_params is defined.
 * @inj_params:		DFSDM injected conversion parameters (optional).
 * @filter_params:	DFSDM filter parameters.
 * @event:		Events callback.
 * @int_oversampling:	Integrator oversampling ratio for average purpose
 *			(range from 1 to 256).
 * @ext_det_ch_mask:	Extreme detector mask for channel selection
 *			mask generated using DFSDM_CHANNEL_0 to
 *			DFSDM_CHANNEL_7. If 0 feature is disable.
 */
struct stm32_dfsdm_filter {
	struct stm32_dfsdm_regular *reg_params;
	struct stm32_dfsdm_injected *inj_params;
	struct stm32_dfsdm_sinc_filter sinc_params;
	struct stm32_dfsdm_fl_event event;
	unsigned int int_oversampling;
};

/**
 * struct stm32_dfsdm - DFSDM context structure.
 *
 * @trig_info: Trigger name and id available last member name is null.
 * @max_channels: max number of channels available.
 * @max_filters: max number of filters available.
 *
 * Notice That structure is filled by mdf driver and must not be updated by
 * user.
 */
struct stm32_dfsdm {
	unsigned int max_channels;
	unsigned int max_filters;
};

int stm32_dfsdm_get_filter(struct stm32_dfsdm *dfsdm, unsigned int fl_id);
void stm32_dfsdm_release_filter(struct stm32_dfsdm *dfsdm, unsigned int fl_id);

dma_addr_t stm32_dfsdm_get_filter_dma_phy_addr(struct stm32_dfsdm *dfsdm,
					       unsigned int fl_id,
					       enum stm32_dfsdm_conv_type conv);

int stm32_dfsdm_configure_filter(struct stm32_dfsdm *dfsdm, unsigned int fl_id,
				 struct stm32_dfsdm_filter *filter);
void stm32_dfsdm_start_filter(struct stm32_dfsdm *dfsdm, unsigned int fl_id,
			      enum stm32_dfsdm_conv_type conv);
void stm32_dfsdm_stop_filter(struct stm32_dfsdm *dfsdm, unsigned int fl_id);

void stm32_dfsdm_read_fl_conv(struct stm32_dfsdm *dfsdm, unsigned int fl_id,
			      u32 *val, int *ch_id,
			      enum stm32_dfsdm_conv_type type);

int stm32_dfsdm_unregister_fl_event(struct stm32_dfsdm *dfsdm,
				    unsigned int fl_id,
				    enum stm32_dfsdm_events event,
				    unsigned int ch_mask);
int stm32_dfsdm_register_fl_event(struct stm32_dfsdm *dfsdm, unsigned int fl_id,
				  enum stm32_dfsdm_events event,
				  unsigned int ch_mask);

int stm32_dfsdm_get_channel(struct stm32_dfsdm *dfsdm,
			    struct stm32_dfsdm_channel *ch);
void stm32_dfsdm_release_channel(struct stm32_dfsdm *dfsdm, unsigned int ch_id);

int stm32_dfsdm_start_channel(struct stm32_dfsdm *dfsdm, unsigned int ch_id,
			      struct stm32_dfsdm_ch_cfg *cfg);
void stm32_dfsdm_stop_channel(struct stm32_dfsdm *dfsdm, unsigned int ch_id);

int stm32_dfsdm_get_clk_out_rate(struct stm32_dfsdm *dfsdm,
				 unsigned long *rate);

#endif
