/*
 * This file is part of STM32 DFSDM mfd driver
 *
 * Copyright (C) 2017, STMicroelectronics - All Rights Reserved
 * Author(s): Arnaud Pouliquen <arnaud.pouliquen@st.com> for STMicroelectronics.
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
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include <linux/mfd/stm32-dfsdm.h>

#include "stm32-dfsdm-reg.h"

#define DFSDM_UPDATE_BITS(regm, reg, mask, val) \
		WARN_ON(regmap_update_bits(regm, reg, mask, val))

#define DFSDM_REG_READ(regm, reg, val) \
		WARN_ON(regmap_read(regm, reg, val))

#define DFSDM_REG_WRITE(regm, reg, val) \
		WARN_ON(regmap_write(regm, reg, val))

#define STM32H7_DFSDM_NUM_FILTERS	4
#define STM32H7_DFSDM_NUM_INPUTS	8

enum dfsdm_clkout_src {
	DFSDM_CLK,
	AUDIO_CLK
};

struct stm32_dev_data {
	const struct stm32_dfsdm dfsdm;
	const struct regmap_config *regmap_cfg;
};

struct dfsdm_priv;

struct filter_params {
	unsigned int id;
	int irq;
	struct stm32_dfsdm_fl_event event;
	u32 event_mask;
	struct dfsdm_priv *priv; /* Cross ref for context */
	unsigned int ext_ch_mask;
	unsigned int scan_ch;
};

struct ch_params {
	struct stm32_dfsdm_channel ch;
};

struct dfsdm_priv {
	struct platform_device *pdev;
	struct stm32_dfsdm dfsdm;

	spinlock_t lock; /* Used for resource sharing & interrupt lock */

	/* Filters */
	struct filter_params *filters;
	unsigned int free_filter_mask;
	unsigned int scd_filter_mask;
	unsigned int ckab_filter_mask;

	/* Channels */
	struct stm32_dfsdm_channel *channels;
	unsigned int free_channel_mask;
	atomic_t n_active_ch;

	/* Clock */
	struct clk *clk;
	struct clk *aclk;
	unsigned int clkout_div;
	unsigned int clkout_freq_req;

	/* Registers*/
	void __iomem *base;
	struct regmap *regmap;
	phys_addr_t phys_base;
};

/*
 * Common
 */
static bool stm32_dfsdm_volatile_reg(struct device *dev, unsigned int reg)
{
	if (reg < DFSDM_FILTER_BASE_ADR)
		return false;

	/*
	 * Mask is done on register to avoid to list registers of all them
	 * filter instances.
	 */
	switch (reg & DFSDM_FILTER_REG_MASK) {
	case DFSDM_CR1(0) & DFSDM_FILTER_REG_MASK:
	case DFSDM_ISR(0) & DFSDM_FILTER_REG_MASK:
	case DFSDM_JDATAR(0) & DFSDM_FILTER_REG_MASK:
	case DFSDM_RDATAR(0) & DFSDM_FILTER_REG_MASK:
		return true;
	}

	return false;
}

static const struct regmap_config stm32h7_dfsdm_regmap_cfg = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = sizeof(u32),
	.max_register = DFSDM_CNVTIMR(STM32H7_DFSDM_NUM_FILTERS - 1),
	.volatile_reg = stm32_dfsdm_volatile_reg,
	.fast_io = true,
};

static const struct stm32_dev_data stm32h7_data = {
	.dfsdm.max_channels = STM32H7_DFSDM_NUM_INPUTS,
	.dfsdm.max_filters = STM32H7_DFSDM_NUM_FILTERS,
	.regmap_cfg = &stm32h7_dfsdm_regmap_cfg,
};

static int stm32_dfsdm_start_dfsdm(struct dfsdm_priv *priv)
{
	int ret;
	struct device *dev = &priv->pdev->dev;

	if (atomic_inc_return(&priv->n_active_ch) == 1) {
		ret = clk_prepare_enable(priv->clk);
		if (ret < 0) {
			dev_err(dev, "Failed to start clock\n");
			return ret;
		}
		if (priv->aclk) {
			ret = clk_prepare_enable(priv->aclk);
			if (ret < 0) {
				dev_err(dev, "Failed to start audio clock\n");
				return ret;
			}
		}
		DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(0),
				  DFSDM_CHCFGR1_CKOUTDIV_MASK,
				  DFSDM_CHCFGR1_CKOUTDIV(priv->clkout_div));

		DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(0),
				  DFSDM_CHCFGR1_DFSDMEN_MASK,
				  DFSDM_CHCFGR1_DFSDMEN(1));
	}

	dev_dbg(&priv->pdev->dev, "%s: n_active_ch %d\n", __func__,
		atomic_read(&priv->n_active_ch));

	return 0;
}

static void stm32_dfsdm_stop_dfsdm(struct dfsdm_priv *priv)
{
	if (atomic_dec_and_test(&priv->n_active_ch)) {
		DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(0),
				  DFSDM_CHCFGR1_DFSDMEN_MASK,
				  DFSDM_CHCFGR1_DFSDMEN(0));
		DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(0),
				  DFSDM_CHCFGR1_CKOUTDIV_MASK,
				  DFSDM_CHCFGR1_CKOUTDIV(0));
		clk_disable_unprepare(priv->clk);
		if (priv->aclk)
			clk_disable_unprepare(priv->aclk);
	}
	dev_dbg(&priv->pdev->dev, "%s: n_active_ch %d\n", __func__,
		atomic_read(&priv->n_active_ch));
}

static unsigned int stm32_dfsdm_get_clkout_divider(struct dfsdm_priv *priv,
						   unsigned long rate)
{
	unsigned int delta, div;

	/* div = 0 disables the clockout */
	if (!priv->clkout_freq_req)
		return 0;

	div = DIV_ROUND_CLOSEST(rate, priv->clkout_freq_req);

	delta = rate - (priv->clkout_freq_req * div);
	if (delta)
		dev_warn(&priv->pdev->dev,
			 "clkout not accurate. delta (Hz): %d\n", delta);

	dev_dbg(&priv->pdev->dev, "%s: clk: %lu (Hz), div %u\n",
		__func__, rate, div);

	return (div - 1);
}

/*
 * Filters
 */

static int stm32_dfsdm_clear_event(struct dfsdm_priv *priv, unsigned int fl_id,
				   unsigned int event, int mask)
{
	int val;

	switch (event) {
	case DFSDM_EVENT_INJ_EOC:
		DFSDM_REG_READ(priv->regmap, DFSDM_JDATAR(fl_id), &val);
		break;
	case DFSDM_EVENT_REG_EOC:
		DFSDM_REG_READ(priv->regmap, DFSDM_RDATAR(fl_id), &val);
		break;
	case DFSDM_EVENT_INJ_XRUN:
		DFSDM_UPDATE_BITS(priv->regmap, DFSDM_ICR(fl_id),
				  DFSDM_ICR_CLRJOVRF_MASK,
				  DFSDM_ICR_CLRJOVRF_MASK);
		break;
	case DFSDM_EVENT_REG_XRUN:
		DFSDM_UPDATE_BITS(priv->regmap, DFSDM_ICR(fl_id),
				  DFSDM_ICR_CLRROVRF_MASK,
				  DFSDM_ICR_CLRROVRF_MASK);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static irqreturn_t stm32_dfsdm_irq(int irq, void *arg)
{
	struct filter_params *params = arg;
	unsigned int status;
	struct dfsdm_priv *priv = params->priv;
	unsigned int event_mask = params->event_mask;

	DFSDM_REG_READ(priv->regmap, DFSDM_ISR(params->id), &status);

	if (status & DFSDM_ISR_JOVRF_MASK) {
		if (event_mask & DFSDM_EVENT_INJ_XRUN) {
			params->event.cb(&priv->dfsdm, params->id,
					 DFSDM_EVENT_INJ_XRUN, 0,
					 params->event.context);
		}
		stm32_dfsdm_clear_event(priv, params->id, DFSDM_EVENT_INJ_XRUN,
					0);
	}

	if (status & DFSDM_ISR_ROVRF_MASK) {
		if (event_mask & DFSDM_EVENT_REG_XRUN) {
			params->event.cb(&priv->dfsdm, params->id,
					 DFSDM_EVENT_REG_XRUN, 0,
					 params->event.context);
		}
		stm32_dfsdm_clear_event(priv, params->id, DFSDM_EVENT_REG_XRUN,
					0);
	}

	if (status & DFSDM_ISR_JEOCF_MASK) {
		if (event_mask & DFSDM_EVENT_INJ_EOC)
			params->event.cb(&priv->dfsdm, params->id,
					 DFSDM_EVENT_INJ_EOC, 0,
					 params->event.context);
		else
			stm32_dfsdm_clear_event(priv, params->id,
						DFSDM_EVENT_INJ_EOC, 0);
	}

	if (status & DFSDM_ISR_REOCF_MASK) {
		if (event_mask & DFSDM_EVENT_REG_EOC)
			params->event.cb(&priv->dfsdm, params->id,
					 DFSDM_EVENT_REG_EOC, 0,
					 params->event.context);
		else
			stm32_dfsdm_clear_event(priv, params->id,
						DFSDM_EVENT_REG_EOC, 0);
	}

	return IRQ_HANDLED;
}

static void stm32_dfsdm_configure_reg_conv(struct dfsdm_priv *priv,
					   unsigned int fl_id,
					   struct stm32_dfsdm_regular *params)
{
	unsigned int ch_id = params->ch_src;

	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR1(fl_id), DFSDM_CR1_RCH_MASK,
			  DFSDM_CR1_RCH(ch_id));
	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR1(fl_id), DFSDM_CR1_FAST_MASK,
			  DFSDM_CR1_FAST(params->fast_mode));

	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR1(fl_id), DFSDM_CR1_RCONT_MASK,
			  DFSDM_CR1_RCONT(params->cont_mode));
	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR1(fl_id), DFSDM_CR1_RDMAEN_MASK,
			  DFSDM_CR1_RDMAEN(params->dma_mode));
	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR1(fl_id), DFSDM_CR1_RSYNC_MASK,
			  DFSDM_CR1_RSYNC(params->sync_mode));

	priv->filters[fl_id].scan_ch = BIT(ch_id);
}

static void stm32_dfsdm_configure_inj_conv(struct dfsdm_priv *priv,
					   unsigned int fl_id,
					   struct stm32_dfsdm_injected *params)
{
	int val;

	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR1(fl_id), DFSDM_CR1_JSCAN_MASK,
			  DFSDM_CR1_JSCAN(params->scan_mode));
	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR1(fl_id), DFSDM_CR1_JDMAEN_MASK,
			  DFSDM_CR1_JDMAEN(params->dma_mode));

	val = (params->trigger == DFSDM_FILTER_EXT_TRIGGER) ?
	      params->trig_src : 0;
	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR1(fl_id),
			  DFSDM_CR1_JEXTSEL_MASK,
			  DFSDM_CR1_JEXTSEL(val));

	val = (params->trigger == DFSDM_FILTER_SYNC_TRIGGER) ? 1 : 0;
	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR1(fl_id), DFSDM_CR1_JSYNC_MASK,
			  DFSDM_CR1_JSYNC(val));
	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR1(fl_id), DFSDM_CR1_JEXTEN_MASK,
			  DFSDM_CR1_JEXTEN(params->trig_pol));
	priv->filters[fl_id].scan_ch = params->ch_group;

	DFSDM_REG_WRITE(priv->regmap, DFSDM_JCHGR(fl_id), params->ch_group);
}

/**
 * stm32_dfsdm_configure_filter - Configure filter.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @fl_id: Filter id.
 * @conv: Conversion type regular or injected.
 */
int stm32_dfsdm_configure_filter(struct stm32_dfsdm *dfsdm, unsigned int fl_id,
				 struct stm32_dfsdm_filter *fl_cfg)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv,
					       dfsdm);
	struct stm32_dfsdm_sinc_filter *sparams = &fl_cfg->sinc_params;

	dev_dbg(&priv->pdev->dev, "%s:config filter %d\n", __func__, fl_id);

	/* Average integrator oversampling */
	if ((!fl_cfg->int_oversampling) ||
	    (fl_cfg->int_oversampling > DFSDM_MAX_INT_OVERSAMPLING)) {
		dev_err(&priv->pdev->dev, "invalid integrator oversampling %d\n",
			fl_cfg->int_oversampling);
		return -EINVAL;
	}
	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_FCR(fl_id), DFSDM_FCR_IOSR_MASK,
			  DFSDM_FCR_IOSR((fl_cfg->int_oversampling - 1)));

	/* Oversamplings and filter*/
	if ((!sparams->oversampling) ||
	    (sparams->oversampling > DFSDM_MAX_FL_OVERSAMPLING)) {
		dev_err(&priv->pdev->dev, "invalid oversampling %d\n",
			sparams->oversampling);
		return -EINVAL;
	}

	if (sparams->order > DFSDM_SINC5_ORDER) {
		dev_err(&priv->pdev->dev, "invalid filter order %d\n",
			sparams->order);
		return -EINVAL;
	}

	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_FCR(fl_id), DFSDM_FCR_FOSR_MASK,
			  DFSDM_FCR_FOSR((sparams->oversampling - 1)));

	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_FCR(fl_id), DFSDM_FCR_FORD_MASK,
			  DFSDM_FCR_FORD(sparams->order));

	/* Conversion */
	if (fl_cfg->inj_params)
		stm32_dfsdm_configure_inj_conv(priv, fl_id, fl_cfg->inj_params);
	else if (fl_cfg->reg_params)
		stm32_dfsdm_configure_reg_conv(priv, fl_id, fl_cfg->reg_params);

	priv->filters[fl_id].event = fl_cfg->event;

	return 0;
}
EXPORT_SYMBOL_GPL(dfsdm_configure_filter);

/**
 * stm32_dfsdm_start_filter - Start filter conversion.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @fl_id: Filter id.
 * @conv: Conversion type regular or injected.
 */
void stm32_dfsdm_start_filter(struct stm32_dfsdm *dfsdm, unsigned int fl_id,
			      enum stm32_dfsdm_conv_type conv)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv, dfsdm);

	dev_dbg(&priv->pdev->dev, "%s:start filter %d\n", __func__, fl_id);

	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR1(fl_id), DFSDM_CR1_DFEN_MASK,
			  DFSDM_CR1_DFEN(1));

	if (conv == DFSDM_FILTER_REG_CONV) {
		DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR1(fl_id),
				  DFSDM_CR1_RSWSTART_MASK,
				  DFSDM_CR1_RSWSTART(1));
	} else if (conv == DFSDM_FILTER_SW_INJ_CONV) {
		DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR1(fl_id),
				  DFSDM_CR1_JSWSTART_MASK,
				  DFSDM_CR1_JSWSTART(1));
	}
}
EXPORT_SYMBOL_GPL(dfsdm_start_filter);

/**
 * stm32_dfsdm_stop_filter - Stop filter conversion.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @fl_id: Filter id.
 */
void stm32_dfsdm_stop_filter(struct stm32_dfsdm *dfsdm, unsigned int fl_id)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv, dfsdm);

	dev_dbg(&priv->pdev->dev, "%s:stop filter %d\n", __func__, fl_id);

	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR1(fl_id), DFSDM_CR1_DFEN_MASK,
			  DFSDM_CR1_DFEN(0));
	priv->filters[fl_id].scan_ch = 0;
}
EXPORT_SYMBOL_GPL(dfsdm_stop_filter);

/**
 * stm32_dfsdm_read_fl_conv - Read filter conversion.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @fl_id: Filter id.
 * @type: Regular or injected conversion.
 */
void stm32_dfsdm_read_fl_conv(struct stm32_dfsdm *dfsdm, unsigned int fl_id,
			      u32 *val, int *ch_id,
			      enum stm32_dfsdm_conv_type type)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv, dfsdm);
	int reg_v, offset;

	if (type == DFSDM_FILTER_REG_CONV)
		offset = DFSDM_RDATAR(fl_id);
	else
		offset = DFSDM_JDATAR(fl_id);

	DFSDM_REG_READ(priv->regmap, offset, &reg_v);

	*ch_id = reg_v & DFSDM_DATAR_CH_MASK;
	*val = reg_v & DFSDM_DATAR_DATA_MASK;
}
EXPORT_SYMBOL_GPL(dfsdm_read_fl_conv);

/**
 * stm32_dfsdm_get_filter - Get filter instance.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @fl_id: Filter instance to reserve.
 *
 * Reserves a DFSDM filter resource.
 */
int stm32_dfsdm_get_filter(struct stm32_dfsdm *dfsdm, unsigned int fl_id)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv,
					       dfsdm);
	struct device *dev = &priv->pdev->dev;

	spin_lock(&priv->lock);
	if (!(priv->free_filter_mask & BIT(fl_id))) {
		spin_unlock(&priv->lock);
		dev_err(dev, "filter resource %d available\n", fl_id);
		return -EBUSY;
	}
	priv->free_filter_mask &= ~BIT(fl_id);

	spin_unlock(&priv->lock);

	dev_dbg(dev, "%s: new mask %#x\n", __func__, priv->free_filter_mask);

	return 0;
}
EXPORT_SYMBOL_GPL(dfsdm_get_filter);

/**
 * stm32_dfsdm_release_filter - Release filter instance.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @fl_id: Filter id.
 *
 * Free the DFSDM filter resource.
 */
void stm32_dfsdm_release_filter(struct stm32_dfsdm *dfsdm, unsigned int fl_id)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv, dfsdm);

	spin_lock(&priv->lock);
	priv->free_filter_mask |= BIT(fl_id);
	spin_unlock(&priv->lock);
}
EXPORT_SYMBOL_GPL(dfsdm_release_filter);

/**
 * stm32_dfsdm_get_filter_dma_addr - Get register address for dma transfer.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @fl_id: Filter id.
 * @conv: Conversion type.
 */
dma_addr_t stm32_dfsdm_get_filter_dma_phy_addr(struct stm32_dfsdm *dfsdm,
					       unsigned int fl_id,
					       enum stm32_dfsdm_conv_type conv)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv, dfsdm);

	if (conv == DFSDM_FILTER_REG_CONV)
		return (dma_addr_t)(priv->phys_base + DFSDM_RDATAR(fl_id));
	else
		return (dma_addr_t)(priv->phys_base + DFSDM_JDATAR(fl_id));
}

/**
 * stm32_dfsdm_register_fl_event - Register filter event.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @fl_id: Filter id.
 * @event: Event to unregister.
 * @chan_mask: Mask of channels associated to filter.
 *
 * The function enables associated IRQ.
 */
int stm32_dfsdm_register_fl_event(struct stm32_dfsdm *dfsdm, unsigned int fl_id,
				  enum stm32_dfsdm_events event,
				  unsigned int chan_mask)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv, dfsdm);
	unsigned long flags, ulmask = chan_mask;
	int ret, i;

	dev_dbg(&priv->pdev->dev, "%s:for filter %d: event %#x ch_mask %#x\n",
		__func__, fl_id, event, chan_mask);

	if (event > DFSDM_EVENT_CKA)
		return -EINVAL;

	/* Clear interrupt before enable them */
	ret = stm32_dfsdm_clear_event(priv, fl_id, event, chan_mask);
	if (ret < 0)
		return ret;

	spin_lock_irqsave(&priv->lock, flags);
	/* Enable interrupts */
	switch (event) {
	case DFSDM_EVENT_SCD:
		for_each_set_bit(i, &ulmask, priv->dfsdm.max_channels) {
			DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(i),
					  DFSDM_CHCFGR1_SCDEN_MASK,
					  DFSDM_CHCFGR1_SCDEN(1));
		}
		if (!priv->scd_filter_mask)
			DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR2(0),
					  DFSDM_CR2_SCDIE_MASK,
					  DFSDM_CR2_SCDIE(1));
		priv->scd_filter_mask |= BIT(fl_id);
		break;
	case DFSDM_EVENT_CKA:
		for_each_set_bit(i, &ulmask, priv->dfsdm.max_channels) {
			DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(i),
					  DFSDM_CHCFGR1_CKABEN_MASK,
					  DFSDM_CHCFGR1_CKABEN(1));
		}
		if (!priv->ckab_filter_mask)
			DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR2(0),
					  DFSDM_CR2_CKABIE_MASK,
					  DFSDM_CR2_CKABIE(1));
		priv->ckab_filter_mask |= BIT(fl_id);
		break;
	default:
		DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR2(fl_id), event, event);
	}
	priv->filters[fl_id].event_mask |= event;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(dfsdm_register_fl_event);

/**
 * stm32_dfsdm_unregister_fl_event - Unregister filter event.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @fl_id: Filter id.
 * @event: Event to unregister.
 * @chan_mask: Mask of channels associated to filter.
 *
 * The function disables associated IRQ.
 */
int stm32_dfsdm_unregister_fl_event(struct stm32_dfsdm *dfsdm,
				    unsigned int fl_id,
				    enum stm32_dfsdm_events event,
				    unsigned int chan_mask)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv, dfsdm);
	unsigned long flags, ulmask = chan_mask;
	int i;

	dev_dbg(&priv->pdev->dev, "%s:for filter %d: event %#x ch_mask %#x\n",
		__func__, fl_id, event, chan_mask);

	if (event > DFSDM_EVENT_CKA)
		return -EINVAL;

	spin_lock_irqsave(&priv->lock, flags);
	/* Disable interrupts */
	switch (event) {
	case DFSDM_EVENT_SCD:
		for_each_set_bit(i, &ulmask, priv->dfsdm.max_channels) {
			DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(i),
					  DFSDM_CHCFGR1_SCDEN_MASK,
					  DFSDM_CHCFGR1_SCDEN(0));
		}
		priv->scd_filter_mask &= ~BIT(fl_id);
		if (!priv->scd_filter_mask)
			DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR2(0),
					  DFSDM_CR2_SCDIE_MASK,
					  DFSDM_CR2_SCDIE(0));
		break;
	case DFSDM_EVENT_CKA:
		for_each_set_bit(i, &ulmask, priv->dfsdm.max_channels) {
			DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(i),
					  DFSDM_CHCFGR1_CKABEN_MASK,
					  DFSDM_CHCFGR1_CKABEN(0));
		}
		priv->ckab_filter_mask &= ~BIT(fl_id);
		if (!priv->ckab_filter_mask)
			DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR2(0),
					  DFSDM_CR2_CKABIE_MASK,
					  DFSDM_CR2_CKABIE(0));
		break;
	default:
		DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CR2(fl_id), event, 0);
	}

	priv->filters[fl_id].event_mask &= ~event;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(dfsdm_unregister_fl_event);

/*
 * Channels
 */
static void stm32_dfsdm_init_channel(struct dfsdm_priv *priv,
				     struct stm32_dfsdm_channel *ch)
{
	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(ch->id),
			  DFSDM_CHCFGR1_DATMPX_MASK,
			  DFSDM_CHCFGR1_DATMPX(ch->type.source));
	if (ch->type.source == DFSDM_CHANNEL_EXTERNAL_INPUTS) {
		DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(ch->id),
				  DFSDM_CHCFGR1_SITP_MASK,
				  DFSDM_CHCFGR1_SITP(ch->serial_if.type));
		DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(ch->id),
				  DFSDM_CHCFGR1_SPICKSEL_MASK,
				DFSDM_CHCFGR1_SPICKSEL(ch->serial_if.spi_clk));
	}
	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(ch->id),
			  DFSDM_CHCFGR1_DATPACK_MASK,
			  DFSDM_CHCFGR1_DATPACK(ch->type.DataPacking));
	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(ch->id),
			  DFSDM_CHCFGR1_CHINSEL_MASK,
			  DFSDM_CHCFGR1_CHINSEL(ch->serial_if.pins));
}

/**
 * stm32_dfsdm_start_channel - Configure and activate DFSDM channel.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @ch: Filter id.
 * @cfg: Filter configuration.
 */
int stm32_dfsdm_start_channel(struct stm32_dfsdm *dfsdm, unsigned int ch_id,
			      struct stm32_dfsdm_ch_cfg *cfg)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv,
					       dfsdm);
	struct regmap *reg = priv->regmap;
	int ret;

	dev_dbg(&priv->pdev->dev, "%s: for channel %d\n", __func__, ch_id);

	ret = stm32_dfsdm_start_dfsdm(priv);
	if (ret < 0)
		return ret;

	DFSDM_UPDATE_BITS(reg, DFSDM_CHCFGR2(ch_id), DFSDM_CHCFGR2_DTRBS_MASK,
			  DFSDM_CHCFGR2_DTRBS(cfg->right_bit_shift));
	DFSDM_UPDATE_BITS(reg, DFSDM_CHCFGR2(ch_id), DFSDM_CHCFGR2_OFFSET_MASK,
			  DFSDM_CHCFGR2_OFFSET(cfg->offset));

	DFSDM_UPDATE_BITS(reg, DFSDM_CHCFGR1(ch_id), DFSDM_CHCFGR1_CHEN_MASK,
			  DFSDM_CHCFGR1_CHEN(1));

	/* Clear absence detection IRQ */
	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_ICR(0),
			  DFSDM_ICR_CLRCKABF_CH_MASK(ch_id),
			  DFSDM_ICR_CLRCKABF_CH(1, ch_id));

	return 0;
}
EXPORT_SYMBOL_GPL(dfsdm_start_channel);

/**
 * stm32_dfsdm_stop_channel - Deactivate channel.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @ch_id: DFSDM channel identifier.
 */
void stm32_dfsdm_stop_channel(struct stm32_dfsdm *dfsdm, unsigned int ch_id)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv, dfsdm);

	dev_dbg(&priv->pdev->dev, "%s:for channel %d\n", __func__, ch_id);

	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(ch_id),
			  DFSDM_CHCFGR1_CHEN_MASK,
			  DFSDM_CHCFGR1_CHEN(0));

	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(ch_id),
			  DFSDM_CHCFGR1_CKABEN_MASK, DFSDM_CHCFGR1_CKABEN(0));

	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(ch_id),
			  DFSDM_CHCFGR1_SCDEN_MASK, DFSDM_CHCFGR1_SCDEN(0));

	stm32_dfsdm_stop_dfsdm(priv);
}
EXPORT_SYMBOL_GPL(dfsdm_stop_channel);

/**
 * stm32_dfsdm_get_channel - Get channel instance.
 *
 * @dfsdm: handle used to retrieve dfsdm context.
 * @ch: DFSDM channel hardware parameters.
 *
 * Reserve DFSDM channel resource.
 */
int stm32_dfsdm_get_channel(struct stm32_dfsdm *dfsdm,
			    struct stm32_dfsdm_channel *ch)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv, dfsdm);
	unsigned int id = ch->id;

	dev_dbg(&priv->pdev->dev, "%s:get channel %d\n", __func__, id);

	if (id >= priv->dfsdm.max_channels) {
		dev_err(&priv->pdev->dev, "channel (%d) is not valid\n", id);
		return -EINVAL;
	}

	if ((ch->type.source != DFSDM_CHANNEL_EXTERNAL_INPUTS) &
	    (ch->serial_if.spi_clk != DFSDM_CHANNEL_SPI_CLOCK_EXTERNAL) &
	    (!priv->clkout_freq_req)) {
		dev_err(&priv->pdev->dev, "clkout not present\n");
		return -EINVAL;
	}

	spin_lock(&priv->lock);
	if (!(BIT(id) & priv->free_channel_mask)) {
		spin_unlock(&priv->lock);
		dev_err(&priv->pdev->dev, "channel (%d) already in use\n", id);
		return -EBUSY;
	}

	priv->free_channel_mask &= ~BIT(id);
	priv->channels[id] = *ch;
	spin_unlock(&priv->lock);

	dev_dbg(&priv->pdev->dev, "%s: new mask %#x\n", __func__,
		priv->free_channel_mask);

	/**
	 * Check clock constrainst between clkout and either
	 * dfsdm/audio clock:
	 * - In SPI mode (clkout is used): Fclk >= 4 * Fclkout
	 *   (e.g. CKOUTDIV >= 3)
	 * - In mancherster mode: Fclk >= 6 * Fclkout
	 */
	switch (ch->serial_if.type) {
	case DFSDM_CHANNEL_SPI_RISING:
	case DFSDM_CHANNEL_SPI_FALLING:
		if (priv->clkout_div && priv->clkout_div < 3)
			dev_warn(&priv->pdev->dev,
				 "Clock div should be higher than 3\n");
		break;
	case DFSDM_CHANNEL_MANCHESTER_RISING:
	case DFSDM_CHANNEL_MANCHESTER_FALLING:
		if (priv->clkout_div && priv->clkout_div < 5)
			dev_warn(&priv->pdev->dev,
				 "Clock div should be higher than 5\n");
		break;
	}

	stm32_dfsdm_init_channel(priv, ch);

	return 0;
}
EXPORT_SYMBOL_GPL(dfsdm_get_channel);

/**
 * stm32_dfsdm_release_channel - Release channel instance.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @ch_id: DFSDM channel identifier.
 *
 * Free the DFSDM channel resource.
 */
void stm32_dfsdm_release_channel(struct stm32_dfsdm *dfsdm, unsigned int ch_id)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv, dfsdm);

	spin_lock(&priv->lock);
	priv->free_channel_mask |= BIT(ch_id);
	spin_unlock(&priv->lock);
}
EXPORT_SYMBOL_GPL(dfsdm_release_channel);

/**
 * stm32_dfsdm_get_clk_out_rate - get clkout frequency.
 *
 * @dfsdm: handle used to retrieve dfsdm context.
 * @rate: clock out rate in Hz.
 *
 * Provide output frequency used for external ADC.
 * return EINVAL if clockout is not used else return 0.
 */
int stm32_dfsdm_get_clk_out_rate(struct stm32_dfsdm *dfsdm, unsigned long *rate)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv, dfsdm);
	unsigned long int clk_rate;

	if (!priv->clkout_div)
		return -EINVAL;

	clk_rate = clk_get_rate(priv->aclk ? priv->aclk : priv->clk);
	*rate = clk_rate / (priv->clkout_div + 1);
	dev_dbg(&priv->pdev->dev, "%s: clkout: %ld (Hz)\n", __func__, *rate);

	return 0;
}
EXPORT_SYMBOL_GPL(dfsdm_get_clk_out_rate);

static int stm32_dfsdm_parse_of(struct platform_device *pdev,
				struct dfsdm_priv *priv)
{
	struct device_node *node = pdev->dev.of_node;
	struct resource *res;
	int ret, val;

	if (!node)
		return -EINVAL;

	/* Get resources */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "Failed to get memory resource\n");
		return -ENODEV;
	}
	priv->phys_base = res->start;
	priv->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	ret = of_property_read_u32(node, "st,clkout-freq", &val);
	if (!ret) {
		if (!val) {
			dev_err(&priv->pdev->dev,
				"st,clkout-freq cannot be 0\n");
			return -EINVAL;
		}
		priv->clkout_freq_req = val;
	} else if (ret != -EINVAL) {
		dev_err(&priv->pdev->dev, "Failed to get st,clkout-freq\n");
		return ret;
	}

	/* Source clock */
	priv->clk = devm_clk_get(&pdev->dev, "dfsdm_clk");
	if (IS_ERR(priv->clk)) {
		dev_err(&pdev->dev, "No stm32_dfsdm_clk clock found\n");
		return -EINVAL;
	}

	priv->aclk = devm_clk_get(&pdev->dev, "audio_clk");
	if (IS_ERR(priv->aclk))
		priv->aclk = NULL;

	return 0;
};

static const struct of_device_id stm32_dfsdm_of_match[] = {
	{
		.compatible = "st,stm32h7-dfsdm",
		.data = &stm32h7_data
	},
	{}
};
MODULE_DEVICE_TABLE(of, stm32_dfsdm_of_match);

static int stm32_dfsdm_remove(struct platform_device *pdev)
{
	of_platform_depopulate(&pdev->dev);

	return 0;
}

static int stm32_dfsdm_probe(struct platform_device *pdev)
{
	struct dfsdm_priv *priv;
	struct device_node *pnode = pdev->dev.of_node;
	const struct of_device_id *of_id;
	const struct stm32_dev_data *dev_data;
	enum dfsdm_clkout_src clk_src;
	int ret, i;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pdev = pdev;

	/* Populate data structure depending on compatibility */
	of_id = of_match_node(stm32_dfsdm_of_match, pnode);
	if (!of_id->data) {
		dev_err(&pdev->dev, "Data associated to device is missing\n");
		return -EINVAL;
	}

	dev_data = (const struct stm32_dev_data *)of_id->data;

	ret = stm32_dfsdm_parse_of(pdev, priv);
	if (ret < 0)
		return ret;

	priv->regmap = devm_regmap_init_mmio(&pdev->dev, priv->base,
					    dev_data->regmap_cfg);
	if (IS_ERR(priv->regmap)) {
		ret = PTR_ERR(priv->regmap);
		dev_err(&pdev->dev, "%s: Failed to allocate regmap: %d\n",
			__func__, ret);
		return ret;
	}

	priv->dfsdm = dev_data->dfsdm;

	priv->filters = devm_kcalloc(&pdev->dev, dev_data->dfsdm.max_filters,
				     sizeof(*priv->filters), GFP_KERNEL);
	if (IS_ERR(priv->filters)) {
		ret = PTR_ERR(priv->filters);
		goto probe_err;
	}

	for (i = 0; i < dev_data->dfsdm.max_filters; i++) {
		struct filter_params *params = &priv->filters[i];

		params->id = i;
		params->irq = platform_get_irq(pdev, i);
		if (params->irq < 0) {
			dev_err(&pdev->dev, "Failed to get IRQ resource\n");
			ret = params->irq;
			goto probe_err;
		}

		ret = devm_request_irq(&pdev->dev, params->irq, stm32_dfsdm_irq,
				       0, dev_name(&pdev->dev), params);
		if (ret) {
			dev_err(&pdev->dev, "Failed to register interrupt\n");
			goto probe_err;
		}

		params->priv = priv;
	}

	priv->channels = devm_kcalloc(&pdev->dev, priv->dfsdm.max_channels,
				      sizeof(*priv->channels), GFP_KERNEL);
	if (IS_ERR(priv->channels)) {
		ret = PTR_ERR(priv->channels);
		goto probe_err;
	}
	priv->free_filter_mask = BIT(priv->dfsdm.max_filters) - 1;
	priv->free_channel_mask = BIT(priv->dfsdm.max_channels) - 1;

	platform_set_drvdata(pdev, &priv->dfsdm);
	spin_lock_init(&priv->lock);

	priv->clkout_div = stm32_dfsdm_get_clkout_divider(priv,
						    clk_get_rate(priv->clk));

	ret = of_platform_populate(pnode, NULL, NULL, &pdev->dev);
	if (ret < 0)
		goto probe_err;

	clk_src = priv->aclk ? AUDIO_CLK : DFSDM_CLK;

	DFSDM_UPDATE_BITS(priv->regmap, DFSDM_CHCFGR1(0),
			  DFSDM_CHCFGR1_CKOUTSRC_MASK,
			  DFSDM_CHCFGR1_CKOUTSRC(clk_src));
	return 0;

probe_err:
	return ret;
}

static struct platform_driver stm32_dfsdm_driver = {
	.probe = stm32_dfsdm_probe,
	.remove = stm32_dfsdm_remove,
	.driver = {
		.name = "stm32-dfsdm",
		.of_match_table = stm32_dfsdm_of_match,
	},
};

module_platform_driver(stm32_dfsdm_driver);

MODULE_AUTHOR("Arnaud Pouliquen <arnaud.pouliquen@st.com>");
MODULE_DESCRIPTION("STMicroelectronics STM32 dfsdm driver");
MODULE_LICENSE("GPL v2");
