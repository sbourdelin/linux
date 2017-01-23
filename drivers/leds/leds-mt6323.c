/*
 * LED driver for Mediatek MT6323 PMIC
 *
 * Copyright (C) 2017 Sean Wang <sean.wang@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/leds.h>
#include <linux/regmap.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6323/registers.h>
#include <linux/module.h>

/* Register to enable 32K clock common for LED device */
#define MTK_MT6323_TOP_CKPDN0         0x0102
#define RG_DRV_32K_CK_PDN	      BIT(11)
#define RG_DRV_32K_CK_PDN_MASK	      BIT(11)

/* Register to enable individual clock for LED device */
#define MTK_MT6323_TOP_CKPDN2         0x010E
#define RG_ISINK_CK_PDN(i)	      BIT(i)
#define RG_ISINK_CK_PDN_MASK(i)       BIT(i)

/* Register to select clock source */
#define MTK_MT6323_TOP_CKCON1	      0x0126
#define RG_ISINK_CK_SEL_MASK(i)	      (BIT(10) << (i))

/* Register to setup the duty cycle of the blink */
#define MTK_MT6323_ISINK_CON0(i)      (0x0330 + 0x8 * (i))
#define ISINK_DIM_DUTY(i)	      (((i) << 8) & GENMASK(12, 8))
#define ISINK_DIM_DUTY_MASK	      GENMASK(12, 8)

/* Register to setup the period of the blink */
#define MTK_MT6323_ISINK_CON1(i)      (0x0332 + 0x8 * (i))
#define ISINK_DIM_FSEL(i)	      ((i) & GENMASK(15, 0))
#define ISINK_DIM_FSEL_MASK	      GENMASK(15, 0)

/* Register to control the brightness */
#define MTK_MT6323_ISINK_CON2(i)      (0x0334 + 0x8 * (i))
#define ISINK_CH_STEP(i)	      (((i) << 12) & GENMASK(14, 12))
#define ISINK_CH_STEP_MASK	      GENMASK(14, 12)
#define ISINK_SFSTR0_TC(i)	      (((i) << 1) & GENMASK(2, 1))
#define ISINK_SFSTR0_TC_MASK	      GENMASK(2, 1)
#define ISINK_SFSTR0_EN		      BIT(0)
#define ISINK_SFSTR0_EN_MASK	      BIT(0)

/* Register to LED channel enablement */
#define MTK_MT6323_ISINK_EN_CTRL      0x0356
#define ISINK_CH_EN(i)		      BIT(i)
#define ISINK_CH_EN_MASK(i)	      BIT(i)

#define MTK_MAX_PERIOD		      10000
#define MTK_MAX_DEVICES			  4
#define MTK_MAX_BRIGHTNESS		  6

struct mtk_led;
struct mtk_leds;

/**
 * struct mtk_led - state container for the LED device
 * @id: the identifier in MT6323 LED device
 * @parent: the pointer to MT6323 LED controller
 * @cdev: LED class device for this LED device
 * @current_brightness: current state of the LED device
 */
struct mtk_led {
	int    id;
	struct mtk_leds *parent;
	struct led_classdev cdev;
	u8 current_brightness;
};

/* struct mtk_leds -	state container for holding LED controller
 *			of the driver
 * @dev:		The device pointer
 * @hw:			The underlying hardware providing shared
			bus for the register operations
 * @led_num:		How much the LED device the controller could control
 * @lock:		The lock among process context
 * @led:		The array that contains the state of individual
			LED device
 */
struct mtk_leds {
	struct device	*dev;
	struct mt6397_chip *hw;
	u8     led_num;
	/* protect among process context */
	struct mutex	 lock;
	struct mtk_led	 led[4];
};

static void mtk_led_hw_off(struct led_classdev *cdev)
{
	struct mtk_led *led = container_of(cdev, struct mtk_led, cdev);
	struct mtk_leds *leds = led->parent;
	struct regmap *regmap = leds->hw->regmap;
	unsigned int status;

	status = ISINK_CH_EN(led->id);
	regmap_update_bits(regmap, MTK_MT6323_ISINK_EN_CTRL,
			   ISINK_CH_EN_MASK(led->id), ~status);

	usleep_range(100, 300);
	regmap_update_bits(regmap, MTK_MT6323_TOP_CKPDN2,
			   RG_ISINK_CK_PDN_MASK(led->id),
			   RG_ISINK_CK_PDN(led->id));

	dev_dbg(leds->dev, "%s called for led%d\n",
		__func__, led->id);
}

static u8 get_mtk_led_hw_brightness(struct led_classdev *cdev)
{
	struct mtk_led *led = container_of(cdev, struct mtk_led, cdev);
	struct mtk_leds *leds = led->parent;
	struct regmap *regmap = leds->hw->regmap;
	unsigned int status;

	regmap_read(regmap, MTK_MT6323_TOP_CKPDN2, &status);
	if (status & RG_ISINK_CK_PDN_MASK(led->id))
		return 0;

	regmap_read(regmap, MTK_MT6323_ISINK_EN_CTRL, &status);
	if (!(status & ISINK_CH_EN(led->id)))
		return 0;

	regmap_read(regmap, MTK_MT6323_ISINK_CON2(led->id), &status);
	return  ((status & ISINK_CH_STEP_MASK) >> 12) + 1;
}

static void mtk_led_hw_on(struct led_classdev *cdev)
{
	struct mtk_led *led = container_of(cdev, struct mtk_led, cdev);
	struct mtk_leds *leds = led->parent;
	struct regmap *regmap = leds->hw->regmap;
	unsigned int status;

	/* Setup required clock source, enable the corresponding
	 * clock and channel and let work with continuous blink as
	 * the default
	 */
	regmap_update_bits(regmap, MTK_MT6323_TOP_CKCON1,
			   RG_ISINK_CK_SEL_MASK(led->id), 0);

	status = RG_ISINK_CK_PDN(led->id);
	regmap_update_bits(regmap, MTK_MT6323_TOP_CKPDN2,
			   RG_ISINK_CK_PDN_MASK(led->id), ~status);

	usleep_range(100, 300);

	regmap_update_bits(regmap, MTK_MT6323_ISINK_EN_CTRL,
			   ISINK_CH_EN_MASK(led->id),
			   ISINK_CH_EN(led->id));

	regmap_update_bits(regmap, MTK_MT6323_ISINK_CON2(led->id),
			   ISINK_CH_STEP_MASK,
			   ISINK_CH_STEP(1));

	regmap_update_bits(regmap, MTK_MT6323_ISINK_CON0(led->id),
			   ISINK_DIM_DUTY_MASK, ISINK_DIM_DUTY(31));

	regmap_update_bits(regmap, MTK_MT6323_ISINK_CON1(led->id),
			   ISINK_DIM_FSEL_MASK, ISINK_DIM_FSEL(1000));

	led->current_brightness = 1;

	dev_dbg(leds->dev, "%s called for led%d\n",
		__func__, led->id);
}

static int mtk_led_set_blink(struct led_classdev *cdev,
			     unsigned long *delay_on,
			     unsigned long *delay_off)
{
	struct mtk_led *led = container_of(cdev, struct mtk_led, cdev);
	struct mtk_leds *leds = led->parent;
	struct regmap *regmap = leds->hw->regmap;
	u16 period;
	u8 duty_cycle, duty_hw;

	/* Units are in ms , if over the hardware able
	 * to support, fallback into software blink
	 */
	if (*delay_on + *delay_off > MTK_MAX_PERIOD)
		return -EINVAL;

	/* LED subsystem requires a default user
	 * friendly blink pattern for the LED so using
	 * 1Hz duty cycle 50% here if without specific
	 * value delay_on and delay off being assigned
	 */
	if (*delay_on == 0 && *delay_off == 0) {
		*delay_on = 500;
		*delay_off = 500;
	}

	period = *delay_on + *delay_off;

	/* duty_cycle is the percentage of period during
	 * which the led is ON
	 */
	duty_cycle = 100 * (*delay_on) / period;

	mutex_lock(&leds->lock);

	if (!led->current_brightness)
		mtk_led_hw_on(cdev);

	duty_hw = DIV_ROUND_CLOSEST(duty_cycle * 1000, 3125);
	regmap_update_bits(regmap, MTK_MT6323_ISINK_CON0(led->id),
			   ISINK_DIM_DUTY_MASK, ISINK_DIM_DUTY(duty_hw));

	regmap_update_bits(regmap, MTK_MT6323_ISINK_CON1(led->id),
			   ISINK_DIM_FSEL_MASK, ISINK_DIM_FSEL(period - 1));

	mutex_unlock(&leds->lock);

	dev_dbg(leds->dev, "%s: Hardware blink! period=%dms duty=%d for led%d\n",
		__func__, period, duty_cycle, led->id);

	return 0;
}

static int mtk_led_set_brightness(struct led_classdev *cdev,
				  enum led_brightness brightness)
{
	struct mtk_led *led = container_of(cdev, struct mtk_led, cdev);
	struct mtk_leds *leds = led->parent;
	struct regmap *regmap = leds->hw->regmap;

	mutex_lock(&leds->lock);

	if (!led->current_brightness && brightness)
		mtk_led_hw_on(cdev);

	if (brightness) {
		/* Setup current output for the corresponding
		 * brightness level
		 */
		regmap_update_bits(regmap, MTK_MT6323_ISINK_CON2(led->id),
				   ISINK_CH_STEP_MASK,
				   ISINK_CH_STEP(brightness - 1));

		regmap_update_bits(regmap, MTK_MT6323_ISINK_CON2(led->id),
				   ISINK_SFSTR0_TC_MASK | ISINK_SFSTR0_EN_MASK,
				   ISINK_SFSTR0_TC(2) | ISINK_SFSTR0_EN);

		dev_dbg(leds->dev, "Update led brightness:%d\n",
			brightness);
	}

	if (!brightness)
		mtk_led_hw_off(cdev);
	led->current_brightness = brightness;

	mutex_unlock(&leds->lock);

	return 0;
}

static int mt6323_led_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child;
	struct mt6397_chip *hw = dev_get_drvdata(pdev->dev.parent);
	struct mtk_leds *leds;
	int ret, i = 0, count;
	const char *state;
	unsigned int status;

	count = of_get_child_count(np);
	if (!count)
		return -ENODEV;

	/* The number the LEDs on MT6323 could be support is
	 * up to MTK_MAX_DEVICES
	 */
	count = (count <= MTK_MAX_DEVICES) ? count : MTK_MAX_DEVICES;

	leds = devm_kzalloc(dev, sizeof(struct mtk_leds) +
			    sizeof(struct mtk_led) * count,
			    GFP_KERNEL);
	if (!leds)
		return -ENOMEM;

	platform_set_drvdata(pdev, leds);
	leds->dev = dev;

	/* leds->hw points to the underlying bus for the register
	 * controlled
	 */
	leds->hw = hw;
	mutex_init(&leds->lock);
	leds->led_num = count;

	status = RG_DRV_32K_CK_PDN;
	regmap_update_bits(leds->hw->regmap, MTK_MT6323_TOP_CKPDN0,
			   RG_DRV_32K_CK_PDN_MASK, ~status);

	/* Waiting for 32K stable prior to give the default value
	 * to each LED state decided through these useful common
	 * propertys such as label, linux,default-trigger and
	 * default-state
	 */
	usleep_range(300, 500);

	for_each_available_child_of_node(np, child) {
		leds->led[i].cdev.name =
			of_get_property(child, "label", NULL) ? :
					child->name;
		leds->led[i].cdev.default_trigger = of_get_property(child,
						    "linux,default-trigger",
						    NULL);
		leds->led[i].cdev.max_brightness = MTK_MAX_BRIGHTNESS;
		leds->led[i].cdev.brightness_set_blocking =
					mtk_led_set_brightness;
		leds->led[i].cdev.blink_set = mtk_led_set_blink;
		leds->led[i].id = i;
		leds->led[i].parent = leds;
		state = of_get_property(child, "default-state", NULL);
		if (state) {
			if (!strcmp(state, "keep")) {
				leds->led[i].current_brightness =
				get_mtk_led_hw_brightness(&leds->led[i].cdev);
			} else if (!strcmp(state, "on")) {
				mtk_led_set_brightness(&leds->led[i].cdev, 1);
			} else  {
				mtk_led_set_brightness(&leds->led[i].cdev,
						       0);
			}
		}
		ret = devm_led_classdev_register(dev, &leds->led[i].cdev);
		if (ret) {
			dev_err(&pdev->dev, "Failed to register LED: %d\n",
				ret);
			return ret;
		}
		leds->led[i].cdev.dev->of_node = child;
		i++;
	}

	return 0;
}

static int mt6323_led_remove(struct platform_device *pdev)
{
	struct mtk_leds *leds = platform_get_drvdata(pdev);
	int i;

	/* Turned the LED to OFF state if driver removal */
	for (i = 0 ; i < leds->led_num ; i++)
		mtk_led_hw_off(&leds->led[i].cdev);

	regmap_update_bits(leds->hw->regmap, MTK_MT6323_TOP_CKPDN0,
			   RG_DRV_32K_CK_PDN_MASK, RG_DRV_32K_CK_PDN);
	return 0;
}

static const struct of_device_id mt6323_led_dt_match[] = {
	{ .compatible = "mediatek,mt6323-led" },
	{},
};
MODULE_DEVICE_TABLE(of, mt6323_led_dt_match);

static struct platform_driver mt6323_led_driver = {
	.probe		= mt6323_led_probe,
	.remove		= mt6323_led_remove,
	.driver		= {
		.name	= "mt6323-led",
		.of_match_table = mt6323_led_dt_match,
	},
};

module_platform_driver(mt6323_led_driver);

MODULE_DESCRIPTION("LED driver for Mediatek MT6323 PMIC");
MODULE_AUTHOR("Sean Wang <sean.wang@mediatek.com>");
MODULE_LICENSE("GPL v2");
