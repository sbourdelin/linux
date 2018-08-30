// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2014-2018 Renesas Electronics Europe Limited
 *
 * Phil Edworthy <phil.edworthy@renesas.com>
 * Based on a driver originally written by Michel Pollet at Renesas.
 */

#include <dt-bindings/pinctrl/rzn1-pinctrl.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "core.h"

/*
 * The pinmux hardware has two levels. The first level functions goes from
 * 0 to 9, and the level 1 mode '15' (0xf) specifies that the second level
 * of pinmux should be used instead, that level has a lot more options,
 * and goes from 0 to ~60.
 *
 * For Linux, we've compounded both numbers together, so 0 to 9 is level 1,
 * and anything higher is in fact 10 + level 2 number, so we end up with one
 * value from 0 to 70 or so.
 *
 * There are 170 configurable pins (called PL_GPIO in the datasheet).
 *
 * Furthermore, the two MDIO outputs also have a mux each, that can be set
 * to 8 different values (including highz as well).
 *
 * So, for Linux, we also made up to extra "GPIOs" 170 and 171, and also added
 * extra functions to match their mux. This allow the device tree to be
 * completely transparent to these subtleties.
 */

struct rzn1_pinctrl_regs {
	union {
		u32	conf[170];
		u8	pad0[0x400];
	};
	u32	status_protect;	/* 0x400 */
	/* MDIO mux registers, level2 only */
	u32	l2_mdio[2];
};

/**
 * struct rzn1_pmx_func - describes rzn1 pinmux functions
 * @name: the name of this specific function
 * @groups: corresponding pin groups
 * @num_groups: the number of groups
 */
struct rzn1_pmx_func {
	const char *name;
	const char **groups;
	unsigned int num_groups;
};

/**
 * struct rzn1_pin_group - describes an rzn1 pin group
 * @name: the name of this specific pin group
 * @func: the name of the function selected by this group
 * @npins: the number of pins in this group array, i.e. the number of
 *	elements in .pins so we can iterate over that array
 * @pin_ids: array of pin_ids. pinctrl forces us to maintain such an array
 * @pins: array of pins
 */
struct rzn1_pin_group {
	const char *name;
	const char *func;
	unsigned int npins;
	u32 *pin_ids;
	u32 *pins;
};

struct rzn1_pinctrl {
	struct device *dev;
	struct clk *clk;
	struct pinctrl_dev *pctl;
	struct rzn1_pinctrl_regs __iomem *lev1;
	struct rzn1_pinctrl_regs __iomem *lev2;
	u32 lev1_protect_phys;
	u32 lev2_protect_phys;

	struct rzn1_pin_group *groups;
	unsigned int ngroups, maxgroups;

	struct rzn1_pmx_func *functions;
	unsigned int nfunctions;
};

#define RZN1_PINS_PROP "renesas,rzn1-pinmux-ids"

#define RZN1_PIN(pin) PINCTRL_PIN(pin, "pl_gpio"#pin)

static const struct pinctrl_pin_desc rzn1_pins[] = {
	RZN1_PIN(0), RZN1_PIN(1), RZN1_PIN(2), RZN1_PIN(3), RZN1_PIN(4),
	RZN1_PIN(5), RZN1_PIN(6), RZN1_PIN(7), RZN1_PIN(8), RZN1_PIN(9),
	RZN1_PIN(10), RZN1_PIN(11), RZN1_PIN(12), RZN1_PIN(13), RZN1_PIN(14),
	RZN1_PIN(15), RZN1_PIN(16), RZN1_PIN(17), RZN1_PIN(18), RZN1_PIN(19),
	RZN1_PIN(20), RZN1_PIN(21), RZN1_PIN(22), RZN1_PIN(23), RZN1_PIN(24),
	RZN1_PIN(25), RZN1_PIN(26), RZN1_PIN(27), RZN1_PIN(28), RZN1_PIN(29),
	RZN1_PIN(30), RZN1_PIN(31), RZN1_PIN(32), RZN1_PIN(33), RZN1_PIN(34),
	RZN1_PIN(35), RZN1_PIN(36), RZN1_PIN(37), RZN1_PIN(38), RZN1_PIN(39),
	RZN1_PIN(40), RZN1_PIN(41), RZN1_PIN(42), RZN1_PIN(43), RZN1_PIN(44),
	RZN1_PIN(45), RZN1_PIN(46), RZN1_PIN(47), RZN1_PIN(48), RZN1_PIN(49),
	RZN1_PIN(50), RZN1_PIN(51), RZN1_PIN(52), RZN1_PIN(53), RZN1_PIN(54),
	RZN1_PIN(55), RZN1_PIN(56), RZN1_PIN(57), RZN1_PIN(58), RZN1_PIN(59),
	RZN1_PIN(60), RZN1_PIN(61), RZN1_PIN(62), RZN1_PIN(63), RZN1_PIN(64),
	RZN1_PIN(65), RZN1_PIN(66), RZN1_PIN(67), RZN1_PIN(68), RZN1_PIN(69),
	RZN1_PIN(70), RZN1_PIN(71), RZN1_PIN(72), RZN1_PIN(73), RZN1_PIN(74),
	RZN1_PIN(75), RZN1_PIN(76), RZN1_PIN(77), RZN1_PIN(78), RZN1_PIN(79),
	RZN1_PIN(80), RZN1_PIN(81), RZN1_PIN(82), RZN1_PIN(83), RZN1_PIN(84),
	RZN1_PIN(85), RZN1_PIN(86), RZN1_PIN(87), RZN1_PIN(88), RZN1_PIN(89),
	RZN1_PIN(90), RZN1_PIN(91), RZN1_PIN(92), RZN1_PIN(93), RZN1_PIN(94),
	RZN1_PIN(95), RZN1_PIN(96), RZN1_PIN(97), RZN1_PIN(98), RZN1_PIN(99),
	RZN1_PIN(100), RZN1_PIN(101), RZN1_PIN(102), RZN1_PIN(103),
	RZN1_PIN(104), RZN1_PIN(105), RZN1_PIN(106), RZN1_PIN(107),
	RZN1_PIN(108), RZN1_PIN(109), RZN1_PIN(110), RZN1_PIN(111),
	RZN1_PIN(112), RZN1_PIN(113), RZN1_PIN(114), RZN1_PIN(115),
	RZN1_PIN(116), RZN1_PIN(117), RZN1_PIN(118), RZN1_PIN(119),
	RZN1_PIN(120), RZN1_PIN(121), RZN1_PIN(122), RZN1_PIN(123),
	RZN1_PIN(124), RZN1_PIN(125), RZN1_PIN(126), RZN1_PIN(127),
	RZN1_PIN(128), RZN1_PIN(129), RZN1_PIN(130), RZN1_PIN(131),
	RZN1_PIN(132), RZN1_PIN(133), RZN1_PIN(134), RZN1_PIN(135),
	RZN1_PIN(136), RZN1_PIN(137), RZN1_PIN(138), RZN1_PIN(139),
	RZN1_PIN(140), RZN1_PIN(141), RZN1_PIN(142), RZN1_PIN(143),
	RZN1_PIN(144), RZN1_PIN(145), RZN1_PIN(146), RZN1_PIN(147),
	RZN1_PIN(148), RZN1_PIN(149), RZN1_PIN(150), RZN1_PIN(151),
	RZN1_PIN(152), RZN1_PIN(153), RZN1_PIN(154), RZN1_PIN(155),
	RZN1_PIN(156), RZN1_PIN(157), RZN1_PIN(158), RZN1_PIN(159),
	RZN1_PIN(160), RZN1_PIN(161), RZN1_PIN(162), RZN1_PIN(163),
	RZN1_PIN(164), RZN1_PIN(165), RZN1_PIN(166), RZN1_PIN(167),
	RZN1_PIN(168), RZN1_PIN(169),
	PINCTRL_PIN(170, "mdio0"), PINCTRL_PIN(171, "mdio1")
};

/* Field positions and masks in the pinmux registers */
#define RZN1_L1_PIN_DRIVE_STRENGTH	10
#define RZN1_L1_PIN_PULL		8
#define RZN1_FUNCTION			0
#define RZN1_L1_FUNC_MASK		0xf
#define RZN1_L1_FUNCTION_L2		0xf

enum {
	MDIO_MUX_HIGHZ = 0,
	MDIO_MUX_MAC0,
	MDIO_MUX_MAC1,
	MDIO_MUX_ECAT,
	MDIO_MUX_S3_MDIO0,
	MDIO_MUX_S3_MDIO1,
	MDIO_MUX_HWRTOS,
	MDIO_MUX_SWITCH,
};

struct rzn1_pin_desc {
	u32	pin : 8, func : 7, has_func : 1, has_drive : 1,
		drive : 2, has_pull : 1, pull : 2;
};

/*
 * Breaks down the configuration word (as present in the DT) into
 * a manageable structural description
 */
static void rzn1_get_pin_desc_from_config(struct rzn1_pinctrl *ipctl,
					  u32 pin_config,
					  struct rzn1_pin_desc *o)
{
	struct rzn1_pin_desc p = {
		.pin = pin_config,
		.func = pin_config >> RZN1_MUX_FUNC_BIT,
		.has_func = pin_config >> RZN1_MUX_HAS_FUNC_BIT,
		.has_drive = pin_config >> RZN1_MUX_HAS_DRIVE_BIT,
		.drive = pin_config >> RZN1_MUX_DRIVE_BIT,
		.has_pull = pin_config >> RZN1_MUX_HAS_PULL_BIT,
		.pull = pin_config >> RZN1_MUX_PULL_BIT,
	};

	if (o)
		*o = p;
}

enum {
	LOCK_LEVEL1 = 0x1,
	LOCK_LEVEL2 = 0x2,
	LOCK_ALL = LOCK_LEVEL1 | LOCK_LEVEL2,
};

static void rzn1_hw_set_lock(struct rzn1_pinctrl *ipctl, u8 lock, u8 value)
{
	/*
	 * The pinmux configuration is locked by writing the physical address of
	 * the status_protect register to itself. It is unlocked by writing the
	 * address | 1.
	 */
	if (lock & LOCK_LEVEL1) {
		u32 val = ipctl->lev1_protect_phys | !(value & LOCK_LEVEL1);

		writel(val, &ipctl->lev1->status_protect);
	}

	if (lock & LOCK_LEVEL2) {
		u32 val = ipctl->lev2_protect_phys | !(value & LOCK_LEVEL2);

		writel(val, &ipctl->lev2->status_protect);
	}
}

static void rzn1_pinctrl_mdio_select(struct rzn1_pinctrl *ipctl, u8 mdio,
				     u32 func)
{
	dev_info(ipctl->dev, "setting mdio %d to 0x%x\n", mdio, func);

	rzn1_hw_set_lock(ipctl, LOCK_LEVEL2, LOCK_LEVEL2);
	writel(func, &ipctl->lev2->l2_mdio[mdio]);
	rzn1_hw_set_lock(ipctl, LOCK_LEVEL2, 0);
}

/*
 * Using a composite pin description, set the hardware pinmux registers
 * with the corresponding values.
 * Make sure to unlock write protection and reset it afterward.
 *
 * NOTE: There is no protection for potential concurrency, it is assumed these
 * calls are serialized already.
 */
static int rzn1_set_hw_pin_parameters(struct rzn1_pinctrl *ipctl,
				      u32 pin_config, u8 use_locks)
{
	struct rzn1_pin_desc p;
	u32 l1_cache;
	u32 l2_cache;
	u32 l1;
	u32 l2;

	rzn1_get_pin_desc_from_config(ipctl, pin_config, &p);

	if (p.pin >= RZN1_MDIO_BUS0 && p.pin <= RZN1_MDIO_BUS1) {
		if (p.has_func && p.func >= RZN1_FUNC_MDIO_MUX_HIGHZ &&
		    p.func <= RZN1_FUNC_MDIO_MUX_SWITCH) {
			p.pin -= RZN1_MDIO_BUS0;
			p.func -= RZN1_FUNC_MDIO_MUX_HIGHZ;
			dev_dbg(ipctl->dev, "MDIO MUX[%d] set to %d\n",
				p.pin, p.func);
			rzn1_pinctrl_mdio_select(ipctl, p.pin, p.func);
		} else {
			dev_warn(ipctl->dev, "MDIO[%d] Invalid configuration: %d\n",
				 p.pin - RZN1_MDIO_BUS0, p.func);
			return -EINVAL;
		}

		return 0;
	}

	/* Note here, we do not allow anything past the MDIO Mux values */
	if (p.pin >= ARRAY_SIZE(ipctl->lev1->conf) ||
	    p.func >= RZN1_FUNC_MDIO_MUX_HIGHZ)
		return -EINVAL;

	l1 = readl(&ipctl->lev1->conf[p.pin]);
	l1_cache = l1;
	l2 = readl(&ipctl->lev2->conf[p.pin]);
	l2_cache = l2;

	if (p.has_drive) {
		l1 &= ~(0x3 << RZN1_L1_PIN_DRIVE_STRENGTH);
		l1 |= (p.drive << RZN1_L1_PIN_DRIVE_STRENGTH);
	}

	if (p.has_pull) {
		l1 &= ~(0x3 << RZN1_L1_PIN_PULL);
		l1 |= (p.pull << RZN1_L1_PIN_PULL);
	}

	if (p.has_func) {
		if (p.func < RZN1_FUNC_LEVEL2_OFFSET) {
			l1 &= ~(RZN1_L1_FUNC_MASK << RZN1_FUNCTION);
			l1 |= (p.func << RZN1_FUNCTION);
		} else {
			l1 &= ~(RZN1_L1_FUNC_MASK << RZN1_FUNCTION);
			l1 |= (RZN1_L1_FUNCTION_L2 << RZN1_FUNCTION);

			l2 = p.func - RZN1_FUNC_LEVEL2_OFFSET;
		}
	}

	/* If either configuration changes, we update both anyway */
	if (l1 != l1_cache || l2 != l2_cache) {
		rzn1_hw_set_lock(ipctl, use_locks, LOCK_ALL);
		writel(l1, &ipctl->lev1->conf[p.pin]);
		writel(l2, &ipctl->lev2->conf[p.pin]);
		rzn1_hw_set_lock(ipctl, use_locks, 0);
	}

	return 0;
}

static const struct rzn1_pin_group *rzn1_pinctrl_find_group_by_name(
	const struct rzn1_pinctrl *ipctl, const char *name)
{
	const struct rzn1_pin_group *grp = NULL;
	int i;

	for (i = 0; i < ipctl->ngroups; i++) {
		if (!strcmp(ipctl->groups[i].name, name)) {
			grp = &ipctl->groups[i];
			break;
		}
	}

	return grp;
}

static int rzn1_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct rzn1_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);

	return ipctl->ngroups;
}

static const char *rzn1_get_group_name(struct pinctrl_dev *pctldev,
				       unsigned int selector)
{
	struct rzn1_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);

	return ipctl->groups[selector].name;
}

static int rzn1_get_group_pins(struct pinctrl_dev *pctldev,
			       unsigned int selector, const unsigned int **pins,
			       unsigned int *npins)
{
	struct rzn1_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);

	if (selector >= ipctl->ngroups)
		return -EINVAL;

	*pins = ipctl->groups[selector].pins;
	*npins = ipctl->groups[selector].npins;

	return 0;
}

static void rzn1_pin_dbg_show(struct pinctrl_dev *pctldev, struct seq_file *s,
			      unsigned int offset)
{
	seq_printf(s, "%s", dev_name(pctldev->dev));
}

static int rzn1_dt_node_to_map(struct pinctrl_dev *pctldev,
			       struct device_node *np,
			       struct pinctrl_map **map, unsigned int *num_maps)
{
	struct rzn1_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct rzn1_pin_group *grp;
	struct pinctrl_map *new_map;
	int map_num = 2;

	/*
	 * First find the group of this node and check if we need create
	 * config maps for pins
	 */
	grp = rzn1_pinctrl_find_group_by_name(ipctl, np->name);
	if (!grp) {
		dev_err(ipctl->dev, "unable to find group for node %pOF\n", np);
		return -EINVAL;
	}

	new_map = devm_kmalloc_array(ipctl->dev, map_num, sizeof(*new_map),
				     GFP_KERNEL);
	if (!new_map)
		return -ENOMEM;

	*map = new_map;
	*num_maps = map_num;

	new_map[0].type = PIN_MAP_TYPE_MUX_GROUP;
	new_map[0].data.mux.function = grp->func;
	new_map[0].data.mux.group = grp->name;

	new_map[1].type = PIN_MAP_TYPE_CONFIGS_GROUP;
	new_map[1].data.configs.group_or_pin = grp->name;
	new_map[1].data.configs.configs = (unsigned long *)grp->pin_ids;
	new_map[1].data.configs.num_configs = grp->npins;

	dev_dbg(pctldev->dev, "maps: function %s group %s (%d pins)\n",
		(*map)->data.mux.function, (*map)->data.mux.group,
		grp->npins);

	return 0;
}

static void rzn1_dt_free_map(struct pinctrl_dev *pctldev,
			     struct pinctrl_map *map, unsigned int num_maps)
{
	struct rzn1_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);

	devm_kfree(ipctl->dev, map);
}

static const struct pinctrl_ops rzn1_pctrl_ops = {
	.get_groups_count = rzn1_get_groups_count,
	.get_group_name = rzn1_get_group_name,
	.get_group_pins = rzn1_get_group_pins,
	.pin_dbg_show = rzn1_pin_dbg_show,
	.dt_node_to_map = rzn1_dt_node_to_map,
	.dt_free_map = rzn1_dt_free_map,
};

static int rzn1_pmx_set_mux(struct pinctrl_dev *pctldev, unsigned int selector,
			    unsigned int group)
{
	struct rzn1_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	struct rzn1_pin_group *grp;

	/*
	 * Configure the mux mode for each pin in the group for a specific
	 * function.
	 */
	grp = &ipctl->groups[group];

	dev_dbg(ipctl->dev, "enable function %s(%d) group %s(%d)\n",
		ipctl->functions[selector].name, selector, grp->name, group);
	/*
	 * There's not much to do here as the individual pin callback is going
	 * to be called anyway
	 */

	return 0;
}

static int rzn1_pmx_get_funcs_count(struct pinctrl_dev *pctldev)
{
	struct rzn1_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);

	return ipctl->nfunctions;
}

static const char *rzn1_pmx_get_func_name(struct pinctrl_dev *pctldev,
					  unsigned int selector)
{
	struct rzn1_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);

	return ipctl->functions[selector].name;
}

static int rzn1_pmx_get_groups(struct pinctrl_dev *pctldev,
			       unsigned int selector,
			       const char * const **groups,
			       unsigned int * const num_groups)
{
	struct rzn1_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);

	*groups = ipctl->functions[selector].groups;
	*num_groups = ipctl->functions[selector].num_groups;

	return 0;
}

static const struct pinmux_ops rzn1_pmx_ops = {
	.get_functions_count = rzn1_pmx_get_funcs_count,
	.get_function_name = rzn1_pmx_get_func_name,
	.get_function_groups = rzn1_pmx_get_groups,
	.set_mux = rzn1_pmx_set_mux,
};

static int rzn1_pinconf_get(struct pinctrl_dev *pctldev, unsigned int pin_id,
			    unsigned long *config)
{
	struct rzn1_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);

	pin_id &= 0xff;
	if (pin_id >= ARRAY_SIZE(ipctl->lev1->conf))
		return -EINVAL;

	*config = readl(&ipctl->lev1->conf[pin_id]) & 0xf;
	if (*config == 0xf)
		*config = (readl(&ipctl->lev2->conf[pin_id]) & 0x3f) +
				RZN1_FUNC_LEVEL2_OFFSET;
	*config = (*config << RZN1_MUX_FUNC_BIT) | pin_id;

	return 0;
}

static int rzn1_pinconf_set(struct pinctrl_dev *pctldev, unsigned int pin_id,
			    unsigned long *configs, unsigned int num_configs)
{
	struct rzn1_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	int i;

	dev_dbg(ipctl->dev, "pinconf set pin %s (%d configs)\n",
		rzn1_pins[pin_id].name, num_configs);

	for (i = 0; i < num_configs; i++)
		rzn1_set_hw_pin_parameters(ipctl, configs[i], LOCK_ALL);

	return 0;
}

static int rzn1_pin_config_group_set(struct pinctrl_dev *pctldev,
				     unsigned int selector,
				     unsigned long *configs,
				     unsigned int num_configs)
{
	struct rzn1_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	struct rzn1_pin_group *grp = &ipctl->groups[selector];
	int i;

	/*
	 * Configure the mux mode for each pin in the group for a specific
	 * function.
	 */
	dev_dbg(ipctl->dev, "group set %s selector:%d configs:%p/%d\n",
		grp->name, selector, configs, num_configs);

	rzn1_hw_set_lock(ipctl, LOCK_ALL, LOCK_ALL);
	for (i = 0; i < num_configs; i++)
		rzn1_set_hw_pin_parameters(ipctl, configs[i], 0);
	rzn1_hw_set_lock(ipctl, LOCK_ALL, 0);

	return 0;
}

static void rzn1_pinconf_dbg_show(struct pinctrl_dev *pctldev,
				  struct seq_file *s, unsigned int pin_id)
{
	unsigned long config = pin_id;

	seq_printf(s, "0x%lx", config);
}

static void rzn1_pinconf_group_dbg_show(struct pinctrl_dev *pctldev,
					struct seq_file *s,
					unsigned int group)
{
	struct rzn1_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	struct rzn1_pin_group *grp;
	unsigned long config;
	const char *name;
	int i, ret;

	if (group > ipctl->ngroups)
		return;

	seq_puts(s, "\n");
	grp = &ipctl->groups[group];
	for (i = 0; i < grp->npins; i++) {
		name = pin_get_name(pctldev, grp->pin_ids[i] & 0xff);
		ret = rzn1_pinconf_get(pctldev, grp->pin_ids[i], &config);
		if (ret)
			return;

		seq_printf(s, "%s: 0x%lx", name, config);
	}
}

static const struct pinconf_ops rzn1_pinconf_ops = {
	.pin_config_get = rzn1_pinconf_get,
	.pin_config_set = rzn1_pinconf_set,
	.pin_config_group_set = rzn1_pin_config_group_set,
	.pin_config_dbg_show = rzn1_pinconf_dbg_show,
	.pin_config_group_dbg_show = rzn1_pinconf_group_dbg_show,
};

static struct pinctrl_desc rzn1_pinctrl_desc = {
	.pctlops = &rzn1_pctrl_ops,
	.pmxops = &rzn1_pmx_ops,
	.confops = &rzn1_pinconf_ops,
	.owner = THIS_MODULE,
};

static int rzn1_pinctrl_parse_groups(struct device_node *np,
				     struct rzn1_pin_group *grp,
				     struct rzn1_pinctrl *ipctl)
{
	int size;
	const __be32 *list;
	int i;

	dev_dbg(ipctl->dev, "%s: %s\n", __func__, np->name);

	/* Initialise group */
	grp->name = np->name;

	/*
	 * The binding format is
	 *	renesas,rzn1-pinmux-ids = <PIN_FUNC_ID CONFIG ...>,
	 * do sanity check and calculate pins number
	 */
	list = of_get_property(np, RZN1_PINS_PROP, &size);
	if (!list) {
		dev_err(ipctl->dev,
			"no " RZN1_PINS_PROP " property in node %s\n",
			np->full_name);
		return -EINVAL;
	}

	/* We do not check return since it's safe node passed down */
	if (!size) {
		dev_err(ipctl->dev, "Invalid " RZN1_PINS_PROP " in node %s\n",
			np->full_name);
		return -EINVAL;
	}

	grp->npins = size / sizeof(list[0]);
	if (!grp->npins)
		return 0;

	grp->pin_ids = devm_kmalloc_array(ipctl->dev,
					  grp->npins, sizeof(grp->pin_ids[0]),
					  GFP_KERNEL);
	grp->pins = devm_kmalloc_array(ipctl->dev,
				       grp->npins, sizeof(grp->pins[0]),
				       GFP_KERNEL);
	if (!grp->pin_ids || !grp->pins)
		return -ENOMEM;

	for (i = 0; i < grp->npins; i++) {
		u32 pin_id = be32_to_cpu(*list++);

		grp->pins[i] = pin_id & 0xff;
		grp->pin_ids[i] = pin_id;
	}

	return grp->npins;
}

static int rzn1_pinctrl_count_function_groups(struct device_node *np)
{
	struct device_node *child;
	int count = 0;

	if (of_property_count_u32_elems(np, RZN1_PINS_PROP) > 0)
		count++;

	for_each_child_of_node(np, child) {
		if (of_property_count_u32_elems(child, RZN1_PINS_PROP) > 0)
			count++;
	}

	return count;
}

static int rzn1_pinctrl_parse_functions(struct device_node *np,
					struct rzn1_pinctrl *ipctl, u32 index)
{
	struct device_node *child;
	struct rzn1_pmx_func *func;
	struct rzn1_pin_group *grp;
	u32 i = 0;
	int ret;

	dev_dbg(ipctl->dev, "parse function(%d): %s\n", index, np->name);

	func = &ipctl->functions[index];

	/* Initialise function */
	func->name = np->name;
	func->num_groups = rzn1_pinctrl_count_function_groups(np);
	dev_dbg(ipctl->dev, "function %s has %d groups\n",
		np->name, func->num_groups);
	if (func->num_groups == 0) {
		dev_err(ipctl->dev, "no groups defined in %s\n", np->full_name);
		return -EINVAL;
	}

	func->groups = devm_kmalloc_array(ipctl->dev,
					  func->num_groups, sizeof(char *),
					  GFP_KERNEL);
	if (!func->groups)
		return -ENOMEM;

	if (of_property_count_u32_elems(np, RZN1_PINS_PROP) > 0) {
		func->groups[i] = np->name;
		grp = &ipctl->groups[ipctl->ngroups];
		grp->func = func->name;
		ret = rzn1_pinctrl_parse_groups(np, grp, ipctl);
		if (ret < 0)
			return ret;
		if (ret > 0) {
			i++;
			ipctl->ngroups++;
		}
	}

	for_each_child_of_node(np, child) {
		func->groups[i] = child->name;
		grp = &ipctl->groups[ipctl->ngroups];
		grp->func = func->name;
		ret = rzn1_pinctrl_parse_groups(child, grp, ipctl);
		if (ret < 0)
			return ret;
		if (ret > 0) {
			i++;
			ipctl->ngroups++;
		}
	}

	dev_dbg(ipctl->dev, "function %s parsed %d/%d groups\n",
		np->name, i, func->num_groups);

	return 0;
}

static int rzn1_pinctrl_probe_dt(struct platform_device *pdev,
				 struct rzn1_pinctrl *ipctl)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child;
	u32 nfuncs = 0;
	u32 i = 0;
	int ret;

	nfuncs = of_get_child_count(np);
	if (nfuncs <= 0) {
		dev_err(&pdev->dev, "no functions defined\n");
		return -EINVAL;
	}

	ipctl->nfunctions = nfuncs;
	ipctl->functions = devm_kmalloc_array(&pdev->dev, nfuncs,
					      sizeof(*ipctl->functions),
					      GFP_KERNEL);
	if (!ipctl->functions)
		return -ENOMEM;

	ipctl->ngroups = 0;
	ipctl->maxgroups = 0;
	for_each_child_of_node(np, child)
		ipctl->maxgroups += rzn1_pinctrl_count_function_groups(child);

	ipctl->groups = devm_kmalloc_array(&pdev->dev,
					   ipctl->maxgroups,
					   sizeof(*ipctl->groups),
					   GFP_KERNEL);
	if (!ipctl->groups)
		return -ENOMEM;

	for_each_child_of_node(np, child) {
		ret = rzn1_pinctrl_parse_functions(child, ipctl, i++);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int rzn1_pinctrl_probe(struct platform_device *pdev)
{
	struct rzn1_pinctrl *ipctl;
	struct resource *res;
	int ret;

	/* Create state holders etc for this driver */
	ipctl = devm_kzalloc(&pdev->dev, sizeof(*ipctl), GFP_KERNEL);
	if (!ipctl)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ipctl->lev1_protect_phys = (u32)res->start + 0x400;
	ipctl->lev1 = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(ipctl->lev1))
		return PTR_ERR(ipctl->lev1);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	ipctl->lev2_protect_phys = (u32)res->start + 0x400;
	ipctl->lev2 = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(ipctl->lev2))
		return PTR_ERR(ipctl->lev2);

	ipctl->clk = devm_clk_get(&pdev->dev, "bus");
	if (IS_ERR(ipctl->clk))
		return PTR_ERR(ipctl->clk);
	ret = clk_prepare_enable(ipctl->clk);
	if (ret)
		return ret;

	ipctl->dev = &pdev->dev;
	rzn1_pinctrl_desc.name = dev_name(&pdev->dev);
	rzn1_pinctrl_desc.pins = rzn1_pins;
	rzn1_pinctrl_desc.npins = ARRAY_SIZE(rzn1_pins);

	ret = rzn1_pinctrl_probe_dt(pdev, ipctl);
	if (ret) {
		dev_err(&pdev->dev, "fail to probe dt properties\n");
		goto err_clk;
	}

	platform_set_drvdata(pdev, ipctl);
	ipctl->pctl = pinctrl_register(&rzn1_pinctrl_desc, &pdev->dev, ipctl);
	if (!ipctl->pctl) {
		dev_err(&pdev->dev, "could not register rzn1 pinctrl driver\n");
		ret = -EINVAL;
		goto err_clk;
	}

	dev_info(&pdev->dev, "probed\n");

	return 0;

err_clk:
	clk_disable_unprepare(ipctl->clk);

	return ret;
}

static int rzn1_pinctrl_remove(struct platform_device *pdev)
{
	struct rzn1_pinctrl *ipctl = platform_get_drvdata(pdev);

	clk_disable_unprepare(ipctl->clk);

	return 0;
}

static const struct of_device_id rzn1_pinctrl_match[] = {
	{ .compatible = "renesas,rzn1-pinctrl", },
	{}
};
MODULE_DEVICE_TABLE(of, rzn1_pinctrl_match);

static struct platform_driver rzn1_pinctrl_driver = {
	.probe	= rzn1_pinctrl_probe,
	.remove = rzn1_pinctrl_remove,
	.driver	= {
		.name		= "rzn1-pinctrl",
		.owner		= THIS_MODULE,
		.of_match_table	= rzn1_pinctrl_match,
	},
};

static int __init _pinctrl_drv_register(void)
{
	return platform_driver_register(&rzn1_pinctrl_driver);
}
subsys_initcall(_pinctrl_drv_register);

MODULE_AUTHOR("Phil Edworthy <phil.edworthy@renesas.com>");
MODULE_DESCRIPTION("Renesas RZ/N1 pinctrl driver");
MODULE_LICENSE("GPL v2");
