// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pinmux and GPIO controller driver for TI DS90Ux9xx De-/Serializer hardware
 *
 * Copyright (c) 2017-2018 Mentor Graphics Inc.
 */

#include <linux/gpio/driver.h>
#include <linux/mfd/ds90ux9xx.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sysfs.h>

#include "pinctrl-utils.h"

#define SER_REG_PIN_CTRL		0x12
#define PIN_CTRL_RGB18			BIT(2)
#define PIN_CTRL_I2S_DATA_ISLAND	BIT(1)
#define PIN_CTRL_I2S_CHANNEL_B		(BIT(0) | BIT(3))

#define SER_REG_I2S_SURROUND		0x1A
#define PIN_CTRL_I2S_SURR_BIT		BIT(0)

#define DES_REG_INDIRECT_PASS		0x16

#define OUTPUT_HIGH			BIT(3)
#define REMOTE_CONTROL			BIT(2)
#define DIR_INPUT			BIT(1)
#define ENABLE_GPIO			BIT(0)

#define GPIO_AS_INPUT			(ENABLE_GPIO | DIR_INPUT)
#define GPIO_AS_OUTPUT			ENABLE_GPIO
#define GPIO_OUTPUT_HIGH		(GPIO_AS_OUTPUT | OUTPUT_HIGH)
#define GPIO_OUTPUT_LOW			GPIO_AS_OUTPUT
#define GPIO_OUTPUT_REMOTE		(GPIO_AS_OUTPUT | REMOTE_CONTROL)

#define DS90UX9XX_MAX_GROUP_PINS	6

struct ds90ux9xx_gpio {
	const u8 cfg_reg;
	const u8 cfg_mask;
	const u8 stat_reg;
	const u8 stat_bit;
	const bool input;
};

#define DS90UX9XX_PIN_GPIO_SIMPLE(_cfg, _high, _input)		\
	{							\
		.cfg_reg	= _cfg,				\
		.cfg_mask	= _high ? 0xf0 : 0x0f,		\
		.stat_reg	= 0x0,				\
		.stat_bit	= 0x0,				\
		.input		= _input ? true : false,	\
	}

#define DS90UX9XX_PIN_GPIO(_cfg, _high, _stat, _bit)		\
	{							\
		.cfg_reg	= _cfg,				\
		.cfg_mask	= _high ? 0xf0 : 0x0f,		\
		.stat_reg	= _stat,			\
		.stat_bit	= BIT(_bit),			\
		.input		= true,				\
	}

enum ds90ux9xx_function {
	DS90UX9XX_FUNC_NONE,
	DS90UX9XX_FUNC_GPIO,
	DS90UX9XX_FUNC_REMOTE,
	DS90UX9XX_FUNC_PASS,
	DS90UX9XX_FUNC_I2S_1,
	DS90UX9XX_FUNC_I2S_2,
	DS90UX9XX_FUNC_I2S_3,
	DS90UX9XX_FUNC_I2S_4,
	DS90UX9XX_FUNC_I2S_M,
	DS90UX9XX_FUNC_PARALLEL,
};

struct ds90ux9xx_pin {
	const unsigned int id;
	const char *name;
	const u32 func_mask;
	const u8 pass_bit;
	const struct ds90ux9xx_gpio gpio;
};

#define TO_BIT(_f)	(DS90UX9XX_FUNC_##_f ? BIT(DS90UX9XX_FUNC_##_f) : 0)
#define DS90UX9XX_GPIO(_pin, _name, _f1, _f2, _f3)			\
	[_pin] = {							\
		.id = _pin,						\
		.name = _name,						\
		.func_mask = TO_BIT(GPIO) | TO_BIT(_f2) | TO_BIT(_f3),	\
		.gpio = DS90UX9XX_PIN_##_f1,				\
	}

#define DS90UX940_GPIO(_pin, _name, _f1, _f2, _f3, _pass)		\
	[_pin] = {							\
		.id = _pin,						\
		.name = _name,						\
		.func_mask = TO_BIT(GPIO) | TO_BIT(PASS) |		\
			     TO_BIT(_f2) | TO_BIT(_f3),			\
		.pass_bit = BIT(_pass),					\
		.gpio = DS90UX9XX_PIN_##_f1,				\
	}

struct ds90ux9xx_pinctrl_data {
	const char *name;
	const struct ds90ux9xx_pin *pins;
	const struct pinctrl_pin_desc *pins_desc;
	const unsigned int npins;
	const enum ds90ux9xx_function *functions;
	const unsigned int nfunctions;
};

static const struct pinctrl_pin_desc ds90ux925_926_pins_desc[] = {
	PINCTRL_PIN(0, "gpio0"),	/* DS90Ux925 pin 25, DS90Ux926 pin 41 */
	PINCTRL_PIN(1, "gpio1"),	/* DS90Ux925 pin 26, DS90Ux926 pin 40 */
	PINCTRL_PIN(2, "gpio2"),	/* DS90Ux925 pin 35, DS90Ux926 pin 28 */
	PINCTRL_PIN(3, "gpio3"),	/* DS90Ux925 pin 36, DS90Ux926 pin 27 */
	PINCTRL_PIN(4, "gpio4"),	/* DS90Ux925 pin 43, DS90Ux926 pin 19 */
	PINCTRL_PIN(5, "gpio5"),	/* DS90Ux925 pin 44, DS90Ux926 pin 18 */
	PINCTRL_PIN(6, "gpio6"),	/* DS90Ux925 pin 11, DS90Ux926 pin 45 */
	PINCTRL_PIN(7, "gpio7"),	/* DS90Ux925 pin 12, DS90Ux926 pin 30 */
	PINCTRL_PIN(8, "gpio8"),	/* DS90Ux925 pin 13, DS90Ux926 pin  1 */
};

static const struct pinctrl_pin_desc ds90ux927_928_pins_desc[] = {
	PINCTRL_PIN(0, "gpio0"),	/* DS90Ux927 pin 39, DS90Ux928 pin 14 */
	PINCTRL_PIN(1, "gpio1"),	/* DS90Ux927 pin 40, DS90Ux928 pin 13 */
	PINCTRL_PIN(2, "gpio2"),	/* DS90Ux927 pin  5, DS90Ux928 pin 37 */
	PINCTRL_PIN(3, "gpio3"),	/* DS90Ux927 pin  6, DS90Ux928 pin 36 */
	PINCTRL_PIN(4, "gpio5"),	/* DS90Ux927 pin  4, DS90Ux928 pin  3 */
	PINCTRL_PIN(5, "gpio6"),	/* DS90Ux927 pin  3, DS90Ux928 pin  7 */
	PINCTRL_PIN(6, "gpio7"),	/* DS90Ux927 pin  2, DS90Ux928 pin 10 */
	PINCTRL_PIN(7, "gpio8"),	/* DS90Ux927 pin  1, DS90Ux928 pin  8 */
};

static const struct pinctrl_pin_desc ds90ux940_pins_desc[] = {
	PINCTRL_PIN(0, "gpio0"),	/* DS90Ux940 pin  7 */
	PINCTRL_PIN(1, "gpio1"),	/* DS90Ux940 pin  8 */
	PINCTRL_PIN(2, "gpio2"),	/* DS90Ux940 pin 10 */
	PINCTRL_PIN(3, "gpio3"),	/* DS90Ux940 pin  9 */
	PINCTRL_PIN(4, "gpio5"),	/* DS90Ux940 pin 11 */
	PINCTRL_PIN(5, "gpio6"),	/* DS90Ux940 pin 12 */
	PINCTRL_PIN(6, "gpio7"),	/* DS90Ux940 pin 14 */
	PINCTRL_PIN(7, "gpio8"),	/* DS90Ux940 pin 13 */
	PINCTRL_PIN(8, "gpio9"),	/* DS90Ux940 pin 15 */
};

static const struct ds90ux9xx_pin ds90ux925_pins[] = {
	DS90UX9XX_GPIO(0, "gpio0", GPIO_SIMPLE(0x0d, 0, 1), REMOTE, PARALLEL),
	DS90UX9XX_GPIO(1, "gpio1", GPIO_SIMPLE(0x0e, 0, 1), REMOTE, PARALLEL),
	DS90UX9XX_GPIO(2, "gpio2", GPIO_SIMPLE(0x0e, 1, 1), REMOTE, PARALLEL),
	DS90UX9XX_GPIO(3, "gpio3", GPIO_SIMPLE(0x0f, 0, 1), REMOTE, PARALLEL),
	DS90UX9XX_GPIO(4, "gpio4", GPIO_SIMPLE(0x0f, 1, 0),   NONE, PARALLEL),
	DS90UX9XX_GPIO(5, "gpio5", GPIO_SIMPLE(0x10, 0, 0),  I2S_2, PARALLEL),
	DS90UX9XX_GPIO(6, "gpio6", GPIO_SIMPLE(0x10, 1, 0),  I2S_1,     NONE),
	DS90UX9XX_GPIO(7, "gpio7", GPIO_SIMPLE(0x11, 0, 0),  I2S_1,     NONE),
	DS90UX9XX_GPIO(8, "gpio8", GPIO_SIMPLE(0x11, 1, 0),  I2S_1,     NONE),
};

static const struct ds90ux9xx_pin ds90ux926_pins[] = {
	DS90UX9XX_GPIO(0, "gpio0", GPIO_SIMPLE(0x1d, 0, 1), REMOTE, PARALLEL),
	DS90UX9XX_GPIO(1, "gpio1", GPIO_SIMPLE(0x1e, 0, 1), REMOTE, PARALLEL),
	DS90UX9XX_GPIO(2, "gpio2", GPIO_SIMPLE(0x1e, 1, 1), REMOTE, PARALLEL),
	DS90UX9XX_GPIO(3, "gpio3", GPIO_SIMPLE(0x1f, 0, 1), REMOTE, PARALLEL),
	DS90UX9XX_GPIO(4, "gpio4", GPIO_SIMPLE(0x1f, 1, 0),   NONE, PARALLEL),
	DS90UX9XX_GPIO(5, "gpio5", GPIO_SIMPLE(0x20, 0, 0),  I2S_2, PARALLEL),
	DS90UX9XX_GPIO(6, "gpio6", GPIO_SIMPLE(0x20, 1, 0),  I2S_1,     NONE),
	DS90UX9XX_GPIO(7, "gpio7", GPIO_SIMPLE(0x21, 0, 0),  I2S_1,     NONE),
	DS90UX9XX_GPIO(8, "gpio8", GPIO_SIMPLE(0x21, 1, 0),  I2S_1,     NONE),
};

static const struct ds90ux9xx_pin ds90ux927_pins[] = {
	DS90UX9XX_GPIO(0, "gpio0", GPIO(0x0d, 0, 0x1c, 0), REMOTE,  NONE),
	DS90UX9XX_GPIO(1, "gpio1", GPIO(0x0e, 0, 0x1c, 1), REMOTE,  NONE),
	DS90UX9XX_GPIO(2, "gpio2", GPIO(0x0e, 1, 0x1c, 2), REMOTE, I2S_3),
	DS90UX9XX_GPIO(3, "gpio3", GPIO(0x0f, 0, 0x1c, 3), REMOTE, I2S_4),
	DS90UX9XX_GPIO(4, "gpio5", GPIO(0x10, 0, 0x1c, 5),   NONE, I2S_2),
	DS90UX9XX_GPIO(5, "gpio6", GPIO(0x10, 1, 0x1c, 6),   NONE, I2S_1),
	DS90UX9XX_GPIO(6, "gpio7", GPIO(0x11, 0, 0x1c, 7),   NONE, I2S_1),
	DS90UX9XX_GPIO(7, "gpio8", GPIO(0x11, 1, 0x1d, 0),   NONE, I2S_1),
};

static const struct ds90ux9xx_pin ds90ux928_pins[] = {
	DS90UX9XX_GPIO(0, "gpio0", GPIO(0x1d, 0, 0x6e, 0), REMOTE, I2S_M),
	DS90UX9XX_GPIO(1, "gpio1", GPIO(0x1e, 0, 0x6e, 1), REMOTE, I2S_M),
	DS90UX9XX_GPIO(2, "gpio2", GPIO(0x1e, 1, 0x6e, 2), REMOTE, I2S_3),
	DS90UX9XX_GPIO(3, "gpio3", GPIO(0x1f, 0, 0x6e, 3), REMOTE, I2S_4),
	DS90UX9XX_GPIO(4, "gpio5", GPIO(0x20, 0, 0x6e, 5),   NONE, I2S_2),
	DS90UX9XX_GPIO(5, "gpio6", GPIO(0x20, 1, 0x6e, 6),   NONE, I2S_1),
	DS90UX9XX_GPIO(6, "gpio7", GPIO(0x21, 0, 0x6e, 7),   NONE, I2S_1),
	DS90UX9XX_GPIO(7, "gpio8", GPIO(0x21, 1, 0x6f, 0),   NONE, I2S_1),
};

static const struct ds90ux9xx_pin ds90ux940_pins[] = {
	DS90UX940_GPIO(0, "gpio0", GPIO(0x1d, 0, 0x6e, 0), REMOTE, I2S_M, 1),
	DS90UX9XX_GPIO(1, "gpio1", GPIO(0x1e, 0, 0x6e, 1), REMOTE, I2S_M),
	DS90UX9XX_GPIO(2, "gpio2", GPIO(0x1e, 1, 0x6e, 2), REMOTE, I2S_3),
	DS90UX940_GPIO(3, "gpio3", GPIO(0x1f, 0, 0x6e, 3), REMOTE, I2S_4, 2),
	DS90UX9XX_GPIO(4, "gpio5", GPIO(0x20, 0, 0x6e, 5),   NONE, I2S_2),
	DS90UX9XX_GPIO(5, "gpio6", GPIO(0x20, 1, 0x6e, 6),   NONE, I2S_1),
	DS90UX9XX_GPIO(6, "gpio7", GPIO(0x21, 0, 0x6e, 7),   NONE, I2S_1),
	DS90UX9XX_GPIO(7, "gpio8", GPIO(0x21, 1, 0x6f, 0),   NONE, I2S_1),
	DS90UX9XX_GPIO(8, "gpio9", GPIO_SIMPLE(0x1a, 0, 1),  NONE, I2S_M),
};

static const enum ds90ux9xx_function ds90ux925_926_pin_functions[] = {
	DS90UX9XX_FUNC_GPIO,
	DS90UX9XX_FUNC_REMOTE,
	DS90UX9XX_FUNC_I2S_1,
	DS90UX9XX_FUNC_I2S_2,
	DS90UX9XX_FUNC_PARALLEL,
};

static const enum ds90ux9xx_function ds90ux927_pin_functions[] = {
	DS90UX9XX_FUNC_GPIO,
	DS90UX9XX_FUNC_REMOTE,
	DS90UX9XX_FUNC_I2S_1,
	DS90UX9XX_FUNC_I2S_2,
	DS90UX9XX_FUNC_I2S_3,
	DS90UX9XX_FUNC_I2S_4,
};

static const enum ds90ux9xx_function ds90ux928_pin_functions[] = {
	DS90UX9XX_FUNC_GPIO,
	DS90UX9XX_FUNC_REMOTE,
	DS90UX9XX_FUNC_I2S_1,
	DS90UX9XX_FUNC_I2S_2,
	DS90UX9XX_FUNC_I2S_3,
	DS90UX9XX_FUNC_I2S_4,
	DS90UX9XX_FUNC_I2S_M,
};

static const enum ds90ux9xx_function ds90ux940_pin_functions[] = {
	DS90UX9XX_FUNC_GPIO,
	DS90UX9XX_FUNC_REMOTE,
	DS90UX9XX_FUNC_PASS,
	DS90UX9XX_FUNC_I2S_1,
	DS90UX9XX_FUNC_I2S_2,
	DS90UX9XX_FUNC_I2S_3,
	DS90UX9XX_FUNC_I2S_4,
	DS90UX9XX_FUNC_I2S_M,
};

static const struct ds90ux9xx_pinctrl_data ds90ux925_pinctrl = {
	.name = "ds90ux925",
	.pins_desc = ds90ux925_926_pins_desc,
	.pins = ds90ux925_pins,
	.npins = ARRAY_SIZE(ds90ux925_pins),
	.functions = ds90ux925_926_pin_functions,
	.nfunctions = ARRAY_SIZE(ds90ux925_926_pin_functions),
};

static const struct ds90ux9xx_pinctrl_data ds90ux926_pinctrl = {
	.name = "ds90ux926",
	.pins_desc = ds90ux925_926_pins_desc,
	.pins = ds90ux926_pins,
	.npins = ARRAY_SIZE(ds90ux926_pins),
	.functions = ds90ux925_926_pin_functions,
	.nfunctions = ARRAY_SIZE(ds90ux925_926_pin_functions),
};

static const struct ds90ux9xx_pinctrl_data ds90ux927_pinctrl = {
	.name = "ds90ux927",
	.pins_desc = ds90ux927_928_pins_desc,
	.pins = ds90ux927_pins,
	.npins = ARRAY_SIZE(ds90ux927_pins),
	.functions = ds90ux927_pin_functions,
	.nfunctions = ARRAY_SIZE(ds90ux927_pin_functions),
};

static const struct ds90ux9xx_pinctrl_data ds90ux928_pinctrl = {
	.name = "ds90ux928",
	.pins_desc = ds90ux927_928_pins_desc,
	.pins = ds90ux928_pins,
	.npins = ARRAY_SIZE(ds90ux928_pins),
	.functions = ds90ux928_pin_functions,
	.nfunctions = ARRAY_SIZE(ds90ux928_pin_functions),
};

static const struct ds90ux9xx_pinctrl_data ds90ux940_pinctrl = {
	.name = "ds90ux940",
	.pins_desc = ds90ux940_pins_desc,
	.pins = ds90ux940_pins,
	.npins = ARRAY_SIZE(ds90ux940_pins),
	.functions = ds90ux940_pin_functions,
	.nfunctions = ARRAY_SIZE(ds90ux940_pin_functions),
};

struct ds90ux9xx_pin_function {
	enum ds90ux9xx_function id;
	const char **groups;
	unsigned int ngroups;
};

struct ds90ux9xx_pin_group {
	const char *name;
	unsigned int pins[DS90UX9XX_MAX_GROUP_PINS];
	unsigned int npins;
};

struct ds90ux9xx_pinctrl {
	struct device *dev;
	struct regmap *regmap;

	struct pinctrl_dev *pctrl;
	struct pinctrl_desc desc;

	struct ds90ux9xx_pin_function *functions;
	unsigned int nfunctions;

	struct ds90ux9xx_pin_group *groups;
	unsigned int ngroups;

	const struct ds90ux9xx_pin *pins;
	unsigned int npins;

	struct gpio_chip gpiochip;
	unsigned int ngpios;
};

static const char *const ds90ux9xx_func_names[] = {
	[DS90UX9XX_FUNC_NONE]		= "none",
	[DS90UX9XX_FUNC_GPIO]		= "gpio",
	[DS90UX9XX_FUNC_REMOTE]		= "gpio-remote",
	[DS90UX9XX_FUNC_PASS]		= "pass",
	[DS90UX9XX_FUNC_I2S_1]		= "i2s-1",
	[DS90UX9XX_FUNC_I2S_2]		= "i2s-2",
	[DS90UX9XX_FUNC_I2S_3]		= "i2s-3",
	[DS90UX9XX_FUNC_I2S_4]		= "i2s-4",
	[DS90UX9XX_FUNC_I2S_M]		= "i2s-m",
	[DS90UX9XX_FUNC_PARALLEL]	= "parallel",
};

static bool ds90ux9xx_func_in_group(u32 func_mask, enum ds90ux9xx_function id)
{
	u32 mask = BIT(id);

	if (id == DS90UX9XX_FUNC_I2S_4) {
		mask |= BIT(DS90UX9XX_FUNC_I2S_3);
		id = DS90UX9XX_FUNC_I2S_3;
	}

	if (id == DS90UX9XX_FUNC_I2S_3) {
		mask |= BIT(DS90UX9XX_FUNC_I2S_2);
		id = DS90UX9XX_FUNC_I2S_2;
	}

	if (id == DS90UX9XX_FUNC_I2S_2)
		mask |= BIT(DS90UX9XX_FUNC_I2S_1);

	return func_mask & mask;
}

static bool ds90ux9xx_pin_function(enum ds90ux9xx_function id)
{
	if (id == DS90UX9XX_FUNC_GPIO || id == DS90UX9XX_FUNC_REMOTE ||
	    id == DS90UX9XX_FUNC_PASS)
		return true;

	return false;
}

static int ds90ux9xx_populate_groups(struct ds90ux9xx_pinctrl *pctrl,
				     const struct ds90ux9xx_pinctrl_data *cfg)
{
	struct ds90ux9xx_pin_function *func;
	struct ds90ux9xx_pin_group *group;
	enum ds90ux9xx_function func_id;
	unsigned int i, j, n;

	pctrl->pins = cfg->pins;
	pctrl->npins = cfg->npins;

	/* Assert that only GPIO pins are muxed, it may be changed in future */
	for (j = 0; j < pctrl->npins; j++)
		if (!(pctrl->pins[j].func_mask & BIT(DS90UX9XX_FUNC_GPIO)))
			return -EINVAL;

	pctrl->ngpios = pctrl->npins;

	pctrl->nfunctions = cfg->nfunctions;
	pctrl->functions = devm_kcalloc(pctrl->dev, pctrl->nfunctions,
					sizeof(*pctrl->functions), GFP_KERNEL);
	if (!pctrl->functions)
		return -ENOMEM;

	for (i = 0; i < pctrl->nfunctions; i++) {
		func = &pctrl->functions[i];
		func->id = cfg->functions[i];

		/*
		 * Number of pin groups is a sum of pins and pin group functions
		 */
		if (ds90ux9xx_pin_function(func->id)) {
			for (j = 0; j < pctrl->npins; j++) {
				if (func->id == DS90UX9XX_FUNC_GPIO)
					pctrl->ngroups++;

				if (pctrl->pins[j].func_mask & BIT(func->id))
					func->ngroups++;
			}
		} else {
			pctrl->ngroups++;
			func->ngroups = 1;
		}

		func->groups = devm_kcalloc(pctrl->dev, func->ngroups,
					    sizeof(*func->groups), GFP_KERNEL);
		if (!func->groups)
			return -ENOMEM;

		if (ds90ux9xx_pin_function(func->id)) {
			n = 0;
			for (j = 0; j < pctrl->npins; j++)
				if (pctrl->pins[j].func_mask & BIT(func->id))
					func->groups[n++] = pctrl->pins[j].name;
		} else {
			/* Group name matches function name */
			func->groups[0] = ds90ux9xx_func_names[func->id];
		}
	}

	pctrl->groups = devm_kcalloc(pctrl->dev, pctrl->ngroups,
				     sizeof(*pctrl->groups), GFP_KERNEL);
	if (!pctrl->groups)
		return -ENOMEM;

	/* Firstly scan for GPIOs as individual pin groups */
	group = pctrl->groups;
	for (i = 0; i < pctrl->npins; i++) {
		group->name = pctrl->pins[i].name;
		group->pins[0] = pctrl->pins[i].id;
		group->npins = 1;
		group++;
	}

	/* Now scan for the rest of pin groups, which has own functions */
	for (i = 0; i < pctrl->nfunctions; i++) {
		func_id = pctrl->functions[i].id;

		/* Those pin groups were accounted above as pin functions */
		if (ds90ux9xx_pin_function(func_id))
			continue;

		group->name = ds90ux9xx_func_names[func_id];
		for (j = 0; j < pctrl->npins; j++) {
			if (ds90ux9xx_func_in_group(pctrl->pins[j].func_mask,
						    func_id)) {
				group->pins[group->npins] = pctrl->pins[j].id;
				group->npins++;
			}
		}

		group++;
	}

	return 0;
}

static int ds90ux9xx_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct ds90ux9xx_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	return pctrl->ngroups;
}

static const char *ds90ux9xx_get_group_name(struct pinctrl_dev *pctldev,
					    unsigned int group)
{
	struct ds90ux9xx_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	return pctrl->groups[group].name;
}

static int ds90ux9xx_get_group_pins(struct pinctrl_dev *pctldev,
				    unsigned int group,
				    const unsigned int **pins,
				    unsigned int *num_pins)
{
	struct ds90ux9xx_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	*pins = pctrl->groups[group].pins;
	*num_pins = pctrl->groups[group].npins;

	return 0;
}

static const struct pinctrl_ops ds90ux9xx_pinctrl_ops = {
	.get_groups_count	= ds90ux9xx_get_groups_count,
	.get_group_name		= ds90ux9xx_get_group_name,
	.get_group_pins		= ds90ux9xx_get_group_pins,
	.dt_node_to_map		= pinconf_generic_dt_node_to_map_all,
	.dt_free_map		= pinctrl_utils_free_map,
};

static int ds90ux9xx_get_funcs_count(struct pinctrl_dev *pctldev)
{
	struct ds90ux9xx_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	return pctrl->nfunctions;
}

static const char *ds90ux9xx_get_func_name(struct pinctrl_dev *pctldev,
					   unsigned int function)
{
	struct ds90ux9xx_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	return ds90ux9xx_func_names[pctrl->functions[function].id];
}

static int ds90ux9xx_get_func_groups(struct pinctrl_dev *pctldev,
				     unsigned int function,
				     const char * const **groups,
				     unsigned int * const num_groups)
{
	struct ds90ux9xx_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	*groups = pctrl->functions[function].groups;
	*num_groups = pctrl->functions[function].ngroups;

	return 0;
}

static int ds90ux9xx_gpio_read(struct ds90ux9xx_pinctrl *pctrl,
			       unsigned int offset, u8 *value)
{
	const struct ds90ux9xx_gpio *gpio;
	unsigned int val;
	int ret;

	gpio = &pctrl->pins[offset].gpio;

	ret = regmap_read(pctrl->regmap, gpio->cfg_reg, &val);
	if (ret) {
		dev_err(pctrl->dev, "Failed to read register %#x, gpio %d\n",
			gpio->cfg_reg, offset);
		return ret;
	}

	*value = val & gpio->cfg_mask;
	if (gpio->cfg_mask == 0xf0)
		*value >>= 4;

	return 0;
}

static int ds90ux9xx_gpio_read_stat(struct ds90ux9xx_pinctrl *pctrl,
				    unsigned int offset, u8 *value)
{
	const struct ds90ux9xx_gpio *gpio;
	unsigned int val;
	int ret;

	gpio = &pctrl->pins[offset].gpio;

	if (!gpio->stat_bit)
		return -EINVAL;

	ret = regmap_read(pctrl->regmap, gpio->stat_reg, &val);
	if (ret) {
		dev_err(pctrl->dev, "Failed to read register %#x, gpio %d\n",
			gpio->stat_reg, offset);
		return ret;
	}

	*value = val & gpio->stat_bit;

	return 0;
}

static int ds90ux9xx_gpio_write(struct ds90ux9xx_pinctrl *pctrl,
				unsigned int offset, u8 value)
{
	const struct ds90ux9xx_gpio *gpio;
	int ret;

	gpio = &pctrl->pins[offset].gpio;

	if (value & DIR_INPUT && !gpio->input)
		return -EINVAL;

	if (gpio->cfg_mask == 0xf0)
		value <<= 4;

	ret = regmap_update_bits(pctrl->regmap, gpio->cfg_reg,
				 gpio->cfg_mask, value);
	if (ret)
		dev_err(pctrl->dev, "Failed to modify register %#x, gpio %d\n",
			gpio->cfg_reg, offset);

	return ret;
}

static int ds90ux940_set_pass(struct ds90ux9xx_pinctrl *pctrl,
			      unsigned int pin, bool enable)
{
	u8 bit = pctrl->pins[pin].pass_bit;

	return ds90ux9xx_update_bits_indirect(pctrl->dev->parent,
			DES_REG_INDIRECT_PASS, bit, enable ? bit : 0x0);
}

static int ds90ux9xx_pinctrl_group_disable(struct ds90ux9xx_pinctrl *pctrl,
					   enum ds90ux9xx_function function,
					   unsigned int pin)
{
	int ret;

	switch (function) {
	case DS90UX9XX_FUNC_GPIO:
	case DS90UX9XX_FUNC_REMOTE:
		return ds90ux9xx_gpio_write(pctrl, pin, 0x0);
	case DS90UX9XX_FUNC_PASS:
		return ds90ux940_set_pass(pctrl, pin, false);
	default:
		break;
	}

	if (!ds90ux9xx_is_serializer(pctrl->dev->parent))
		return 0;

	switch (function) {
	case DS90UX9XX_FUNC_PARALLEL:
		return regmap_update_bits(pctrl->regmap, SER_REG_PIN_CTRL,
					  PIN_CTRL_RGB18, PIN_CTRL_RGB18);
	case DS90UX9XX_FUNC_I2S_4:
	case DS90UX9XX_FUNC_I2S_3:
		ret = regmap_update_bits(pctrl->regmap, SER_REG_I2S_SURROUND,
					 PIN_CTRL_I2S_SURR_BIT, 0x0);
		if (ret)
			return ret;
		/* Fall through */
	case DS90UX9XX_FUNC_I2S_2:
		return regmap_update_bits(pctrl->regmap, SER_REG_PIN_CTRL,
					  PIN_CTRL_I2S_CHANNEL_B, 0x0);
	default:
		break;
	}

	return 0;
}

static int ds90ux9xx_pinctrl_group_enable(struct ds90ux9xx_pinctrl *pctrl,
					  enum ds90ux9xx_function function,
					  struct ds90ux9xx_pin_group *group)
{
	unsigned int pin = group->pins[0];
	int ret;

	switch (function) {
	case DS90UX9XX_FUNC_GPIO:
		/* Not all GPIOs can be set to input, fallback to output low */
		ret = ds90ux9xx_gpio_write(pctrl, pin, GPIO_AS_INPUT);
		if (ret < 0)
			ret = ds90ux9xx_gpio_write(pctrl, pin, GPIO_OUTPUT_LOW);
		return ret;
	case DS90UX9XX_FUNC_REMOTE:
		return ds90ux9xx_gpio_write(pctrl, pin, GPIO_OUTPUT_REMOTE);
	case DS90UX9XX_FUNC_PASS:
		return ds90ux940_set_pass(pctrl, pin, true);
	default:
		break;
	}

	if (!ds90ux9xx_is_serializer(pctrl->dev->parent))
		return 0;

	switch (function) {
	case DS90UX9XX_FUNC_PARALLEL:
		return regmap_update_bits(pctrl->regmap, SER_REG_PIN_CTRL,
					  PIN_CTRL_RGB18, 0x0);
	case DS90UX9XX_FUNC_I2S_4:
	case DS90UX9XX_FUNC_I2S_3:
		ret = regmap_update_bits(pctrl->regmap, SER_REG_I2S_SURROUND,
					 PIN_CTRL_I2S_SURR_BIT,
					 PIN_CTRL_I2S_SURR_BIT);
		if (ret)
			return ret;
		/* Fall through */
	case DS90UX9XX_FUNC_I2S_2:
		return regmap_update_bits(pctrl->regmap, SER_REG_PIN_CTRL,
			PIN_CTRL_I2S_CHANNEL_B | PIN_CTRL_I2S_DATA_ISLAND,
					  PIN_CTRL_I2S_CHANNEL_B);
	default:
		break;
	}

	return 0;
}

static int ds90ux9xx_pinctrl_func_enable(struct ds90ux9xx_pinctrl *pctrl,
					 enum ds90ux9xx_function function,
					 struct ds90ux9xx_pin_group *group)
{
	enum ds90ux9xx_function func;
	unsigned int i, pin;
	u32 func_mask;
	int ret;

	/* Disable probably enabled pin functions with pin resource conflicts */
	for (i = 0; i < group->npins; i++) {
		pin = group->pins[i];

		func_mask = pctrl->pins[pin].func_mask & ~BIT(function);

		/* Abandon remote GPIOs which are covered by GPIO function */
		if (function == DS90UX9XX_FUNC_REMOTE)
			func_mask &= ~BIT(DS90UX9XX_FUNC_GPIO);
		else
			func_mask &= ~BIT(DS90UX9XX_FUNC_REMOTE);

		/* Zero to three possible conflicting pin functions */
		while (func_mask) {
			func = __ffs(func_mask);
			func_mask &= ~BIT(func);
			ret = ds90ux9xx_pinctrl_group_disable(pctrl, func, pin);
			if (ret)
				return ret;
		}
	}

	return ds90ux9xx_pinctrl_group_enable(pctrl, function, group);
}

static int ds90ux9xx_set_mux(struct pinctrl_dev *pctldev,
			     unsigned int function, unsigned int group)
{
	struct ds90ux9xx_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	enum ds90ux9xx_function func = pctrl->functions[function].id;
	struct ds90ux9xx_pin_group *grp = &pctrl->groups[group];

	return ds90ux9xx_pinctrl_func_enable(pctrl, func, grp);
}

static int ds90ux9xx_gpio_request_enable(struct pinctrl_dev *pctldev,
					 struct pinctrl_gpio_range *range,
					 unsigned int offset)
{
	struct ds90ux9xx_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	return ds90ux9xx_pinctrl_func_enable(pctrl, DS90UX9XX_FUNC_GPIO,
					     &pctrl->groups[offset]);
}

static const struct pinmux_ops ds90ux9xx_pinmux_ops = {
	.get_functions_count	= ds90ux9xx_get_funcs_count,
	.get_function_name	= ds90ux9xx_get_func_name,
	.get_function_groups	= ds90ux9xx_get_func_groups,
	.set_mux		= ds90ux9xx_set_mux,
	.gpio_request_enable	= ds90ux9xx_gpio_request_enable,
	.strict = true,
};

static const struct pinctrl_desc ds90ux9xx_pinctrl_desc = {
	.pctlops	= &ds90ux9xx_pinctrl_ops,
	.pmxops		= &ds90ux9xx_pinmux_ops,
};

static int ds90ux9xx_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct ds90ux9xx_pinctrl *pctrl = gpiochip_get_data(chip);
	int ret;
	u8 val;

	ret = ds90ux9xx_gpio_read(pctrl, offset, &val);
	if (ret)
		return ret;

	if (!(val & DIR_INPUT))
		return !!(val & OUTPUT_HIGH);

	ret = ds90ux9xx_gpio_read_stat(pctrl, offset, &val);
	if (ret)
		return ret;

	return !!val;
}

static void ds90ux9xx_gpio_set(struct gpio_chip *chip, unsigned int offset,
			       int value)
{
	struct ds90ux9xx_pinctrl *pctrl = gpiochip_get_data(chip);
	u8 val = value ? GPIO_OUTPUT_HIGH : GPIO_OUTPUT_LOW;

	ds90ux9xx_gpio_write(pctrl, offset, val);
}

static int ds90ux9xx_gpio_get_direction(struct gpio_chip *chip,
					unsigned int offset)
{
	struct ds90ux9xx_pinctrl *pctrl = gpiochip_get_data(chip);
	int ret;
	u8 val;

	ret = ds90ux9xx_gpio_read(pctrl, offset, &val);
	if (ret)
		return ret;

	return !!(val & DIR_INPUT);
}

static int ds90ux9xx_gpio_direction_input(struct gpio_chip *chip,
					  unsigned int offset)
{
	struct ds90ux9xx_pinctrl *pctrl = gpiochip_get_data(chip);

	return ds90ux9xx_gpio_write(pctrl, offset, GPIO_AS_INPUT);
}

static int ds90ux9xx_gpio_direction_output(struct gpio_chip *chip,
					   unsigned int offset, int value)
{
	struct ds90ux9xx_pinctrl *pctrl = gpiochip_get_data(chip);
	u8 val = value ? GPIO_OUTPUT_HIGH : GPIO_OUTPUT_LOW;

	return ds90ux9xx_gpio_write(pctrl, offset, val);
}

static const struct gpio_chip ds90ux9xx_gpio_chip = {
	.owner			= THIS_MODULE,
	.get			= ds90ux9xx_gpio_get,
	.set			= ds90ux9xx_gpio_set,
	.get_direction		= ds90ux9xx_gpio_get_direction,
	.direction_input	= ds90ux9xx_gpio_direction_input,
	.direction_output	= ds90ux9xx_gpio_direction_output,
	.base			= -1,
	.can_sleep		= 1,
	.of_gpio_n_cells	= 2,
	.of_xlate		= of_gpio_simple_xlate,
};

static int ds90ux9xx_parse_dt_properties(struct ds90ux9xx_pinctrl *pctrl)
{
	struct device_node *np = pctrl->dev->of_node;
	unsigned int val;

	if (!ds90ux9xx_is_serializer(pctrl->dev->parent))
		return 0;

	/*
	 * Optionally set "Video Color Depth Mode" to RGB18 mode, it may be
	 * needed if DS90Ux927 serializer is paired with DS90Ux926 deserializer
	 * or if there is no pins conflicting with the "parallel" pin group
	 * to disable RGB24 mode implicitly.
	 */
	if (of_property_read_bool(np, "ti,video-depth-18bit"))
		val = PIN_CTRL_RGB18;
	else
		val = 0;

	return regmap_update_bits(pctrl->regmap, SER_REG_PIN_CTRL,
				  PIN_CTRL_RGB18, val);
}

static void ds90ux9xx_get_data(struct ds90ux9xx_pinctrl *pctrl,
			       const struct ds90ux9xx_pinctrl_data **pctrl_data)
{
	enum ds90ux9xx_device_id id = ds90ux9xx_get_ic_type(pctrl->dev->parent);

	switch (id) {
	case TI_DS90UB925:
	case TI_DS90UH925:
		*pctrl_data = &ds90ux925_pinctrl;
		break;
	case TI_DS90UB927:
	case TI_DS90UH927:
		*pctrl_data = &ds90ux927_pinctrl;
		break;
	case TI_DS90UB926:
	case TI_DS90UH926:
		*pctrl_data = &ds90ux926_pinctrl;
		break;
	case TI_DS90UB928:
	case TI_DS90UH928:
		*pctrl_data = &ds90ux928_pinctrl;
		break;
	case TI_DS90UB940:
	case TI_DS90UH940:
		*pctrl_data = &ds90ux940_pinctrl;
		break;
	default:
		dev_err(pctrl->dev, "Unsupported hardware id %d\n", id);
	}
}

static int ds90ux9xx_pinctrl_probe(struct platform_device *pdev)
{
	const struct ds90ux9xx_pinctrl_data *pctrl_data;
	struct ds90ux9xx_pinctrl *pctrl;
	struct device *dev = &pdev->dev;
	int ret;

	pctrl = devm_kzalloc(dev, sizeof(*pctrl), GFP_KERNEL);
	if (!pctrl)
		return -ENOMEM;

	pctrl->dev = dev;
	pctrl->regmap = dev_get_regmap(dev->parent, NULL);
	if (!pctrl->regmap)
		return -ENODEV;

	pctrl_data = of_device_get_match_data(dev);
	if (!pctrl_data)
		ds90ux9xx_get_data(pctrl, &pctrl_data);

	if (!pctrl_data)
		return -ENODEV;

	ret = ds90ux9xx_populate_groups(pctrl, pctrl_data);
	if (ret)
		return ret;

	ret = ds90ux9xx_parse_dt_properties(pctrl);
	if (ret)
		return ret;

	pctrl->desc = ds90ux9xx_pinctrl_desc;
	pctrl->desc.name = pctrl_data->name;
	pctrl->desc.pins = pctrl_data->pins_desc;
	pctrl->desc.npins = pctrl_data->npins;

	pctrl->pctrl = devm_pinctrl_register(dev, &pctrl->desc, pctrl);
	if (IS_ERR(pctrl->pctrl))
		return PTR_ERR(pctrl->pctrl);

	platform_set_drvdata(pdev, pctrl);

	pctrl->gpiochip = ds90ux9xx_gpio_chip;
	pctrl->gpiochip.parent = dev;
	pctrl->gpiochip.label = pctrl_data->name;
	pctrl->gpiochip.ngpio = pctrl->ngpios;

	if (of_property_read_bool(dev->of_node, "gpio-ranges")) {
		pctrl->gpiochip.request = gpiochip_generic_request;
		pctrl->gpiochip.free = gpiochip_generic_free;
	}

	return devm_gpiochip_add_data(dev, &pctrl->gpiochip, pctrl);
}

static const struct of_device_id ds90ux9xx_pinctrl_dt_ids[] = {
	{ .compatible = "ti,ds90ux9xx-pinctrl", },
	{ .compatible = "ti,ds90ux925-pinctrl", .data = &ds90ux925_pinctrl, },
	{ .compatible = "ti,ds90ux926-pinctrl", .data = &ds90ux926_pinctrl, },
	{ .compatible = "ti,ds90ux927-pinctrl", .data = &ds90ux927_pinctrl, },
	{ .compatible = "ti,ds90ux928-pinctrl", .data = &ds90ux928_pinctrl, },
	{ .compatible = "ti,ds90ux940-pinctrl", .data = &ds90ux940_pinctrl, },
	{ },
};
MODULE_DEVICE_TABLE(of, ds90ux9xx_pinctrl_dt_ids);

static struct platform_driver ds90ux9xx_pinctrl_driver = {
	.probe = ds90ux9xx_pinctrl_probe,
	.driver = {
		.name = "ds90ux9xx-pinctrl",
		.of_match_table = ds90ux9xx_pinctrl_dt_ids,
	},
};
module_platform_driver(ds90ux9xx_pinctrl_driver);

MODULE_AUTHOR("Vladimir Zapolskiy <vladimir_zapolskiy@mentor.com>");
MODULE_DESCRIPTION("TI DS90Ux9xx pinmux and GPIO controller driver");
MODULE_LICENSE("GPL");
