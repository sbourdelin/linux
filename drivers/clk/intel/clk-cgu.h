/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Copyright(c) 2018 Intel Corporation.
 *  Zhu YiXin <Yixin.zhu@intel.com>
 */

#ifndef __INTEL_CLK_H
#define __INTEL_CLK_H

#define PNAME(x) static const char *const x[] __initconst

struct intel_clk_mux {
	struct clk_hw	hw;
	struct regmap	*map;
	unsigned int	reg;
	u8		shift;
	u8		width;
	unsigned long	flags;
};

struct intel_clk_divider {
	struct clk_hw	hw;
	struct regmap	*map;
	unsigned int	reg;
	u8		shift;
	u8		width;
	unsigned long	flags;
	const struct clk_div_table	*table;
};

struct intel_clk_gate {
	struct clk_hw	hw;
	struct regmap	*map;
	unsigned int	reg;
	u8		shift;
	unsigned long	flags;
};

enum intel_clk_type {
	intel_clk_fixed,
	intel_clk_mux,
	intel_clk_divider,
	intel_clk_fixed_factor,
	intel_clk_gate,
};

/**
 * struct intel_clk_provider
 * @map: regmap type base address for register.
 * @np: device node
 * @clk_data: array of hw clocks and clk number.
 */
struct intel_clk_provider {
	struct regmap		*map;
	struct device_node	*np;
	struct clk_onecell_data	clk_data;
};

/**
 * struct intel_pll_clk
 * @id: plaform specific id of the clock.
 * @name: name of this pll clock.
 * @parent_names: name of the parent clock.
 * @num_parents: number of parents.
 * @flags: optional flags for basic clock.
 * @type: platform type of pll.
 * @reg: offset of the register.
 * @mult: init value of mulitplier.
 * @div: init value of divider.
 * @frac: init value of fraction.
 * @rate_table: table of pll clock rate.
 */
struct intel_pll_clk {
	unsigned int		id;
	const char		*name;
	const char		*const *parent_names;
	u8			num_parents;
	unsigned long		flags;
	enum intel_pll_type	type;
	int			reg;
	unsigned int		mult;
	unsigned int		div;
	unsigned int		frac;
	const struct intel_pll_rate_table *rate_table;
};

#define INTEL_PLL(_id, _type, _name, _pnames, _flags,	\
	    _reg, _rtable, _mult, _div, _frac)		\
	{						\
		.id		= _id,			\
		.type		= _type,		\
		.name		= _name,		\
		.parent_names	= _pnames,		\
		.num_parents	= ARRAY_SIZE(_pnames),	\
		.flags		= _flags,		\
		.reg		= _reg,			\
		.rate_table	= _rtable,		\
		.mult		= _mult,		\
		.div		= _div,			\
		.frac		= _frac			\
	}

/**
 * struct intel_osc_clk
 * @id: platform specific id of the clock.
 * @name: name of the osc clock.
 * @dt_freq: frequency node name in device tree.
 * @def_rate: default rate of the osc clock.
 * @flags: optional flags for basic clock.
 */
struct intel_osc_clk {
	unsigned int		id;
	const char		*name;
	const char		*dt_freq;
	const u32		def_rate;
};

#define INTEL_OSC(_id, _name, _freq, _rate)			\
	{						\
		.id		= _id,			\
		.name		= _name,		\
		.dt_freq	= _freq,		\
		.def_rate	= _rate,		\
	}

struct intel_clk_branch {
	unsigned int			id;
	enum intel_clk_type		type;
	const char			*name;
	const char			*const *parent_names;
	u8				num_parents;
	unsigned long			flags;
	unsigned int			mux_off;
	u8				mux_shift;
	u8				mux_width;
	unsigned long			mux_flags;
	unsigned int			mux_val;
	unsigned int			div_off;
	u8				div_shift;
	u8				div_width;
	unsigned long			div_flags;
	unsigned int			div_val;
	const struct clk_div_table	*div_table;
	unsigned int			gate_off;
	u8				gate_shift;
	unsigned long			gate_flags;
	unsigned int			gate_val;
	unsigned int			mult;
	unsigned int			div;
};

/* clock flags definition */
#define CLOCK_FLAG_VAL_INIT	BIT(16)
#define GATE_CLK_HW		BIT(17)
#define GATE_CLK_SW		BIT(18)
#define GATE_CLK_VT		BIT(19)

#define INTEL_MUX(_id, _name, _pname, _f, _reg,			\
	    _shift, _width, _cf, _v)				\
	{							\
		.id		= _id,				\
		.type		= intel_clk_mux,		\
		.name		= _name,			\
		.parent_names	= _pname,			\
		.num_parents	= ARRAY_SIZE(_pname),		\
		.flags		= _f,				\
		.mux_off	= _reg,				\
		.mux_shift	= _shift,			\
		.mux_width	= _width,			\
		.mux_flags	= _cf,				\
		.mux_val	= _v,				\
	}

#define INTEL_DIV(_id, _name, _pname, _f, _reg,			\
	    _shift, _width, _cf, _v, _dtable)			\
	{							\
		.id		= _id,				\
		.type		= intel_clk_divider,		\
		.name		= _name,			\
		.parent_names	= (const char *[]) { _pname },	\
		.num_parents	= 1,				\
		.flags		= _f,				\
		.div_off	= _reg,				\
		.div_shift	= _shift,			\
		.div_width	= _width,			\
		.div_flags	= _cf,				\
		.div_val	= _v,				\
		.div_table	= _dtable,			\
	}

#define INTEL_GATE(_id, _name, _pname, _f, _reg,		\
	     _shift, _cf, _v)					\
	{							\
		.id		= _id,				\
		.type		= intel_clk_gate,		\
		.name		= _name,			\
		.parent_names	= (const char *[]) { _pname },	\
		.num_parents	= !_pname ? 0 : 1,		\
		.flags		= _f,				\
		.gate_off	= _reg,				\
		.gate_shift	= _shift,			\
		.gate_flags	= _cf,				\
		.gate_val	= _v,				\
	}

#define INTEL_FIXED(_id, _name, _pname, _f, _reg,		\
	      _shift, _width, _cf, _freq, _v)			\
	{							\
		.id		= _id,				\
		.type		= intel_clk_fixed,		\
		.name		= _name,			\
		.parent_names	= (const char *[]) { _pname },	\
		.num_parents	= !_pname ? 0 : 1,		\
		.flags		= _f,				\
		.div_off	= _reg,				\
		.div_shift	= _shift,			\
		.div_width	= _width,			\
		.div_flags	= _cf,				\
		.div_val	= _v,				\
		.mux_flags	= _freq,			\
	}

#define INTEL_FIXED_FACTOR(_id, _name, _pname, _f, _reg,	\
	       _shift, _width, _cf, _v, _m, _d)			\
	{							\
		.id		= _id,				\
		.type		= intel_clk_fixed_factor,	\
		.name		= _name,			\
		.parent_names	= (const char *[]) { _pname },	\
		.num_parents	= 1,				\
		.flags		= _f,				\
		.div_off	= _reg,				\
		.div_shift	= _shift,			\
		.div_width	= _width,			\
		.div_flags	= _cf,				\
		.div_val	= _v,				\
		.mult		= _m,				\
		.div		= _d,				\
	}

void intel_set_clk_val(struct regmap *map, u32 reg, u8 shift,
		       u8 width, u32 set_val);
u32 intel_get_clk_val(struct regmap *map, u32 reg, u8 shift, u8 width);
void intel_clk_add_lookup(struct intel_clk_provider *ctx,
			  struct clk *clk, unsigned int id);
void __init intel_clk_of_add_provider(struct device_node *np,
				      struct intel_clk_provider *ctx);
struct intel_clk_provider * __init
intel_clk_init(struct device_node *np, struct regmap *map,
	       unsigned int nr_clks);
void __init intel_clk_register_osc(struct intel_clk_provider *ctx,
				   struct intel_osc_clk *osc,
				   unsigned int nr_clks);
void intel_clk_register_branches(struct intel_clk_provider *ctx,
				 struct intel_clk_branch *list,
				 unsigned int nr_clk);
void intel_clk_register_plls(struct intel_clk_provider *ctx,
			     struct intel_pll_clk *list, unsigned int nr_clk);
#endif /* __INTEL_CLK_H */
