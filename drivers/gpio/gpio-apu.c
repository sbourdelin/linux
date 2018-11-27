// SPDX-License-Identifier: GPL-2.0
/* PC Engines APU2/APU3 GPIO device driver
 *
 * Copyright (C) 2018 Florian Eckert <fe@dev.tdt.de>
 */

#include <linux/bits.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define APU_FCH_ACPI_MMIO_BASE 0xFED80000
#define APU_FCH_GPIO_BASE      (APU_FCH_ACPI_MMIO_BASE + 0x1500)
#define APU_GPIO_BIT_RD        16
#define APU_GPIO_BIT_WR        22
#define APU_GPIO_BIT_DIR       23

struct apu_gpio_pdata {
	struct gpio_chip chip;
	unsigned long *offset;		/* base register offset */
	void __iomem **addr;		/* remapped iomem addresses */
	spinlock_t lock;		/* lock register access */
};

static struct platform_device *apu_gpio_pdev;

/* APU2 */
static unsigned long apu2_gpio_offset[] = {
	APU_FCH_GPIO_BASE + 89 * sizeof(u32),
	APU_FCH_GPIO_BASE + 67 * sizeof(u32),
	APU_FCH_GPIO_BASE + 66 * sizeof(u32),
};
static const char * const apu2_gpio_names[] = {
	"button_reset",
	"mpcie2_reset",
	"mpcie3_reset",
};

/* APU3 */
static unsigned long apu3_gpio_offset[] = {
	APU_FCH_GPIO_BASE + 89 * sizeof(u32),
	APU_FCH_GPIO_BASE + 67 * sizeof(u32),
	APU_FCH_GPIO_BASE + 66 * sizeof(u32),
	APU_FCH_GPIO_BASE + 90 * sizeof(u32),
};
static const char * const apu3_gpio_names[] = {
	"button_reset",
	"mpcie2_reset",
	"mpcie3_reset",
	"simswap",
};

static int gpio_apu_get_dir(struct gpio_chip *chip, unsigned int offset)
{
	u32 val;
	struct apu_gpio_pdata *apu_gpio = gpiochip_get_data(chip);

	spin_lock(&apu_gpio->lock);

	val = ~ioread32(apu_gpio->addr[offset]);

	spin_unlock(&apu_gpio->lock);

	return !!(val & BIT(APU_GPIO_BIT_DIR));
}

static int gpio_apu_dir_in(struct gpio_chip *chip, unsigned int offset)
{
	u32 val;
	struct apu_gpio_pdata *apu_gpio = gpiochip_get_data(chip);

	spin_lock(&apu_gpio->lock);

	val = ioread32(apu_gpio->addr[offset]);
	val &= ~BIT(APU_GPIO_BIT_DIR);
	iowrite32(val, apu_gpio->addr[offset]);

	spin_unlock(&apu_gpio->lock);

	return 0;
}

static int gpio_apu_dir_out(struct gpio_chip *chip, unsigned int offset,
		int value)
{
	u32 val;
	struct apu_gpio_pdata *apu_gpio = gpiochip_get_data(chip);

	spin_lock(&apu_gpio->lock);

	val = ioread32(apu_gpio->addr[offset]);
	val |= BIT(APU_GPIO_BIT_DIR);
	iowrite32(val, apu_gpio->addr[offset]);

	spin_unlock(&apu_gpio->lock);

	return 0;
}

static int gpio_apu_get_data(struct gpio_chip *chip, unsigned int offset)
{
	u32 val;
	struct apu_gpio_pdata *apu_gpio = gpiochip_get_data(chip);

	spin_lock(&apu_gpio->lock);

	val = ioread32(apu_gpio->addr[offset]);

	spin_unlock(&apu_gpio->lock);

	return !!(val & BIT(APU_GPIO_BIT_RD));
}

static void gpio_apu_set_data(struct gpio_chip *chip, unsigned int offset,
		int value)
{
	u32 val;
	struct apu_gpio_pdata *apu_gpio = gpiochip_get_data(chip);

	spin_lock(&apu_gpio->lock);

	val = ioread32(apu_gpio->addr[offset]);
	if (value)
		val |= BIT(APU_GPIO_BIT_WR);
	else
		val &= ~BIT(APU_GPIO_BIT_WR);
	iowrite32(val, apu_gpio->addr[offset]);

	spin_unlock(&apu_gpio->lock);
}

static const struct dmi_system_id apu2_gpio_dmi_table[] __initconst = {
	/* PC Engines APU2 with "Legacy" bios < 4.0.8 */
	{
		.ident = "apu2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "PC Engines"),
			DMI_MATCH(DMI_BOARD_NAME, "APU2")
		}
	},
	/* PC Engines APU2 with "Legacy" bios >= 4.0.8 */
	{
		.ident = "apu2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "PC Engines"),
			DMI_MATCH(DMI_BOARD_NAME, "apu2")
		}
	},
	/* PC Engines APU2 with "Mainline" bios */
	{
		.ident = "apu2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "PC Engines"),
			DMI_MATCH(DMI_BOARD_NAME, "PC Engines apu2")
		}
	},
	{}
}
MODULE_DEVICE_TABLE(dmi, apu2_gpio_dmi_table);

static const struct dmi_system_id apu3_gpio_dmi_table[] __initconst = {
	/* PC Engines APU3 with "Legacy" bios < 4.0.8 */
	{
		.ident = "apu3",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "PC Engines"),
			DMI_MATCH(DMI_BOARD_NAME, "APU3")
		}
	},
	/* PC Engines APU3 with "Legacy" bios >= 4.0.8 */
	{
		.ident = "apu3",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "PC Engines"),
			DMI_MATCH(DMI_BOARD_NAME, "apu3")
		}
	},
	/* PC Engines APU3 with "Mainline" bios */
	{
		.ident = "apu3",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "PC Engines"),
			DMI_MATCH(DMI_BOARD_NAME, "PC Engines apu3")
		}
	},
	{}
};
MODULE_DEVICE_TABLE(dmi, apu3_gpio_dmi_table);

static int __init apu_gpio_probe(struct platform_device *pdev)
{
	unsigned int i;
	struct apu_gpio_pdata *apu_gpio;

	apu_gpio = devm_kzalloc(&pdev->dev, sizeof(*apu_gpio), GFP_KERNEL);
	if (!apu_gpio)
		return -ENOMEM;

	platform_set_drvdata(pdev, apu_gpio);
	spin_lock_init(&apu_gpio->lock);

	apu_gpio->chip.label = KBUILD_MODNAME;
	apu_gpio->chip.base = 20;
	apu_gpio->chip.get_direction = gpio_apu_get_dir;
	apu_gpio->chip.direction_input = gpio_apu_dir_in;
	apu_gpio->chip.direction_output = gpio_apu_dir_out;
	apu_gpio->chip.get = gpio_apu_get_data;
	apu_gpio->chip.set = gpio_apu_set_data;

	if (dmi_check_system(apu3_gpio_dmi_table)) {
		apu_gpio->addr = devm_kzalloc(&pdev->dev,
				sizeof(apu3_gpio_offset),
				GFP_KERNEL);

		if (!apu_gpio->addr)
			return -ENOMEM;

		apu_gpio->offset = apu3_gpio_offset;
		apu_gpio->chip.names = apu3_gpio_names;
		apu_gpio->chip.ngpio = ARRAY_SIZE(apu3_gpio_offset);
		for (i = 0; i < ARRAY_SIZE(apu3_gpio_offset); i++) {
			apu_gpio->addr[i] = devm_ioremap(&pdev->dev,
					apu_gpio->offset[i], sizeof(u32));
			if (!apu_gpio->addr[i])
				return -ENOMEM;
		}
	} else if (dmi_check_system(apu2_gpio_dmi_table)) {
		apu_gpio->addr = devm_kzalloc(&pdev->dev,
				sizeof(apu2_gpio_offset),
				GFP_KERNEL);

		if (!apu_gpio->addr)
			return -ENOMEM;

		apu_gpio->offset = apu2_gpio_offset;
		apu_gpio->chip.names = apu2_gpio_names;
		apu_gpio->chip.ngpio = ARRAY_SIZE(apu2_gpio_offset);
		for (i = 0; i < ARRAY_SIZE(apu2_gpio_offset); i++) {
			apu_gpio->addr[i] = devm_ioremap(&pdev->dev,
					apu_gpio->offset[i], sizeof(u32));
			if (!apu_gpio->addr[i])
				return -ENOMEM;
		}
	}

	return devm_gpiochip_add_data(&pdev->dev, &apu_gpio->chip, apu_gpio);
}

static struct platform_driver apu_gpio_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
	},
	.probe = apu_gpio_probe,
};

static int __init apu_gpio_init(void)
{
	if (!(dmi_check_system(apu2_gpio_dmi_table)) &&
		!(dmi_check_system(apu3_gpio_dmi_table))) {
		pr_err("No PC Engines board detected\n");
		return -ENODEV;
	}

	apu_gpio_pdev = platform_device_register_simple(KBUILD_MODNAME,
			-1, NULL, 0);
	if (IS_ERR(apu_gpio_pdev))
		return PTR_ERR(apu_gpio_pdev);


	return platform_driver_register(&apu_gpio_driver);
}

static void __exit apu_gpio_exit(void)
{
	platform_device_unregister(apu_gpio_pdev);
	platform_driver_unregister(&apu_gpio_driver);
}

module_init(apu_gpio_init);
module_exit(apu_gpio_exit);

MODULE_AUTHOR("Florian Eckert");
MODULE_DESCRIPTION("PC Engines APU2/APU3 family GPIO driver");
MODULE_LICENSE("GPL v2");
