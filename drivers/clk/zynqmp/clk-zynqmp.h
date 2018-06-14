/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Copyright (C) 2016-2018 Xilinx
 */

#ifndef __LINUX_CLK_ZYNQMP_H_
#define __LINUX_CLK_ZYNQMP_H_

#include <linux/spinlock.h>

#include <linux/firmware/xlnx-zynqmp.h>

/* Clock APIs payload parameters */
#define CLK_GET_NAME_RESP_LEN				16
#define CLK_GET_TOPOLOGY_RESP_WORDS			3
#define CLK_GET_PARENTS_RESP_WORDS			3
#define CLK_GET_ATTR_RESP_WORDS				1

enum topology_type {
	TYPE_INVALID,
	TYPE_MUX,
	TYPE_PLL,
	TYPE_FIXEDFACTOR,
	TYPE_DIV1,
	TYPE_DIV2,
	TYPE_GATE,
};

struct clk_hw *zynqmp_clk_register_pll(struct device *dev, const char *name,
				       u32 clk_id,
				       const char *parent,
				       unsigned long flag);

struct clk_hw *zynqmp_clk_register_gate(struct device *dev, const char *name,
					u32 clk_id,
					const char *parent,
					unsigned long flags,
					u8 clk_gate_flags);

struct clk_hw *zynqmp_clk_register_divider(struct device *dev,
					   const char *name,
					   u32 clk_id, u32 div_type,
					   const char *parent,
					   unsigned long flags,
					   u8 clk_divider_flags);

struct clk_hw *zynqmp_clk_register_mux(struct device *dev, const char *name,
				       u32 clk_id,
				       const char * const *parents,
				       u8 num_parents,
				       unsigned long flags,
				       u8 clk_mux_flags);
#endif
