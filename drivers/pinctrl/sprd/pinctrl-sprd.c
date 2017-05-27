/*
 * Spreadtrum pin controller driver
 * Copyright (C) 2017 Spreadtrum  - http://www.spreadtrum.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/slab.h>

#include "../core.h"
#include "../pinmux.h"
#include "pinctrl-sprd.h"

#define PINCTRL_BIT_MASK(width)		(~(~0UL << (width)))
#define PINCTRL_REG_OFFSET		0x20
#define PINCTRL_REG_MISC_OFFSET		0x4020
#define SPRD_PIN_SIZE			8

/**
 * struct sprd_pin: represent one pin's description
 * @name: pin name
 * @number: pin number
 * @type: pin type, can be GLOBAL_CTRL_PIN/COMMON_PIN/MISC_PIN
 * @reg: pin register address
 * @bit_offset: bit offset in pin register
 * @bit_width: bit width in pin register
 * @config: pin configuration values
 */
struct sprd_pin {
	const char *name;
	unsigned int number;
	enum pin_type type;
	unsigned long reg;
	unsigned long bit_offset;
	unsigned long bit_width;
	unsigned long config;
};

/**
 * struct sprd_pin_group: represent one group's description
 * @name: group name
 * @npins: pin numbers of this group
 * @pins: pointer to pins array
 * @configs: pointer to pins configuration array
 */
struct sprd_pin_group {
	const char *name;
	unsigned int npins;
	unsigned int *pins;
	unsigned long *configs;
};

/**
 * struct sprd_pinctrl_soc_info: represent the SoC's pins description
 * @groups: pointer to groups of pins
 * @ngroups: group numbers of the whole SoC
 * @pins: pointer to pins description
 * @npins: pin numbers of the whole SoC
 */
struct sprd_pinctrl_soc_info {
	struct sprd_pin_group *groups;
	unsigned int ngroups;
	struct sprd_pin *pins;
	unsigned int npins;
};

/**
 * struct sprd_pinctrl: represent the pin controller device
 * @dev: pointer to the device structure
 * @pctl: pointer to the pinctrl handle
 * @base: base address of the controller
 * @info: pointer to SoC's pins description information
 */
struct sprd_pinctrl {
	struct device *dev;
	struct pinctrl_dev *pctl;
	void __iomem *base;
	struct sprd_pinctrl_soc_info *info;
};

static struct sprd_pin *
sprd_pinctrl_get_pin_by_id(struct sprd_pinctrl *sprd_pctl, unsigned int id)
{
	struct sprd_pinctrl_soc_info *info = sprd_pctl->info;
	struct sprd_pin *pin = NULL;
	int i;

	for (i = 0; i < info->npins; i++) {
		if (info->pins[i].number == id) {
			pin = &info->pins[i];
			break;
		}
	}

	return pin;
}

static const struct sprd_pin_group *
sprd_pinctrl_find_group_by_name(struct sprd_pinctrl *sprd_pctl,
				const char *name)
{
	struct sprd_pinctrl_soc_info *info = sprd_pctl->info;
	const struct sprd_pin_group *grp = NULL;
	int i;

	for (i = 0; i < info->ngroups; i++) {
		if (!strcmp(info->groups[i].name, name)) {
			grp = &info->groups[i];
			break;
		}
	}

	return grp;
}

static int sprd_pctrl_group_count(struct pinctrl_dev *pctldev)
{
	struct sprd_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct sprd_pinctrl_soc_info *info = pctl->info;

	return info->ngroups;
}

static const char *sprd_pctrl_group_name(struct pinctrl_dev *pctldev,
					 unsigned int selector)
{
	struct sprd_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct sprd_pinctrl_soc_info *info = pctl->info;

	return info->groups[selector].name;
}

static int sprd_pctrl_group_pins(struct pinctrl_dev *pctldev,
				 unsigned int selector,
				 const unsigned int **pins,
				 unsigned int *npins)
{
	struct sprd_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct sprd_pinctrl_soc_info *info = pctl->info;

	if (selector >= info->ngroups)
		return -EINVAL;

	*pins = info->groups[selector].pins;
	*npins = info->groups[selector].npins;

	return 0;
}

static int sprd_dt_node_to_map(struct pinctrl_dev *pctldev,
			       struct device_node *np,
			       struct pinctrl_map **map,
			       unsigned int *num_maps)
{
	struct sprd_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	const struct sprd_pin_group *grp;
	struct pinctrl_map *new_map;
	struct device_node *parent;
	int map_num;

	grp = sprd_pinctrl_find_group_by_name(pctl, np->name);
	if (!grp) {
		pr_err("unable to find group for node %s\n", np->name);
		return -EINVAL;
	}

	map_num = 1;
	new_map = kmalloc(sizeof(struct pinctrl_map) * map_num, GFP_KERNEL);
	if (!new_map)
		return -ENOMEM;

	*map = new_map;
	*num_maps = map_num;

	parent = of_get_parent(np);
	if (!parent) {
		kfree(new_map);
		return -EINVAL;
	}

	/* create mux map */
	if (parent != pctl->dev->of_node) {
		new_map[0].type = PIN_MAP_TYPE_MUX_GROUP;
		new_map[0].data.mux.function = parent->name;
		new_map[0].data.mux.group = np->name;

		of_node_put(parent);
		return 0;
	}

	of_node_put(parent);

	if (grp->npins == 1) {
		unsigned int pin_id = grp->pins[0];

		/* create config map for one pin */
		new_map->type = PIN_MAP_TYPE_CONFIGS_PIN;
		new_map->data.configs.group_or_pin =
			pin_get_name(pctldev, pin_id);
		new_map->data.configs.configs = grp->configs;
		new_map->data.configs.num_configs = 1;
	} else {
		/* create config map for group */
		new_map->type = PIN_MAP_TYPE_CONFIGS_GROUP;
		new_map->data.configs.group_or_pin = grp->name;
		new_map->data.configs.num_configs = grp->npins;
		new_map->data.configs.configs = grp->configs;
	}

	return 0;
}

static void sprd_pctrl_dbg_show(struct pinctrl_dev *pctldev, struct seq_file *s,
				unsigned int offset)
{
	seq_printf(s, "%s", dev_name(pctldev->dev));
}

static void sprd_dt_free_map(struct pinctrl_dev *pctldev,
			     struct pinctrl_map *map, unsigned int num_maps)
{
	kfree(map);
}

static const struct pinctrl_ops sprd_pctrl_ops = {
	.get_groups_count = sprd_pctrl_group_count,
	.get_group_name = sprd_pctrl_group_name,
	.get_group_pins = sprd_pctrl_group_pins,
	.pin_dbg_show = sprd_pctrl_dbg_show,
	.dt_node_to_map = sprd_dt_node_to_map,
	.dt_free_map = sprd_dt_free_map,
};

static int sprd_pmx_set_mux(struct pinctrl_dev *pctldev,
			    unsigned int func_selector,
			    unsigned int group_selector)
{
	struct sprd_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct sprd_pinctrl_soc_info *info = pctl->info;
	struct sprd_pin_group *grp = &info->groups[group_selector];
	unsigned int grp_pins = grp->npins;
	struct function_desc *func;
	unsigned long reg;
	int i;

	if (group_selector > info->ngroups)
		return -EINVAL;

	func = pinmux_generic_get_function(pctldev, func_selector);
	if (!func)
		return -EINVAL;

	for (i = 0; i < grp_pins; i++) {
		unsigned int pin_id = grp->pins[i];
		struct sprd_pin *pin = sprd_pinctrl_get_pin_by_id(pctl, pin_id);

		if (pin->type == GLOBAL_CTRL_PIN) {
			reg = readl((void __iomem *)pin->reg);
			reg |= (grp->configs[i] &
				PINCTRL_BIT_MASK(pin->bit_width))
				<< pin->bit_offset;
			writel(reg, (void __iomem *)pin->reg);
		} else {
			writel(grp->configs[i], (void __iomem *)pin->reg);
		}

		pin->config = grp->configs[i];
	}

	return 0;
}

static const struct pinmux_ops sprd_pmx_ops = {
	.get_functions_count = pinmux_generic_get_function_count,
	.get_function_name = pinmux_generic_get_function_name,
	.get_function_groups = pinmux_generic_get_function_groups,
	.set_mux = sprd_pmx_set_mux,
};

static int sprd_pinconf_get(struct pinctrl_dev *pctldev, unsigned int pin_id,
			    unsigned long *config)
{
	struct sprd_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct sprd_pin *pin = sprd_pinctrl_get_pin_by_id(pctl, pin_id);

	if (!pin)
		return -EINVAL;

	if (pin->type == GLOBAL_CTRL_PIN) {
		*config = (readl((void __iomem *)pin->reg) >>
			   pin->bit_offset) & PINCTRL_BIT_MASK(pin->bit_width);
	} else {
		*config = readl((void __iomem *)pin->reg);
	}

	return 0;
}

static int sprd_pinconf_set(struct pinctrl_dev *pctldev, unsigned int pin_id,
			    unsigned long *configs, unsigned int num_configs)
{
	struct sprd_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct sprd_pin *pin = sprd_pinctrl_get_pin_by_id(pctl, pin_id);
	unsigned long reg;
	int i;

	if (!pin)
		return -EINVAL;

	for (i = 0; i < num_configs; i++) {
		if (pin->type == GLOBAL_CTRL_PIN) {
			reg = readl((void __iomem *)pin->reg);
			reg &= ~(PINCTRL_BIT_MASK(pin->bit_width)
				<< pin->bit_offset);
			reg |= (configs[i] & PINCTRL_BIT_MASK(pin->bit_width))
				<< pin->bit_offset;
			writel(reg, (void __iomem *)pin->reg);
		} else {
			writel(configs[i], (void __iomem *)pin->reg);
		}
		pin->config = configs[i];
	}

	return 0;
}

static int sprd_pinconf_group_get(struct pinctrl_dev *pctldev,
				  unsigned int selector, unsigned long *config)
{
	struct sprd_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct sprd_pinctrl_soc_info *info = pctl->info;
	struct sprd_pin_group *grp;
	int ret, i;

	if (selector > info->ngroups)
		return -EINVAL;

	grp = &info->groups[selector];

	for (i = 0; i < grp->npins; i++, config++) {
		unsigned int pin_id = grp->pins[i];

		ret = sprd_pinconf_get(pctldev, pin_id, config);
		if (ret)
			return ret;
	}

	return 0;
}

static int sprd_pinconf_group_set(struct pinctrl_dev *pctldev,
				  unsigned int selector,
				  unsigned long *configs,
				  unsigned int num_configs)
{
	struct sprd_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct sprd_pinctrl_soc_info *info = pctl->info;
	struct sprd_pin_group *grp;
	int ret, i;

	if (selector > info->ngroups)
		return -EINVAL;

	grp = &info->groups[selector];
	if (grp->npins != num_configs)
		return -EINVAL;

	for (i = 0; i < grp->npins; i++, configs++) {
		unsigned int pin_id = grp->pins[i];

		ret = sprd_pinconf_set(pctldev, pin_id, configs, 1);
		if (ret)
			return ret;
	}

	return 0;
}

static void sprd_pinconf_dbg_show(struct pinctrl_dev *pctldev,
				  struct seq_file *s, unsigned int pin_id)
{
	unsigned long config;
	int ret;

	ret = sprd_pinconf_get(pctldev, pin_id, &config);
	if (ret)
		return;

	seq_printf(s, "0x%lx", config);
}

static void sprd_pinconf_group_dbg_show(struct pinctrl_dev *pctldev,
					struct seq_file *s,
					unsigned int selector)
{
	struct sprd_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct sprd_pinctrl_soc_info *info = pctl->info;
	struct sprd_pin_group *grp;
	unsigned long config;
	const char *name;
	int i, ret;

	if (selector > info->ngroups)
		return;

	grp = &info->groups[selector];

	seq_printf(s, "\n");
	for (i = 0; i < grp->npins; i++, config++) {
		unsigned int pin_id = grp->pins[i];

		name = pin_get_name(pctldev, pin_id);
		ret = sprd_pinconf_get(pctldev, pin_id, &config);
		if (ret)
			return;

		seq_printf(s, "%s: 0x%lx", name, config);
	}
}

static const struct pinconf_ops sprd_pinconf_ops = {
	.pin_config_get = sprd_pinconf_get,
	.pin_config_set = sprd_pinconf_set,
	.pin_config_group_get = sprd_pinconf_group_get,
	.pin_config_group_set = sprd_pinconf_group_set,
	.pin_config_dbg_show = sprd_pinconf_dbg_show,
	.pin_config_group_dbg_show = sprd_pinconf_group_dbg_show,
};

static struct pinctrl_desc sprd_pinctrl_desc = {
	.pctlops = &sprd_pctrl_ops,
	.pmxops = &sprd_pmx_ops,
	.confops = &sprd_pinconf_ops,
	.owner = THIS_MODULE,
};

static int sprd_pinctrl_parse_groups(struct device_node *np,
				     struct sprd_pinctrl *sprd_pctl,
				     struct sprd_pin_group *grp)
{
	int i, size, pin_cnt;
	const __be32 *list;

	list = of_get_property(np, "sprd,pins", &size);
	if (!size || !list) {
		dev_err(sprd_pctl->dev, "no pins property in node %s\n",
			np->full_name);
		return -EINVAL;
	}

	pin_cnt = size / SPRD_PIN_SIZE;
	grp->name = np->name;
	grp->npins = pin_cnt;
	grp->pins = devm_kzalloc(sprd_pctl->dev, grp->npins *
				 sizeof(unsigned int), GFP_KERNEL);
	if (!grp->pins)
		return -ENOMEM;

	grp->configs = devm_kzalloc(sprd_pctl->dev, grp->npins *
				    sizeof(unsigned long), GFP_KERNEL);
	if (!grp->configs)
		return -ENOMEM;

	for (i = 0; i < grp->npins; i++) {
		grp->pins[i] = be32_to_cpu(*list++);
		grp->configs[i] = be32_to_cpu(*list++);
	}

	for (i = 0; i < grp->npins; i++) {
		dev_dbg(sprd_pctl->dev, "Group[%s] contains [%d] pins: "
			"pin id = %d, pin config = %ld\n",
			grp->name, grp->npins, grp->pins[i],
			grp->configs[i]);
	}

	return 0;
}

static unsigned int sprd_pinctrl_get_groups(struct device_node *np)
{
	struct device_node *child;
	unsigned int group_cnt, cnt;

	group_cnt = of_get_child_count(np);

	for_each_child_of_node(np, child) {
		cnt = of_get_child_count(child);
		if (cnt > 0)
			group_cnt += cnt - 1;
	}

	return group_cnt;
}

static int sprd_pinctrl_parse_dt(struct sprd_pinctrl *sprd_pctl)
{
	struct sprd_pinctrl_soc_info *info = sprd_pctl->info;
	struct device_node *np = sprd_pctl->dev->of_node;
	struct device_node *child, *sub_child;
	struct sprd_pin_group *grp;
	int ret, num_groups;

	if (!np)
		return -ENODEV;

	info->ngroups = sprd_pinctrl_get_groups(np);
	if (!info->ngroups)
		return 0;

	info->groups = devm_kzalloc(sprd_pctl->dev, info->ngroups *
				    sizeof(struct sprd_pin_group),
				    GFP_KERNEL);
	if (!info->groups)
		return -ENOMEM;

	grp = info->groups;
	for_each_child_of_node(np, child) {
		num_groups = of_get_child_count(child);

		if (num_groups == 0) {
			ret = sprd_pinctrl_parse_groups(child, sprd_pctl, grp);
			if (ret)
				return ret;

			grp++;
		} else {
			const char **temp;
			const char **groups = devm_kzalloc(sprd_pctl->dev,
						num_groups * sizeof(char *),
						GFP_KERNEL);
			if (!groups)
				return -ENOMEM;

			temp = groups;
			for_each_child_of_node(child, sub_child) {
				ret = sprd_pinctrl_parse_groups(sub_child,
								sprd_pctl, grp);
				if (ret)
					return ret;

				*temp++ = grp->name;
				grp++;
			}

			ret = pinmux_generic_add_function(sprd_pctl->pctl,
							  child->name,
							  groups,
							  num_groups,
							  sprd_pctl);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int sprd_pinctrl_add_pins(struct sprd_pinctrl *sprd_pctl,
				 struct sprd_pins_info *sprd_soc_pin_info,
				 int pins_cnt)
{
	struct sprd_pinctrl_soc_info *info = sprd_pctl->info;
	unsigned int ctrl_pin = 0, com_pin = 0;
	struct sprd_pin *pin;
	int i;

	info->npins = pins_cnt;
	info->pins = devm_kzalloc(sprd_pctl->dev,
				  info->npins * sizeof(struct sprd_pin),
				  GFP_KERNEL);
	if (!info->pins)
		return -ENOMEM;

	for (i = 0, pin = info->pins; i < info->npins; i++, pin++) {
		unsigned int reg;

		pin->name = sprd_soc_pin_info[i].name;
		pin->type = sprd_soc_pin_info[i].type;
		pin->number = sprd_soc_pin_info[i].num;
		reg = sprd_soc_pin_info[i].reg;
		if (pin->type == GLOBAL_CTRL_PIN) {
			pin->reg = (unsigned long)sprd_pctl->base + 0x4 * reg;
			pin->bit_offset = sprd_soc_pin_info[i].bit_offset;
			pin->bit_width = sprd_soc_pin_info[i].bit_width;
			ctrl_pin++;
		} else if (pin->type == COMMON_PIN) {
			pin->reg = (unsigned long)sprd_pctl->base +
				PINCTRL_REG_OFFSET + 0x4 * (i - ctrl_pin);
			com_pin++;
		} else if (pin->type == MISC_PIN) {
			pin->reg = (unsigned long)sprd_pctl->base +
				PINCTRL_REG_MISC_OFFSET +
					0x4 * (i - ctrl_pin - com_pin);
		}
	}

	for (i = 0, pin = info->pins; i < info->npins; pin++, i++) {
		dev_dbg(sprd_pctl->dev, "pin name[%s-%d], type = %d, "
			"bit offset = %ld, bit width = %ld, reg = 0x%lx\n",
			pin->name, pin->number, pin->type,
			pin->bit_offset, pin->bit_width, pin->reg);
	}

	return 0;
}

int sprd_pinctrl_core_probe(struct platform_device *pdev,
			    struct sprd_pins_info *sprd_soc_pin_info,
			    int pins_cnt)
{
	struct sprd_pinctrl *sprd_pctl;
	struct sprd_pinctrl_soc_info *pinctrl_info;
	struct pinctrl_pin_desc *pin_desc;
	struct resource *res;
	int ret, i;

	sprd_pctl = devm_kzalloc(&pdev->dev, sizeof(struct sprd_pinctrl),
				 GFP_KERNEL);
	if (!sprd_pctl)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	sprd_pctl->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(sprd_pctl->base))
		return PTR_ERR(sprd_pctl->base);

	pinctrl_info = devm_kzalloc(&pdev->dev,
				    sizeof(struct sprd_pinctrl_soc_info),
				    GFP_KERNEL);
	if (!pinctrl_info)
		return -ENOMEM;

	sprd_pctl->info = pinctrl_info;
	sprd_pctl->dev = &pdev->dev;
	platform_set_drvdata(pdev, sprd_pctl);

	ret = sprd_pinctrl_add_pins(sprd_pctl, sprd_soc_pin_info, pins_cnt);
	if (ret) {
		dev_err(&pdev->dev, "fail to add pins information\n");
		return ret;
	}

	pin_desc = devm_kzalloc(&pdev->dev, pinctrl_info->npins *
				sizeof(struct pinctrl_pin_desc),
				GFP_KERNEL);
	if (!pin_desc)
		return -ENOMEM;

	for (i = 0; i < pinctrl_info->npins; i++) {
		pin_desc[i].number = pinctrl_info->pins[i].number;
		pin_desc[i].name = pinctrl_info->pins[i].name;
		pin_desc[i].drv_data = pinctrl_info;
	}

	sprd_pinctrl_desc.pins = pin_desc;
	sprd_pinctrl_desc.name = dev_name(&pdev->dev);
	sprd_pinctrl_desc.npins = pinctrl_info->npins;

	sprd_pctl->pctl = pinctrl_register(&sprd_pinctrl_desc,
					   &pdev->dev, (void *)sprd_pctl);
	if (IS_ERR(sprd_pctl->pctl)) {
		dev_err(&pdev->dev, "could not register pinctrl driver\n");
		return PTR_ERR(sprd_pctl->pctl);
	}

	ret = sprd_pinctrl_parse_dt(sprd_pctl);
	if (ret) {
		dev_err(&pdev->dev, "fail to parse dt properties\n");
		pinctrl_unregister(sprd_pctl->pctl);
		return ret;
	}

	return 0;
}

int sprd_pinctrl_remove(struct platform_device *pdev)
{
	struct sprd_pinctrl *sprd_pctl = platform_get_drvdata(pdev);

	pinctrl_unregister(sprd_pctl->pctl);
	return 0;
}

void sprd_pinctrl_shutdown(struct platform_device *pdev)
{
	struct pinctrl *pinctl = devm_pinctrl_get(&pdev->dev);
	struct pinctrl_state *state;

	state = pinctrl_lookup_state(pinctl, "pins-shutdown");
	if (!IS_ERR(state))
		pinctrl_select_state(pinctl, state);
}

MODULE_DESCRIPTION("SPREADTRUM Pin Controller Driver");
MODULE_AUTHOR("Baolin Wang <baolin.wang@spreadtrum.com>");
MODULE_LICENSE("GPL v2");
