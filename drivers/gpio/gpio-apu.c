/* PC Engines APU2/APU3 GPIO device driver
 *
 * Copyright (C) 2018 Florian Eckert <fe@dev.tdt.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * This program is distributed in the hope that it will be useful
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>
 */

#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio_keys.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define DEVNAME                "gpio-apu"

#define APU_FCH_ACPI_MMIO_BASE 0xFED80000
#define APU_FCH_GPIO_BASE      (APU_FCH_ACPI_MMIO_BASE + 0x1500)
#define APU_GPIO_BIT_WRITE     22
#define APU_GPIO_BIT_READ      16
#define APU_GPIO_BIT_DIR       23
#define APU_IOSIZE             sizeof(u32)

#define APU2_NUM_GPIO          1
#define APU3_NUM_GPIO          2

struct apu_gpio_pdata {
	struct platform_device *pdev;
	struct gpio_chip *chip;
	unsigned long *offset;
	void __iomem **addr;
	int iosize; /* for devm_ioremap() */
	spinlock_t lock;
};

static struct apu_gpio_pdata *apu_gpio;
static struct platform_device *keydev;

/* APU2 */
static unsigned long apu2_gpio_offset[APU2_NUM_GPIO] = {
	APU_FCH_GPIO_BASE + 89 * APU_IOSIZE, //KEY
};
static void __iomem *apu2_gpio_addr[APU2_NUM_GPIO] = {NULL};

/* APU3 */
static unsigned long apu3_gpio_offset[APU3_NUM_GPIO] = {
	APU_FCH_GPIO_BASE + 89 * APU_IOSIZE, //KEY
	APU_FCH_GPIO_BASE + 90 * APU_IOSIZE, //SIM
};
static void __iomem *apu3_gpio_addr[APU3_NUM_GPIO] = {NULL, NULL};

static int gpio_apu_get_dir (struct gpio_chip *chip, unsigned offset)
{
	u32 val;

	spin_lock(&apu_gpio->lock);

	val = ~ioread32(apu_gpio->addr[offset]);
	val = (val >> APU_GPIO_BIT_DIR) & 1;

	spin_unlock(&apu_gpio->lock);

	return val;
}

static int gpio_apu_dir_in (struct gpio_chip *chip, unsigned offset)
{
	u32 val;

	spin_lock(&apu_gpio->lock);

	val = ioread32(apu_gpio->addr[offset]);
	val &= ~BIT(APU_GPIO_BIT_DIR);
	iowrite32(val, apu_gpio->addr[offset]);

	spin_unlock(&apu_gpio->lock);

	return 0;
}

static int gpio_apu_dir_out (struct gpio_chip *chip, unsigned offset,
		int value)
{
	u32 val;

	spin_lock(&apu_gpio->lock);

	val = ioread32(apu_gpio->addr[offset]);
	val |= BIT(APU_GPIO_BIT_DIR);
	iowrite32(val, apu_gpio->addr[offset]);

	spin_unlock(&apu_gpio->lock);

	return 0;
}

static int gpio_apu_get_data (struct gpio_chip *chip, unsigned offset)
{
	u32 val;

	spin_lock(&apu_gpio->lock);

	val = ioread32(apu_gpio->addr[offset]);
	val = (val >> APU_GPIO_BIT_READ) & 1;

	spin_unlock(&apu_gpio->lock);

	return val;
}

static void gpio_apu_set_data (struct gpio_chip *chip, unsigned offset, int value)
{
	u32 val;

	spin_lock(&apu_gpio->lock);

	val = ioread32(apu_gpio->addr[offset]);
	if (value)
		val |= BIT(APU_GPIO_BIT_WRITE);
	else
		val &= ~BIT(APU_GPIO_BIT_WRITE);
	iowrite32(val, apu_gpio->addr[offset]);

	spin_unlock(&apu_gpio->lock);
}

static const struct dmi_system_id apu_gpio_dmi_table[] __initconst = {
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
	/* PC Engines APU3 with "Legancy" bios >= 4.0.7 */
	{
		.ident = "apu2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "PC Engines"),
			DMI_MATCH(DMI_BOARD_NAME, "APU3")
		}
	},
	{}
};
MODULE_DEVICE_TABLE(dmi, apu_gpio_dmi_table);

static struct gpio_chip gpio_apu_chip = {
	.label = "gpio-apu",
	.owner = THIS_MODULE,
	.base = -1,
	.get_direction = gpio_apu_get_dir,
	.direction_input = gpio_apu_dir_in,
	.direction_output = gpio_apu_dir_out,
	.get = gpio_apu_get_data,
	.set = gpio_apu_set_data,
};

static struct gpio_keys_button apu_gpio_keys[] = {
	{
		.desc           = "Reset button",
		.type           = EV_KEY,
		.code           = KEY_RESTART,
		.debounce_interval = 60,
		.gpio           = 510,
		.active_low     = 1,
	},
};

static void register_gpio_keys_polled(int id, unsigned poll_interval,
				      unsigned nbuttons,
				      struct gpio_keys_button *buttons)
{
	struct gpio_keys_platform_data pdata = { };
	int err;

	keydev = platform_device_alloc("gpio-keys-polled", id);
	if (!keydev) {
		printk(KERN_ERR "Failed to allocate gpio-keys platform device\n");
		return;
	}

	pdata.poll_interval = poll_interval;
	pdata.nbuttons = nbuttons;
	pdata.buttons = buttons;

	err = platform_device_add_data(keydev, &pdata, sizeof(pdata));
	if (err) {
		dev_err(&keydev->dev, "failed to add platform data to key driver (%d)", err);
		goto err_put_pdev;
	}

	err = platform_device_add(keydev);
	if (err) {
		dev_err(&keydev->dev, "failed to register key platform device (%d)", err);
		goto err_put_pdev;
	}

	return;

err_put_pdev:
	platform_device_put(keydev);
	keydev = NULL;
}

static int __init apu_gpio_probe(struct platform_device *pdev)
{
	int i;
	int ret;

	apu_gpio = devm_kzalloc(&pdev->dev, sizeof(*apu_gpio), GFP_KERNEL);

	if (!apu_gpio)
		return -ENOMEM;

	apu_gpio->pdev = pdev;
	apu_gpio->chip = &gpio_apu_chip;
	spin_lock_init(&apu_gpio->lock);

	if (dmi_match(DMI_PRODUCT_NAME, "APU3")) {
		apu_gpio->offset = apu3_gpio_offset;
		apu_gpio->addr = apu3_gpio_addr;
		apu_gpio->iosize = APU_IOSIZE;
		apu_gpio->chip->ngpio = ARRAY_SIZE(apu3_gpio_offset);
		for( i = 0; i < ARRAY_SIZE(apu3_gpio_offset); i++) {
			apu3_gpio_addr[i] = devm_ioremap(&pdev->dev,
					apu_gpio->offset[i], apu_gpio->iosize);
			if (!apu3_gpio_addr[i]) {
				return -ENOMEM;
			}
		}
	} else if (dmi_match(DMI_BOARD_NAME, "APU2") ||
		   dmi_match(DMI_BOARD_NAME, "apu2") ||
		   dmi_match(DMI_BOARD_NAME, "PC Engines apu2")) {
		apu_gpio->offset = apu2_gpio_offset;
		apu_gpio->addr = apu2_gpio_addr;
		apu_gpio->iosize = APU_IOSIZE;
		apu_gpio->chip->ngpio = ARRAY_SIZE(apu2_gpio_offset);
		for( i = 0; i < ARRAY_SIZE(apu2_gpio_offset); i++) {
			apu2_gpio_addr[i] = devm_ioremap(&pdev->dev,
					apu_gpio->offset[i], apu_gpio->iosize);
			if (!apu2_gpio_addr[i]) {
				return -ENOMEM;
			}
		}
	}

	ret = gpiochip_add(&gpio_apu_chip);
	if (ret) {
		pr_err("Adding gpiochip failed\n");
	}

	register_gpio_keys_polled(-1, 20, ARRAY_SIZE(apu_gpio_keys), apu_gpio_keys);

	return ret;
}

static struct platform_driver apu_gpio_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
	},
};

static int __init apu_gpio_init(void)
{
	struct platform_device *pdev;
	int err;

	if (!dmi_match(DMI_SYS_VENDOR, "PC Engines")) {
		pr_err("No PC Engines board detected\n");
		return -ENODEV;
	}
	if (!(dmi_match(DMI_PRODUCT_NAME, "APU") ||
	      dmi_match(DMI_PRODUCT_NAME, "APU2") ||
	      dmi_match(DMI_PRODUCT_NAME, "APU3") ||
	      dmi_match(DMI_PRODUCT_NAME, "apu2") ||
	      dmi_match(DMI_PRODUCT_NAME, "PC Engines apu2"))) {
		pr_err("Unknown PC Engines board: %s\n",
				dmi_get_system_info(DMI_PRODUCT_NAME));
		return -ENODEV;
	}

	pdev = platform_device_register_simple(KBUILD_MODNAME, -1, NULL, 0);
	if (IS_ERR(pdev)) {
		pr_err("Device allocation failed\n");
		return PTR_ERR(pdev);
	}

	err = platform_driver_probe(&apu_gpio_driver, apu_gpio_probe);
	if (err) {
		pr_err("Probe platform driver failed\n");
		platform_device_unregister(pdev);
	}

	pr_info ("%s: APU2/3 GPIO driver module loaded\n", DEVNAME);

	return err;
}

static void __exit apu_gpio_exit(void)
{
	platform_device_unregister(keydev);
	gpiochip_remove(apu_gpio->chip);
	platform_device_unregister(apu_gpio->pdev);
	platform_driver_unregister(&apu_gpio_driver);
	pr_info ("%s: APU2/3 GPIO driver module unloaded\n", DEVNAME);
}

module_init(apu_gpio_init);
module_exit(apu_gpio_exit);

MODULE_AUTHOR("Florian Eckert <fe@dev.tdt.de>");
MODULE_DESCRIPTION("PC Engines APU2/APU3 family GPIO driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:gpio_apu");
