// SPDX-License-Identifier: GPL-2.0
//
// UP Board pin controller driver
//
// Copyright (c) 2018, Emutex Ltd.
//
// Authors: Javier Arteaga <javier@emutex.com>
//          Dan O'Donovan <dan@emutex.com>
//

#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/kernel.h>
#include <linux/mfd/upboard.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/string.h>

#include "core.h"

#define to_upboard_pinctrl(gc) container_of(gc, struct upboard_pinctrl, chip)

struct upboard_pin {
	struct regmap_field *func_en;
	struct regmap_field *gpio_en;
	struct regmap_field *gpio_dir;
};

struct upboard_pinctrl {
	struct pinctrl_dev *pctldev;
	struct gpio_chip chip;
	unsigned int nsoc_gpios;
	struct gpio_desc **soc_gpios;
};

enum upboard_func0_enables {
	UPBOARD_I2C0_EN = 8,
	UPBOARD_I2C1_EN = 9,
};

static const struct reg_field upboard_i2c0_reg =
	REG_FIELD(UPBOARD_REG_FUNC_EN0, UPBOARD_I2C0_EN, UPBOARD_I2C0_EN);

static const struct reg_field upboard_i2c1_reg =
	REG_FIELD(UPBOARD_REG_FUNC_EN0, UPBOARD_I2C1_EN, UPBOARD_I2C1_EN);

#define UPBOARD_BIT_TO_PIN(r, bit) ((r) * UPBOARD_REGISTER_SIZE + (bit))

/*
 * UP Squared data
 */

#define UPBOARD_UP2_BIT_TO_PIN(r, id) (UPBOARD_BIT_TO_PIN(r, UPBOARD_UP2_##id))

#define UPBOARD_UP2_PIN_ANON(r, bit)					\
	{								\
		.number = UPBOARD_BIT_TO_PIN(r, bit),			\
	}

#define UPBOARD_UP2_PIN_NAME(r, id)					\
	{								\
		.number = UPBOARD_UP2_BIT_TO_PIN(r, id),		\
		.name = #id,						\
	}

#define UPBOARD_UP2_PIN_FUNC(r, id, data)				\
	{								\
		.number = UPBOARD_UP2_BIT_TO_PIN(r, id),		\
		.name = #id,						\
		.drv_data = (void *)(data),				\
	}

enum upboard_up2_reg0_bit {
	UPBOARD_UP2_UART1_TXD,
	UPBOARD_UP2_UART1_RXD,
	UPBOARD_UP2_UART1_RTS,
	UPBOARD_UP2_UART1_CTS,
	UPBOARD_UP2_GPIO3,
	UPBOARD_UP2_GPIO5,
	UPBOARD_UP2_GPIO6,
	UPBOARD_UP2_GPIO11,
	UPBOARD_UP2_EXHAT_LVDS1n,
	UPBOARD_UP2_EXHAT_LVDS1p,
	UPBOARD_UP2_SPI2_TXD,
	UPBOARD_UP2_SPI2_RXD,
	UPBOARD_UP2_SPI2_FS1,
	UPBOARD_UP2_SPI2_FS0,
	UPBOARD_UP2_SPI2_CLK,
	UPBOARD_UP2_SPI1_TXD,
};

enum upboard_up2_reg1_bit {
	UPBOARD_UP2_SPI1_RXD,
	UPBOARD_UP2_SPI1_FS1,
	UPBOARD_UP2_SPI1_FS0,
	UPBOARD_UP2_SPI1_CLK,
	UPBOARD_UP2_BIT20,
	UPBOARD_UP2_BIT21,
	UPBOARD_UP2_BIT22,
	UPBOARD_UP2_BIT23,
	UPBOARD_UP2_PWM1,
	UPBOARD_UP2_PWM0,
	UPBOARD_UP2_EXHAT_LVDS0n,
	UPBOARD_UP2_EXHAT_LVDS0p,
	UPBOARD_UP2_I2C0_SCL,
	UPBOARD_UP2_I2C0_SDA,
	UPBOARD_UP2_I2C1_SCL,
	UPBOARD_UP2_I2C1_SDA,
};

enum upboard_up2_reg2_bit {
	UPBOARD_UP2_EXHAT_LVDS3n,
	UPBOARD_UP2_EXHAT_LVDS3p,
	UPBOARD_UP2_EXHAT_LVDS4n,
	UPBOARD_UP2_EXHAT_LVDS4p,
	UPBOARD_UP2_EXHAT_LVDS5n,
	UPBOARD_UP2_EXHAT_LVDS5p,
	UPBOARD_UP2_I2S_SDO,
	UPBOARD_UP2_I2S_SDI,
	UPBOARD_UP2_I2S_WS_SYNC,
	UPBOARD_UP2_I2S_BCLK,
	UPBOARD_UP2_EXHAT_LVDS6n,
	UPBOARD_UP2_EXHAT_LVDS6p,
	UPBOARD_UP2_EXHAT_LVDS7n,
	UPBOARD_UP2_EXHAT_LVDS7p,
	UPBOARD_UP2_EXHAT_LVDS2n,
	UPBOARD_UP2_EXHAT_LVDS2p,
};

static struct pinctrl_pin_desc upboard_up2_pins[] = {
	UPBOARD_UP2_PIN_NAME(0, UART1_TXD),
	UPBOARD_UP2_PIN_NAME(0, UART1_RXD),
	UPBOARD_UP2_PIN_NAME(0, UART1_RTS),
	UPBOARD_UP2_PIN_NAME(0, UART1_CTS),
	UPBOARD_UP2_PIN_NAME(0, GPIO3),
	UPBOARD_UP2_PIN_NAME(0, GPIO5),
	UPBOARD_UP2_PIN_NAME(0, GPIO6),
	UPBOARD_UP2_PIN_NAME(0, GPIO11),
	UPBOARD_UP2_PIN_NAME(0, EXHAT_LVDS1n),
	UPBOARD_UP2_PIN_NAME(0, EXHAT_LVDS1p),
	UPBOARD_UP2_PIN_NAME(0, SPI2_TXD),
	UPBOARD_UP2_PIN_NAME(0, SPI2_RXD),
	UPBOARD_UP2_PIN_NAME(0, SPI2_FS1),
	UPBOARD_UP2_PIN_NAME(0, SPI2_FS0),
	UPBOARD_UP2_PIN_NAME(0, SPI2_CLK),
	UPBOARD_UP2_PIN_NAME(0, SPI1_TXD),
	UPBOARD_UP2_PIN_NAME(1, SPI1_RXD),
	UPBOARD_UP2_PIN_NAME(1, SPI1_FS1),
	UPBOARD_UP2_PIN_NAME(1, SPI1_FS0),
	UPBOARD_UP2_PIN_NAME(1, SPI1_CLK),
	UPBOARD_UP2_PIN_ANON(1, 4),
	UPBOARD_UP2_PIN_ANON(1, 5),
	UPBOARD_UP2_PIN_ANON(1, 6),
	UPBOARD_UP2_PIN_ANON(1, 7),
	UPBOARD_UP2_PIN_NAME(1, PWM1),
	UPBOARD_UP2_PIN_NAME(1, PWM0),
	UPBOARD_UP2_PIN_NAME(1, EXHAT_LVDS0n),
	UPBOARD_UP2_PIN_NAME(1, EXHAT_LVDS0p),
	UPBOARD_UP2_PIN_FUNC(1, I2C0_SCL, &upboard_i2c0_reg),
	UPBOARD_UP2_PIN_FUNC(1, I2C0_SDA, &upboard_i2c0_reg),
	UPBOARD_UP2_PIN_FUNC(1, I2C1_SCL, &upboard_i2c1_reg),
	UPBOARD_UP2_PIN_FUNC(1, I2C1_SDA, &upboard_i2c1_reg),
	UPBOARD_UP2_PIN_NAME(2, EXHAT_LVDS3n),
	UPBOARD_UP2_PIN_NAME(2, EXHAT_LVDS3p),
	UPBOARD_UP2_PIN_NAME(2, EXHAT_LVDS4n),
	UPBOARD_UP2_PIN_NAME(2, EXHAT_LVDS4p),
	UPBOARD_UP2_PIN_NAME(2, EXHAT_LVDS5n),
	UPBOARD_UP2_PIN_NAME(2, EXHAT_LVDS5p),
	UPBOARD_UP2_PIN_NAME(2, I2S_SDO),
	UPBOARD_UP2_PIN_NAME(2, I2S_SDI),
	UPBOARD_UP2_PIN_NAME(2, I2S_WS_SYNC),
	UPBOARD_UP2_PIN_NAME(2, I2S_BCLK),
	UPBOARD_UP2_PIN_NAME(2, EXHAT_LVDS6n),
	UPBOARD_UP2_PIN_NAME(2, EXHAT_LVDS6p),
	UPBOARD_UP2_PIN_NAME(2, EXHAT_LVDS7n),
	UPBOARD_UP2_PIN_NAME(2, EXHAT_LVDS7p),
	UPBOARD_UP2_PIN_NAME(2, EXHAT_LVDS2n),
	UPBOARD_UP2_PIN_NAME(2, EXHAT_LVDS2p),
};

static int upboard_get_functions_count(struct pinctrl_dev *pctldev)
{
	return 0;
}

static int upboard_get_function_groups(struct pinctrl_dev *pctldev,
				       unsigned int selector,
				       const char * const **groups,
				       unsigned int *num_groups)
{
	*groups = NULL;
	*num_groups = 0;
	return 0;
}

static const char *upboard_get_function_name(struct pinctrl_dev *pctldev,
					     unsigned int selector)
{
	return NULL;
}

static int upboard_set_mux(struct pinctrl_dev *pctldev, unsigned int function,
			   unsigned int group)
{
	return 0;
}

static int upboard_gpio_request_enable(struct pinctrl_dev *pctldev,
				       struct pinctrl_gpio_range *range,
				       unsigned int pin)
{
	const struct pin_desc * const pd = pin_desc_get(pctldev, pin);
	const struct upboard_pin *p = pd->drv_data;
	int ret;

	/* if this pin has an associated function bit, disable it first */
	if (p->func_en) {
		ret = regmap_field_write(p->func_en, 0);
		if (ret)
			return ret;
	}

	if (p->gpio_en) {
		ret = regmap_field_write(p->gpio_en, 1);
		if (ret)
			return ret;
	}

	return 0;
}

static int upboard_gpio_set_direction(struct pinctrl_dev *pctldev,
				      struct pinctrl_gpio_range *range,
				      unsigned int pin, bool input)
{
	const struct pin_desc * const pd = pin_desc_get(pctldev, pin);
	const struct upboard_pin *p = pd->drv_data;

	return regmap_field_write(p->gpio_dir, input);
}

static const struct pinmux_ops upboard_pinmux_ops = {
	.get_functions_count = upboard_get_functions_count,
	.get_function_groups = upboard_get_function_groups,
	.get_function_name = upboard_get_function_name,
	.set_mux = upboard_set_mux,
	.gpio_request_enable = upboard_gpio_request_enable,
	.gpio_set_direction = upboard_gpio_set_direction,
};

static int upboard_get_groups_count(struct pinctrl_dev *pctldev)
{
	return 0;
}

static const char *upboard_get_group_name(struct pinctrl_dev *pctldev,
					  unsigned int selector)
{
	return NULL;
}

static const struct pinctrl_ops upboard_pinctrl_ops = {
	.get_groups_count = upboard_get_groups_count,
	.get_group_name = upboard_get_group_name,
};

static struct pinctrl_desc upboard_up2_pinctrl_desc = {
	.pins = upboard_up2_pins,
	.npins = ARRAY_SIZE(upboard_up2_pins),
	.pctlops = &upboard_pinctrl_ops,
	.pmxops = &upboard_pinmux_ops,
	.owner = THIS_MODULE,
};

static int upboard_gpio_request(struct gpio_chip *gc, unsigned int offset)
{
	struct upboard_pinctrl *pctrl = to_upboard_pinctrl(gc);
	struct gpio_desc *desc;
	int ret;

	ret = pinctrl_gpio_request(gc->base + offset);
	if (ret)
		return ret;

	desc = devm_gpiod_get_index(gc->parent, "external", offset, GPIOD_ASIS);
	if (IS_ERR(desc))
		return PTR_ERR(desc);

	pctrl->soc_gpios[offset] = desc;
	return 0;
}

static void upboard_gpio_free(struct gpio_chip *gc, unsigned int offset)
{
	struct upboard_pinctrl *pctrl = to_upboard_pinctrl(gc);

	if (!pctrl->soc_gpios[offset])
		return;

	devm_gpiod_put(gc->parent, pctrl->soc_gpios[offset]);
	pctrl->soc_gpios[offset] = NULL;

	pinctrl_gpio_free(gc->base + offset);
}

static int upboard_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct upboard_pinctrl *pctrl = to_upboard_pinctrl(gc);
	struct gpio_desc *desc = pctrl->soc_gpios[offset];

	if (!desc)
		return -ENODEV;

	return gpiod_get_direction(desc);
}

static int upboard_gpio_direction_input(struct gpio_chip *gc,
					unsigned int offset)
{
	struct upboard_pinctrl *pctrl = to_upboard_pinctrl(gc);
	struct gpio_desc *desc = pctrl->soc_gpios[offset];
	int ret;

	if (!desc)
		return -ENODEV;

	ret = gpiod_direction_input(desc);
	if (ret)
		return ret;

	return pinctrl_gpio_direction_input(gc->base + offset);
}

static int upboard_gpio_direction_output(struct gpio_chip *gc,
					 unsigned int offset, int value)
{
	struct upboard_pinctrl *pctrl = to_upboard_pinctrl(gc);
	struct gpio_desc *desc = pctrl->soc_gpios[offset];
	int ret;

	if (!desc)
		return -ENODEV;

	ret = pinctrl_gpio_direction_output(gc->base + offset);
	if (ret)
		return ret;

	return gpiod_direction_output(desc, value);
}

static int upboard_gpio_get_value(struct gpio_chip *gc, unsigned int offset)
{
	struct upboard_pinctrl *pctrl = to_upboard_pinctrl(gc);
	struct gpio_desc *desc = pctrl->soc_gpios[offset];

	if (!desc)
		return -ENODEV;

	return gpiod_get_value(desc);
}

static void upboard_gpio_set_value(struct gpio_chip *gc, unsigned int offset,
				   int value)
{
	struct upboard_pinctrl *pctrl = to_upboard_pinctrl(gc);
	struct gpio_desc *desc = pctrl->soc_gpios[offset];

	if (!desc)
		return;

	gpiod_set_value(desc, value);
}

static struct gpio_chip upboard_gpio_chip = {
	.label = "UP pin controller",
	.owner = THIS_MODULE,
	.request = upboard_gpio_request,
	.free = upboard_gpio_free,
	.get_direction = upboard_gpio_get_direction,
	.direction_input = upboard_gpio_direction_input,
	.direction_output = upboard_gpio_direction_output,
	.get = upboard_gpio_get_value,
	.set = upboard_gpio_set_value,
	.base = -1,
};

static struct regmap_field *upboard_field_alloc(struct device *dev,
						struct regmap *regmap,
						unsigned int base,
						unsigned int number)
{
	const unsigned int reg = number / UPBOARD_REGISTER_SIZE;
	const unsigned int bit = number % UPBOARD_REGISTER_SIZE;
	const struct reg_field field = {
		.reg = base + reg,
		.msb = bit,
		.lsb = bit,
	};

	return devm_regmap_field_alloc(dev, regmap, field);
}

static int upboard_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pinctrl_desc *pctldesc;
	struct upboard_pinctrl *pctrl;
	struct upboard_pin *pins;
	struct regmap *regmap;
	unsigned int i;
	int ret;

	if (!dev->parent)
		return -EINVAL;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap)
		return -EINVAL;

	pctldesc = &upboard_up2_pinctrl_desc;
	pctldesc->name = dev_name(dev);

	pins = devm_kcalloc(dev, pctldesc->npins, sizeof(*pins), GFP_KERNEL);
	if (!pins)
		return -ENOMEM;

	for (i = 0; i < pctldesc->npins; i++) {
		struct upboard_pin *pin = &pins[i];
		struct pinctrl_pin_desc *pd;

		pd = (struct pinctrl_pin_desc *)&pctldesc->pins[i];
		if (pd->drv_data) {
			struct reg_field *field = pd->drv_data;

			pin->func_en = devm_regmap_field_alloc(dev, regmap,
							       *field);
			if (IS_ERR(pin->func_en))
				return PTR_ERR(pin->func_en);
		}

		pin->gpio_en = upboard_field_alloc(dev, regmap,
						   UPBOARD_REG_GPIO_EN0, i);
		if (IS_ERR(pin->gpio_en))
			return PTR_ERR(pin->gpio_en);

		pin->gpio_dir = upboard_field_alloc(dev, regmap,
						    UPBOARD_REG_GPIO_DIR0, i);
		if (IS_ERR(pin->gpio_dir))
			return PTR_ERR(pin->gpio_dir);

		pd->drv_data = pin;
	}

	pctrl = devm_kzalloc(dev, sizeof(*pctrl), GFP_KERNEL);
	if (!pctrl)
		return -ENOMEM;

	pctrl->chip = upboard_gpio_chip;
	pctrl->chip.parent = dev;
	pctrl->chip.ngpio = pctldesc->npins;

	pctrl->nsoc_gpios = gpiod_count(dev, "external");
	pctrl->soc_gpios = devm_kcalloc(dev, pctrl->nsoc_gpios,
					sizeof(*pctrl->soc_gpios), GFP_KERNEL);
	if (!pctrl->soc_gpios)
		return -ENOMEM;

	pctrl->pctldev = devm_pinctrl_register(dev, pctldesc, pctrl);
	if (IS_ERR(pctrl->pctldev))
		return PTR_ERR(pctrl->pctldev);

	ret = devm_gpiochip_add_data(dev, &pctrl->chip, &pctrl->chip);
	if (ret)
		return ret;

	return gpiochip_add_pin_range(&pctrl->chip, pctldesc->name, 0, 0,
				      pctldesc->npins);
}

static struct platform_driver upboard_pinctrl_driver = {
	.driver = {
		.name = "upboard-pinctrl",
	},
};

module_platform_driver_probe(upboard_pinctrl_driver, upboard_pinctrl_probe);

MODULE_ALIAS("platform:upboard-pinctrl");
MODULE_AUTHOR("Javier Arteaga <javier@emutex.com>");
MODULE_AUTHOR("Dan O'Donovan <dan@emutex.com>");
MODULE_DESCRIPTION("UP Board pin control and GPIO driver");
MODULE_LICENSE("GPL v2");
