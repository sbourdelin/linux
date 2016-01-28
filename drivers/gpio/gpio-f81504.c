/*
 * Copyright (C) 2016 Fintek Corporation
 * Based on gpio-mpc8xxx.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/pci.h>
#include <linux/mfd/f81504.h>

struct f81504_gpio_chip {
	struct gpio_chip chip;
	struct mutex locker;
	u8 idx;
	u8 save_out_en;
	u8 save_drive_en;
	u8 save_value;
};

static struct f81504_gpio_chip *gpio_to_f81504_chip(struct gpio_chip *chip)
{
	return container_of(chip, struct f81504_gpio_chip, chip);
}

static int f81504_gpio_direction_in(struct gpio_chip *chip, unsigned offset)
{
	u8 tmp;
	struct f81504_gpio_chip *gc = gpio_to_f81504_chip(chip);
	struct platform_device *pdev = to_platform_device(chip->dev);
	struct pci_dev *pci_dev = to_pci_dev(pdev->dev.parent);

	mutex_lock(&gc->locker);

	/* set input mode */
	pci_read_config_byte(pci_dev, F81504_GPIO_START_ADDR + gc->idx *
			F81504_GPIO_SET_OFFSET + F81504_GPIO_OUT_EN_OFFSET,
			&tmp);
	pci_write_config_byte(pci_dev, F81504_GPIO_START_ADDR + gc->idx *
			F81504_GPIO_SET_OFFSET + F81504_GPIO_OUT_EN_OFFSET,
			tmp & ~BIT(offset));

	mutex_unlock(&gc->locker);
	return 0;
}

static int f81504_gpio_direction_out(struct gpio_chip *chip, unsigned offset,
		int value)
{
	u8 tmp;
	struct f81504_gpio_chip *gc = gpio_to_f81504_chip(chip);
	struct platform_device *pdev = to_platform_device(chip->dev);
	struct pci_dev *pci_dev = to_pci_dev(pdev->dev.parent);
	struct f81504_pci_private *priv = pci_get_drvdata(pci_dev);

	mutex_lock(&gc->locker);

	/* set output mode */
	pci_read_config_byte(pci_dev, F81504_GPIO_START_ADDR + gc->idx *
			F81504_GPIO_SET_OFFSET + F81504_GPIO_OUT_EN_OFFSET,
			&tmp);
	pci_write_config_byte(pci_dev, F81504_GPIO_START_ADDR + gc->idx *
			F81504_GPIO_SET_OFFSET + F81504_GPIO_OUT_EN_OFFSET,
			tmp | BIT(offset));

	/*
	 * The GPIO default driven mode for this device is open-drain. The
	 * GPIOLIB had no change GPIO mode API currently. So we leave the
	 * Push-Pull code below.
	 *
	 * pci_read_config_byte(dev, GPIO_START_ADDR + idx * GPIO_SET_OFFSET +
	 *			GPIO_DRIVE_EN_OFFSET, &tmp);
	 * pci_write_config_byte(dev, GPIO_START_ADDR + idx * GPIO_SET_OFFSET +
	 *			GPIO_DRIVE_EN_OFFSET, tmp | BIT(gpio_num));
	 */

	/* set output data */
	tmp = inb(priv->gpio_ioaddr + gc->idx);

	if (value)
		outb(tmp | BIT(offset), priv->gpio_ioaddr + gc->idx);
	else
		outb(tmp & ~BIT(offset), priv->gpio_ioaddr + gc->idx);

	mutex_unlock(&gc->locker);
	return 0;
}

static int f81504_gpio_get_direction(struct gpio_chip *chip, unsigned offset)
{
	u8 tmp;
	struct f81504_gpio_chip *gc = gpio_to_f81504_chip(chip);
	struct platform_device *pdev = to_platform_device(chip->dev);
	struct pci_dev *pci_dev = to_pci_dev(pdev->dev.parent);

	mutex_lock(&gc->locker);
	pci_read_config_byte(pci_dev, F81504_GPIO_START_ADDR + gc->idx *
			F81504_GPIO_SET_OFFSET, &tmp);
	mutex_unlock(&gc->locker);

	if (tmp & BIT(offset))
		return GPIOF_DIR_OUT;

	return GPIOF_DIR_IN;
}

static int f81504_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	int tmp;
	struct f81504_gpio_chip *gc = gpio_to_f81504_chip(chip);
	struct platform_device *pdev = to_platform_device(chip->dev);
	struct pci_dev *pci_dev = to_pci_dev(pdev->dev.parent);
	struct f81504_pci_private *priv = pci_get_drvdata(pci_dev);

	mutex_lock(&gc->locker);
	tmp = inb(priv->gpio_ioaddr + gc->idx);
	mutex_unlock(&gc->locker);

	return !!(tmp & BIT(offset));
}

static void f81504_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	f81504_gpio_direction_out(chip, offset, value);
}

static int f81504_gpio_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct pci_dev *pci_dev = to_pci_dev(pdev->dev.parent);
	struct f81504_pci_private *priv = pci_get_drvdata(pci_dev);
	struct f81504_gpio_chip *gc = platform_get_drvdata(pdev);

	mutex_lock(&gc->locker);
	pci_read_config_byte(pci_dev, F81504_GPIO_START_ADDR + gc->idx *
			F81504_GPIO_SET_OFFSET + F81504_GPIO_OUT_EN_OFFSET,
			&gc->save_out_en);

	pci_read_config_byte(pci_dev, F81504_GPIO_START_ADDR + gc->idx *
			F81504_GPIO_SET_OFFSET + F81504_GPIO_DRIVE_EN_OFFSET,
			&gc->save_drive_en);

	gc->save_value = inb(priv->gpio_ioaddr + gc->idx);
	mutex_unlock(&gc->locker);
	return 0;
}

static int f81504_gpio_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct pci_dev *pci_dev = to_pci_dev(pdev->dev.parent);
	struct f81504_pci_private *priv = pci_get_drvdata(pci_dev);
	struct f81504_gpio_chip *gc = platform_get_drvdata(pdev);

	mutex_lock(&gc->locker);
	pci_write_config_byte(pci_dev, F81504_GPIO_START_ADDR + gc->idx *
			F81504_GPIO_SET_OFFSET + F81504_GPIO_OUT_EN_OFFSET,
			gc->save_out_en);

	pci_write_config_byte(pci_dev, F81504_GPIO_START_ADDR + gc->idx *
			F81504_GPIO_SET_OFFSET + F81504_GPIO_DRIVE_EN_OFFSET,
			gc->save_drive_en);

	outb(gc->save_value, priv->gpio_ioaddr + gc->idx);
	mutex_unlock(&gc->locker);
	return 0;
}

static int f81504_gpio_probe(struct platform_device *pdev)
{
	int status;
	struct f81504_gpio_chip *gc;
	void *data = dev_get_platdata(&pdev->dev);
	u8 gpio_idx = *(u8 *)data;
	char *name;

	if (gpio_idx >= ARRAY_SIZE(fintek_gpio_mapping)) {
		dev_err(&pdev->dev, "%s: gpio_idx:%d out of range.\n",
				__func__, gpio_idx);
		return -ENODEV;
	}

	gc = devm_kzalloc(&pdev->dev, sizeof(*gc), GFP_KERNEL);
	if (!gc)
		return -ENOMEM;

	kfree(data);
	mutex_init(&gc->locker);
	platform_set_drvdata(pdev, gc);

	name = devm_kzalloc(&pdev->dev, FINTEK_GPIO_NAME_LEN, GFP_KERNEL);
	if (!name)
		return -ENOMEM;

	/* This will display like as GPIO-1x */
	sprintf(name, "%s-%dx", FINTEK_GPIO_DISPLAY, gpio_idx);

	gc->chip.owner = THIS_MODULE;
	gc->chip.label = name;
	gc->chip.ngpio = 8;
	gc->chip.dev = &pdev->dev;
	gc->chip.get = f81504_gpio_get;
	gc->chip.set = f81504_gpio_set;
	gc->chip.direction_input = f81504_gpio_direction_in;
	gc->chip.direction_output = f81504_gpio_direction_out;
	gc->chip.get_direction = f81504_gpio_get_direction;
	gc->chip.can_sleep = 1;
	gc->chip.base = -1;
	gc->idx = gpio_idx;

	status = gpiochip_add(&gc->chip);
	if (status) {
		dev_err(&pdev->dev, "%s: gpiochip_add failed: %d\n", __func__,
				status);
		return -ENOMEM;
	}

	return 0;
}

static int f81504_gpio_remove(struct platform_device *pdev)
{
	struct f81504_gpio_chip *gc = platform_get_drvdata(pdev);

	gpiochip_remove(&gc->chip);
	return 0;
}

static SIMPLE_DEV_PM_OPS(f81504_gpio_pm_ops, f81504_gpio_suspend,
		f81504_gpio_resume);

static struct platform_driver f81504_gpio_driver = {
	.driver = {
		.name	= F81504_GPIO_NAME,
		.owner	= THIS_MODULE,
		.pm     = &f81504_gpio_pm_ops,
	},
	.probe		= f81504_gpio_probe,
	.remove		= f81504_gpio_remove,
};

module_platform_driver(f81504_gpio_driver);

MODULE_AUTHOR("Peter Hong <Peter_Hong@fintek.com.tw>");
MODULE_DESCRIPTION("Fintek F81504/508/512 PCIE GPIOLIB driver");
MODULE_LICENSE("GPL");
