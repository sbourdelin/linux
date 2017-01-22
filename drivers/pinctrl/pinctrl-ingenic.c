/*
 * Ingenic SoCs pinctrl driver
 *
 * Copyright (c) 2017 Paul Cercueil <paul@crapouillou.net>
 *
 * License terms: GNU General Public License (GPL) version 2
 */

#include <linux/compiler.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "core.h"
#include "pinconf.h"
#include "pinmux.h"

#define JZ4740_GPIO_DATA	0x10
#define JZ4740_GPIO_PULL_DIS	0x30
#define JZ4740_GPIO_FUNC	0x40
#define JZ4740_GPIO_SELECT	0x50
#define JZ4740_GPIO_DIR		0x60
#define JZ4740_GPIO_TRIG	0x70
#define JZ4740_GPIO_FLAG	0x80

#define JZ4780_GPIO_INT		0x10
#define JZ4780_GPIO_MSK		0x20
#define JZ4780_GPIO_PAT1	0x30
#define JZ4780_GPIO_PAT0	0x40
#define JZ4780_GPIO_FLAG	0x50
#define JZ4780_GPIO_PEN		0x70

#define REG_SET(x) ((x) + 0x4)
#define REG_CLEAR(x) ((x) + 0x8)

#define PINS_PER_GPIO_CHIP 32
#define NUM_MAX_GPIO_CHIPS 6

enum jz_version {
	ID_JZ4740,
	ID_JZ4780,
};

struct ingenic_pinctrl {
	struct device *dev;
	void __iomem *base;
	struct pinctrl_dev *pctl;
	struct pinctrl_pin_desc *pdesc;
	enum jz_version version;

	u32 pull_ups[NUM_MAX_GPIO_CHIPS];
	u32 pull_downs[NUM_MAX_GPIO_CHIPS];
};

static inline void ingenic_config_pin(struct ingenic_pinctrl *jzpc,
		unsigned int pin, u8 reg, bool set)
{
	unsigned int idx = pin % PINS_PER_GPIO_CHIP;
	unsigned int offt = pin / PINS_PER_GPIO_CHIP;

	writel(BIT(idx), jzpc->base + offt * 0x100 +
			(set ? REG_SET(reg) : REG_CLEAR(reg)));
}

static inline bool ingenic_get_pin_config(struct ingenic_pinctrl *jzpc,
		unsigned int pin, u8 reg)
{
	unsigned int idx = pin % PINS_PER_GPIO_CHIP;
	unsigned int offt = pin / PINS_PER_GPIO_CHIP;

	return readl(jzpc->base + offt * 0x100 + reg) & BIT(idx);
}

static struct pinctrl_ops ingenic_pctlops = {
	.get_groups_count = pinctrl_generic_get_group_count,
	.get_group_name = pinctrl_generic_get_group_name,
	.get_group_pins = pinctrl_generic_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_all,
	.dt_free_map = pinconf_generic_dt_free_map,
};

static int ingenic_pinmux_set_pin_fn(struct ingenic_pinctrl *jzpc,
		int pin, int func)
{
	unsigned int idx = pin % PINS_PER_GPIO_CHIP;
	unsigned int offt = pin / PINS_PER_GPIO_CHIP;

	dev_dbg(jzpc->dev, "set pin P%c%u to function %u\n",
			'A' + offt, idx, func);

	if (jzpc->version >= ID_JZ4780) {
		ingenic_config_pin(jzpc, pin, JZ4780_GPIO_INT, false);
		ingenic_config_pin(jzpc, pin, JZ4780_GPIO_MSK, false);
		ingenic_config_pin(jzpc, pin, JZ4780_GPIO_PAT1, func & 0x2);
		ingenic_config_pin(jzpc, pin, JZ4780_GPIO_PAT0, func & 0x1);
	} else {
		ingenic_config_pin(jzpc, pin, JZ4740_GPIO_FUNC, true);
		ingenic_config_pin(jzpc, pin, JZ4740_GPIO_TRIG, func & 0x2);
		ingenic_config_pin(jzpc, pin, JZ4740_GPIO_SELECT, func > 0);
	}

	return 0;
}

static int ingenic_pinmux_set_mux(struct pinctrl_dev *pctldev,
		unsigned int selector, unsigned int group)
{
	struct ingenic_pinctrl *jzpc = pinctrl_dev_get_drvdata(pctldev);
	struct function_desc *func;
	struct group_desc *grp;
	unsigned int i;

	func = pinmux_generic_get_function(pctldev, selector);
	if (!func)
		return -EINVAL;

	grp = pinctrl_generic_get_group(pctldev, group);
	if (!grp)
		return -EINVAL;

	dev_dbg(pctldev->dev, "enable function %s group %s\n",
		func->name, grp->name);

	for (i = 0; i < grp->num_pins; i++) {
		int *pin_modes = grp->data;

		ingenic_pinmux_set_pin_fn(jzpc, grp->pins[i], pin_modes[i]);
	}

	return 0;
}

static int ingenic_pinmux_gpio_set_direction(struct pinctrl_dev *pctldev,
		struct pinctrl_gpio_range *range,
		unsigned int pin, bool input)
{
	struct ingenic_pinctrl *jzpc = pinctrl_dev_get_drvdata(pctldev);
	unsigned int idx = pin % PINS_PER_GPIO_CHIP;
	unsigned int offt = pin / PINS_PER_GPIO_CHIP;

	dev_dbg(pctldev->dev, "set pin P%c%u to %sput\n",
			'A' + offt, idx, input ? "in" : "out");

	if (jzpc->version >= ID_JZ4780) {
		ingenic_config_pin(jzpc, pin, JZ4780_GPIO_INT, false);
		ingenic_config_pin(jzpc, pin, JZ4780_GPIO_MSK, true);
		ingenic_config_pin(jzpc, pin, JZ4780_GPIO_PAT1, input);
	} else {
		ingenic_config_pin(jzpc, pin, JZ4740_GPIO_SELECT, false);
		ingenic_config_pin(jzpc, pin, JZ4740_GPIO_DIR, input);
		ingenic_config_pin(jzpc, pin, JZ4740_GPIO_FUNC, false);
	}

	return 0;
}

static struct pinmux_ops ingenic_pmxops = {
	.get_functions_count = pinmux_generic_get_function_count,
	.get_function_name = pinmux_generic_get_function_name,
	.get_function_groups = pinmux_generic_get_function_groups,
	.set_mux = ingenic_pinmux_set_mux,
	.gpio_set_direction = ingenic_pinmux_gpio_set_direction,
};

static int ingenic_pinconf_get(struct pinctrl_dev *pctldev,
		unsigned int pin, unsigned long *config)
{
	struct ingenic_pinctrl *jzpc = pinctrl_dev_get_drvdata(pctldev);
	enum pin_config_param param = pinconf_to_config_param(*config);
	unsigned int idx = pin % PINS_PER_GPIO_CHIP;
	unsigned int offt = pin / PINS_PER_GPIO_CHIP;
	bool pull;

	if (jzpc->version >= ID_JZ4780)
		pull = !ingenic_get_pin_config(jzpc, pin, JZ4780_GPIO_PEN);
	else
		pull = !ingenic_get_pin_config(jzpc, pin, JZ4740_GPIO_PULL_DIS);

	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		if (pull)
			return -EINVAL;
		break;

	case PIN_CONFIG_BIAS_PULL_UP:
		if (!pull || !(jzpc->pull_ups[offt] & BIT(idx)))
			return -EINVAL;
		break;

	case PIN_CONFIG_BIAS_PULL_DOWN:
		if (!pull || !(jzpc->pull_downs[offt] & BIT(idx)))
			return -EINVAL;
		break;

	default:
		return -ENOTSUPP;
	}

	*config = pinconf_to_config_packed(param, 1);
	return 0;
}

static void ingenic_set_bias(struct ingenic_pinctrl *jzpc,
		unsigned int pin, bool enabled)
{
	if (jzpc->version >= ID_JZ4780)
		ingenic_config_pin(jzpc, pin, JZ4780_GPIO_PEN, !enabled);
	else
		ingenic_config_pin(jzpc, pin, JZ4740_GPIO_PULL_DIS, !enabled);
}

static int ingenic_pinconf_set(struct pinctrl_dev *pctldev, unsigned int pin,
		unsigned long *configs, unsigned int num_configs)
{
	struct ingenic_pinctrl *jzpc = pinctrl_dev_get_drvdata(pctldev);
	unsigned int idx = pin % PINS_PER_GPIO_CHIP;
	unsigned int offt = pin / PINS_PER_GPIO_CHIP;
	unsigned int cfg;

	for (cfg = 0; cfg < num_configs; cfg++) {
		switch (pinconf_to_config_param(configs[cfg])) {
		case PIN_CONFIG_BIAS_DISABLE:
		case PIN_CONFIG_BIAS_PULL_UP:
		case PIN_CONFIG_BIAS_PULL_DOWN:
			continue;
		default:
			return -ENOTSUPP;
		}
	}

	for (cfg = 0; cfg < num_configs; cfg++) {
		switch (pinconf_to_config_param(configs[cfg])) {
		case PIN_CONFIG_BIAS_DISABLE:
			dev_dbg(jzpc->dev, "disable pull-over for pin P%c%u\n",
					'A' + offt, idx);
			ingenic_set_bias(jzpc, pin, false);
			break;

		case PIN_CONFIG_BIAS_PULL_UP:
			if (!(jzpc->pull_ups[offt] & BIT(idx)))
				return -EINVAL;
			dev_dbg(jzpc->dev, "set pull-up for pin P%c%u\n",
					'A' + offt, idx);
			ingenic_set_bias(jzpc, pin, true);
			break;

		case PIN_CONFIG_BIAS_PULL_DOWN:
			if (!(jzpc->pull_downs[offt] & BIT(idx)))
				return -EINVAL;
			dev_dbg(jzpc->dev, "set pull-down for pin P%c%u\n",
					'A' + offt, idx);
			ingenic_set_bias(jzpc, pin, true);
			break;

		default:
			unreachable();
		}
	}

	return 0;
}

static int ingenic_pinconf_group_get(struct pinctrl_dev *pctldev,
		unsigned int group, unsigned long *config)
{
	const unsigned *pins;
	unsigned int i, npins, old = 0;
	int ret;

	ret = pinctrl_generic_get_group_pins(pctldev, group, &pins, &npins);
	if (ret)
		return ret;

	for (i = 0; i < npins; i++) {
		if (ingenic_pinconf_get(pctldev, pins[i], config))
			return -ENOTSUPP;

		/* configs do not match between two pins */
		if (i && (old != *config))
			return -ENOTSUPP;

		old = *config;
	}

	return 0;
}

static int ingenic_pinconf_group_set(struct pinctrl_dev *pctldev,
		unsigned int group, unsigned long *configs,
		unsigned int num_configs)
{
	const unsigned *pins;
	unsigned int i, npins;
	int ret;

	ret = pinctrl_generic_get_group_pins(pctldev, group, &pins, &npins);
	if (ret)
		return ret;

	for (i = 0; i < npins; i++) {
		ret = ingenic_pinconf_set(pctldev,
				pins[i], configs, num_configs);
		if (ret)
			return ret;
	}

	return 0;
}

static struct pinconf_ops ingenic_confops = {
	.is_generic = true,
	.pin_config_get = ingenic_pinconf_get,
	.pin_config_set = ingenic_pinconf_set,
	.pin_config_group_get = ingenic_pinconf_group_get,
	.pin_config_group_set = ingenic_pinconf_group_set,
};

static int ingenic_pinctrl_parse_dt_func(struct ingenic_pinctrl *jzpc,
		struct device_node *np)
{
	unsigned int num_groups;
	struct device_node *group_node;
	unsigned int i, j;
	int err, npins, *pins, *confs;
	const char **groups;

	num_groups = of_get_child_count(np);
	groups = devm_kzalloc(jzpc->dev,
			sizeof(*groups) * num_groups, GFP_KERNEL);
	if (!groups)
		return -ENOMEM;

	i = 0;
	for_each_child_of_node(np, group_node) {
		groups[i++] = group_node->name;

		npins = of_property_count_elems_of_size(group_node,
				"ingenic,pins", 8);
		if (npins < 0)
			return npins;

		pins = devm_kzalloc(jzpc->dev,
				sizeof(*pins) * npins, GFP_KERNEL);
		confs = devm_kzalloc(jzpc->dev,
				sizeof(*confs) * npins, GFP_KERNEL);
		if (!pins || !confs)
			return -ENOMEM;

		for (j = 0; j < npins; j++) {
			of_property_read_u32_index(group_node,
					"ingenic,pins", j * 2, &pins[j]);

			of_property_read_u32_index(group_node,
					"ingenic,pins", j * 2 + 1, &confs[j]);
		}

		err = pinctrl_generic_add_group(jzpc->pctl, group_node->name,
				pins, npins, confs);
		if (err)
			return err;
	}

	return pinmux_generic_add_function(jzpc->pctl, np->name,
			groups, num_groups, NULL);
}

static const struct of_device_id ingenic_pinctrl_of_match[] = {
	{ .compatible = "ingenic,jz4740-pinctrl", .data = (void *) ID_JZ4740 },
	{ .compatible = "ingenic,jz4780-pinctrl", .data = (void *) ID_JZ4780 },
	{},
};

int ingenic_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ingenic_pinctrl *jzpc;
	struct pinctrl_desc *pctl_desc;
	struct device_node *np, *functions_node;
	const struct of_device_id *of_id = of_match_device(
			ingenic_pinctrl_of_match, dev);
	unsigned int i, num_chips;
	int err;

	jzpc = devm_kzalloc(dev, sizeof(*jzpc), GFP_KERNEL);
	if (!jzpc)
		return -ENOMEM;

	jzpc->base = of_iomap(dev->of_node, 0);
	if (!jzpc->base) {
		dev_err(dev, "failed to map IO memory\n");
		return -ENXIO;
	}

	jzpc->dev = dev;
	dev_set_drvdata(dev, jzpc);

	jzpc->version = (enum jz_version)of_id->data;

	if (jzpc->version >= ID_JZ4780)
		num_chips = 6;
	else
		num_chips = 4;

	/*
	 * Read the ingenic,pull-ups and ingenic,pull-downs arrays if present in
	 * the devicetree. Otherwise set all bits to 0xff to consider that
	 * pull-over resistors are available on all pins.
	 */
	err = of_property_read_u32_array(dev->of_node, "ingenic,pull-ups",
			jzpc->pull_ups, num_chips);
	if (err)
		memset(jzpc->pull_ups, 0xff, sizeof(jzpc->pull_ups));

	err = of_property_read_u32_array(dev->of_node, "ingenic,pull-downs",
			jzpc->pull_downs, num_chips);
	if (err)
		memset(jzpc->pull_downs, 0xff, sizeof(jzpc->pull_downs));

	functions_node = of_find_node_by_name(dev->of_node, "functions");
	if (!functions_node) {
		dev_err(dev, "Missing \"functions\" devicetree node\n");
		return -EINVAL;
	}

	pctl_desc = devm_kzalloc(&pdev->dev, sizeof(*pctl_desc), GFP_KERNEL);
	if (!pctl_desc)
		return -ENOMEM;

	/* fill in pinctrl_desc structure */
	pctl_desc->name = dev_name(dev);
	pctl_desc->owner = THIS_MODULE;
	pctl_desc->pctlops = &ingenic_pctlops;
	pctl_desc->pmxops = &ingenic_pmxops;
	pctl_desc->confops = &ingenic_confops;
	pctl_desc->npins = num_chips * PINS_PER_GPIO_CHIP;
	pctl_desc->pins = jzpc->pdesc = devm_kzalloc(&pdev->dev,
			sizeof(*jzpc->pdesc) * pctl_desc->npins, GFP_KERNEL);
	if (!jzpc->pdesc)
		return -ENOMEM;

	for (i = 0; i < pctl_desc->npins; i++) {
		jzpc->pdesc[i].number = i;
		jzpc->pdesc[i].name = kasprintf(GFP_KERNEL, "P%c%d",
						'A' + (i / PINS_PER_GPIO_CHIP),
						i % PINS_PER_GPIO_CHIP);
	}

	jzpc->pctl = devm_pinctrl_register(dev, pctl_desc, jzpc);
	if (!jzpc->pctl) {
		dev_err(dev, "Failed pinctrl registration\n");
		return -EINVAL;
	}

	for_each_child_of_node(functions_node, np) {
		err = ingenic_pinctrl_parse_dt_func(jzpc, np);
		if (err) {
			dev_err(dev, "failed to parse function %s\n",
					np->full_name);
			continue;
		}
	}

	return 0;
}

static struct platform_driver ingenic_pinctrl_driver = {
	.driver = {
		.name = "pinctrl-ingenic",
		.of_match_table = of_match_ptr(ingenic_pinctrl_of_match),
		.suppress_bind_attrs = true,
	},
	.probe = ingenic_pinctrl_probe,
};

static int __init ingenic_pinctrl_drv_register(void)
{
	return platform_driver_register(&ingenic_pinctrl_driver);
}
postcore_initcall(ingenic_pinctrl_drv_register);
