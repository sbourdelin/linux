/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018 MediaTek Inc.
 * Author: Owen Chen <owen.chen@mediatek.com>
 */

#ifndef __DRV_CLK_MUX_H
#define __DRV_CLK_MUX_H

#include <linux/clk-provider.h>

struct mtk_clk_mux {
	struct clk_hw hw;
	struct regmap *regmap;

	const char *name;

	int mux_set_ofs;
	int mux_clr_ofs;
	int mux_ofs;
	int upd_ofs;

	s8 mux_shift;
	s8 mux_width;
	s8 gate_shift;
	s8 upd_shift;

	spinlock_t *lock;
};

extern const struct clk_ops mtk_mux_upd_ops;
extern const struct clk_ops mtk_mux_clr_set_upd_ops;

struct clk *mtk_clk_register_mux(const struct mtk_mux *mux,
				 struct regmap *regmap,
				 spinlock_t *lock);

#endif /* __DRV_CLK_MUX_H */
