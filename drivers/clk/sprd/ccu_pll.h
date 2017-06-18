/*
 * Spreadtrum clock pll configurations
 *
 * Copyright (C) 2015~2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _CCU_PLL_H_
#define _CCU_PLL_H_

#include "ccu_common.h"

struct reg_cfg {
	u32 val;
	u32 msk;
};

struct ccu_bit_field {
	u8 shift;
	u8 width;
};

enum {
	PLL_LOCK_DONE = 0,
	PLL_DIV_S,
	PLL_MOD_EN,
	PLL_SDM_EN,
	PLL_REFIN,
	PLL_IBIAS,
	PLL_N,
	PLL_NINT,
	PLL_KINT,
	PLL_PREDIV,
	PLL_POSTDIV,

	PLL_FACT_MAX
};

/*
 * struct ccu_pll - defination of adjustable pll clock
 *
 * @reg:	registers used to set the configuration of pll clock,
 *		reg[0] shows how many registers this pll clock uses.
 * @itable:	pll ibias table, itable[0] means how many items this
 *		table includes
 * @udelay	delay time after setting rate
 * @factors	used to calculate the pll clock rate
 * @fvco:	fvco threshold rate
 * @fflag:	fvco flag
 */
struct ccu_pll {
	const u32 *regs;
	const u64 *itable;
	u16 udelay;
	const struct ccu_bit_field *factors;
	u64 fvco;
	u16 fflag;

	struct ccu_common	common;
};

#define SPRD_CCU_PLL_WITH_ITABLE_FVCO(_struct, _name, _parent, _reg,	\
				      _regs, _itable, _udelay,		\
				      _factors,	_fvco, _fflag)		\
	struct ccu_pll _struct = {					\
		.regs		= _regs,				\
		.itable		= _itable,				\
		.udelay		= _udelay,				\
		.factors	= _factors,				\
		.fvco		= _fvco,				\
		.fflag		= _fflag,				\
		.common		= {					\
			.reg		= _reg,				\
			.hw.init	= CLK_HW_INIT(_name,		\
						      _parent,		\
						      &ccu_pll_ops,	\
						CLK_IGNORE_UNUSED),	\
		},							\
	}

#define SPRD_CCU_PLL_WITH_ITABLE(_struct, _name, _parent, _reg,		\
				 _regs, _itable, _udelay,		\
				 _factors)				\
	SPRD_CCU_PLL_WITH_ITABLE_FVCO(_struct, _name, _parent, _reg,	\
				      _regs, _itable, _udelay,		\
				      _factors, 0, 0)

static inline struct ccu_pll *hw_to_ccu_pll(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);

	return container_of(common, struct ccu_pll, common);
}

static inline u32 ccu_pll_readl(struct ccu_pll *pll, u8 index)
{
	struct ccu_common *common = &pll->common;

	if (WARN_ON(index >= pll->regs[0]))
		return 0;

	return readl(common->base + pll->regs[index + 1]);
}

static inline void ccu_pll_writel(struct ccu_pll *pll, u8 index,
				  u32 val, u32 msk)
{
	struct ccu_common *common = &pll->common;
	void __iomem *addr;
	u32 reg;

	if (WARN_ON(index >= pll->regs[0]))
		return;

	addr = common->base + pll->regs[index + 1];
	reg = readl(addr);
	writel((reg & ~msk) | val, addr);
}

extern const struct clk_ops ccu_pll_ops;

#endif /* _CCU_PLL_H_ */
