// SPDX-License-Identifier: GPL-2.0
/*
 * Motorola Mapphone MDM6600 modem GPIO controlled USB PHY driver
 * Copyright (C) 2018 Tony Lindgren <tony@atomide.com>
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <linux/gpio/consumer.h>
#include <linux/of_platform.h>
#include <linux/phy/phy.h>
#include <linux/usb/phy_companion.h>

#define PHY_MDM6600_STARTUP_DELAY_MS	3000	/* A least 2.2s usually */

/*
 * MDM6600 status codes. These are copied from Motorola Mapphone Linux
 * kernel tree. The BB naming here refers to "BaseBand" for modem.
 */
enum phy_mdm6600_status {
	BP_STATUS_PANIC,		/* Seems to be really off state */
	BP_STATUS_PANIC_BUSY_WAIT,
	BP_STATUS_QC_DLOAD,
	BP_STATUS_RAM_DOWNLOADER,	/* MDM6600 USB flashing mode */
	BP_STATUS_PHONE_CODE_AWAKE,	/* MDM6600 normal USB mode */
	BP_STATUS_PHONE_CODE_ASLEEP,
	BP_STATUS_SHUTDOWN_ACK,
	BP_STATUS_UNDEFINED,
};

static const char * const
phy_mdm6600_status_name[] = {
	"off", "busy", "qc_dl", "ram_dl", "awake",
	"asleep", "shutdown", "undefined",
};

/*
 * MDM6600 command codes. These are copied from Motorola Mapphone Linux
 * kernel tree. The AP naming here refers to "Application Processor".
 */
enum phy_mdm6600_cmd {
	AP_STATUS_BP_PANIC_ACK,
	AP_STATUS_DATA_ONLY_BYPASS,	/* Reroute USB to CPCAP PHY */
	AP_STATUS_FULL_BYPASS,		/* Reroute USB to CPCAP PHY */
	AP_STATUS_NO_BYPASS,		/* Request normal start-up mode */
	AP_STATUS_BP_SHUTDOWN_REQ,	/* Request device power off */
	AP_STATUS_BP_UNKNOWN_5,
	AP_STATUS_BP_UNKNOWN_6,
	AP_STATUS_UNDEFINED,
};

enum phy_mdm6600_lines {
	PHY_MDM6600_ENABLE,		/* USB PHY enable */
	PHY_MDM6600_POWER,		/* Device power */
	PHY_MDM6600_RESET,		/* Device reset */
	PHY_MDM6600_MODE0,		/* USB boot mode flashing vs normal */
	PHY_MDM6600_MODE1,		/* USB boot mode flashing vs normal */
	PHY_MDM6600_STATUS0,		/* Device state */
	PHY_MDM6600_STATUS1,		/* Device state */
	PHY_MDM6600_STATUS2,		/* Device state */
	PHY_MDM6600_CMD0,		/* Device command */
	PHY_MDM6600_CMD1,		/* Device command */
	PHY_MDM6600_CMD2,		/* Device command */
	PHY_MDM6600_NR_LINES,
};

struct phy_mdm6600 {
	struct device *dev;
	struct usb_phy phy;
	struct phy *generic_phy;
	struct phy_provider *phy_provider;
	struct gpio_desc *gpio[PHY_MDM6600_NR_LINES];
	struct delayed_work bootup_work;
	struct delayed_work status_work;
	struct completion ack;
	bool enabled;
	int status;
};

static int phy_mdm6600_init(struct phy *x)
{
	struct phy_mdm6600 *ddata = phy_get_drvdata(x);
	struct gpio_desc *enable_gpio = ddata->gpio[PHY_MDM6600_ENABLE];

	if (!ddata->enabled)
		return -EPROBE_DEFER;

	gpiod_set_value_cansleep(enable_gpio, 0);

	return 0;
}

static int phy_mdm6600_power_on(struct phy *x)
{
	struct phy_mdm6600 *ddata = phy_get_drvdata(x);
	struct gpio_desc *enable_gpio = ddata->gpio[PHY_MDM6600_ENABLE];

	if (!ddata->enabled)
		return -ENODEV;

	gpiod_set_value_cansleep(enable_gpio, 1);

	return 0;
}

static int phy_mdm6600_power_off(struct phy *x)
{
	struct phy_mdm6600 *ddata = phy_get_drvdata(x);
	struct gpio_desc *enable_gpio = ddata->gpio[PHY_MDM6600_ENABLE];

	if (!ddata->enabled)
		return -ENODEV;

	gpiod_set_value_cansleep(enable_gpio, 0);

	return 0;
}

static const struct phy_ops gpio_usb_ops = {
	.init = phy_mdm6600_init,
	.power_on = phy_mdm6600_power_on,
	.power_off = phy_mdm6600_power_off,
	.owner = THIS_MODULE,
};

struct phy_mdm6600_map {
	const char *name;
	int nr_gpios;
	int direction;
};

static const struct phy_mdm6600_map
phy_mdm6600_line_map[PHY_MDM6600_NR_LINES] = {
	{ "enable", 1, GPIOD_OUT_LOW, },	/* low = disabled */
	{ "power", 1, GPIOD_OUT_LOW, },		/* low = off */
	{ "reset", 1, GPIOD_OUT_HIGH, },	/* high = reset */
	{ "mode", 2, GPIOD_OUT_LOW, },
	{ "status", 3, GPIOD_IN, },
	{ "cmd", 3, GPIOD_OUT_LOW, },		/* low = no command */
};

/**
 * phy_mdm6600_cmd() - send a command request to mdm6600
 * @ddata: device driver data
 *
 * Configures the three command request GPIOs to the specified value.
 */
static void phy_mdm6600_cmd(struct phy_mdm6600 *ddata, int val)
{
	int i;

	val &= 0x7;

	for (i = PHY_MDM6600_CMD0;
	     i <= PHY_MDM6600_CMD2; i++) {
		struct gpio_desc *gpio = ddata->gpio[i];
		int shift = (2 - (i - PHY_MDM6600_CMD0));

		if (IS_ERR(gpio))
			return;

		gpiod_set_value_cansleep(gpio, (val & BIT(shift)) >> shift);
	}
}

/**
 * phy_mdm6600_status() - read mdm6600 status lines
 * @ddata: device driver data
 */
static void phy_mdm6600_status(struct work_struct *work)
{
	struct phy_mdm6600 *ddata;
	struct device *dev;
	int i, val;

	ddata = container_of(work, struct phy_mdm6600, status_work.work);
	dev = ddata->dev;

	for (i = PHY_MDM6600_STATUS0;
	     i <= PHY_MDM6600_STATUS2; i++) {
		struct gpio_desc *gpio = ddata->gpio[i];
		int shift = (2 - (i - PHY_MDM6600_STATUS0));

		if (IS_ERR(ddata->gpio[i]))
			continue;
		val = gpiod_get_value_cansleep(gpio);
		ddata->status &= ~(BIT(shift));
		ddata->status |= (val << shift);
	}
	dev_info(dev, "modem status: %i %s\n",
		 ddata->status,
		 phy_mdm6600_status_name[ddata->status & 7]);
	complete(&ddata->ack);
}

static irqreturn_t phy_mdm6600_irq_thread(int irq, void *data)
{
	struct phy_mdm6600 *ddata = data;

	schedule_delayed_work(&ddata->status_work, msecs_to_jiffies(10));

	return IRQ_HANDLED;
}

/**
 * phy_mdm6600_init_irq() - initialize mdm6600 status IRQ lines
 * @ddata: device driver data
 */
static void phy_mdm6600_init_irq(struct phy_mdm6600 *ddata)
{
	struct device *dev = ddata->dev;
	int i, error, irq;

	for (i = PHY_MDM6600_STATUS0;
	     i <= PHY_MDM6600_STATUS2; i++) {
		if (IS_ERR(ddata->gpio[i]))
			continue;

		irq = gpiod_to_irq(ddata->gpio[i]);
		if (irq <= 0)
			continue;

		error = devm_request_threaded_irq(dev, irq, NULL,
					phy_mdm6600_irq_thread,
					IRQF_TRIGGER_RISING |
					IRQF_TRIGGER_FALLING |
					IRQF_ONESHOT,
					"mdm6600",
					ddata);
		if (error)
			dev_warn(dev, "no modem status irq%i: %i\n",
				 irq, error);
	}
}

/**
 * phy_mdm6600_init_lines() - initialize mdm6600 GPIO lines
 * @ddata: device driver data
 */
static int phy_mdm6600_init_lines(struct phy_mdm6600 *ddata)
{
	struct device *dev = ddata->dev;
	int i, j, nr_gpio = 0;

	for (i = 0; i < ARRAY_SIZE(phy_mdm6600_line_map); i++) {
		const struct phy_mdm6600_map *map =
			&phy_mdm6600_line_map[i];

		for (j = 0; j < map->nr_gpios; j++) {
			struct gpio_desc **gpio = &ddata->gpio[nr_gpio];

			*gpio = devm_gpiod_get_index(dev,
						     map->name, j,
						     map->direction);
			if (IS_ERR(*gpio)) {
				dev_info(dev,
					 "gpio %s error %li, already taken?\n",
					 map->name, PTR_ERR(*gpio));
				return PTR_ERR(*gpio);
			}
			nr_gpio++;
		}
	}

	return 0;
}

/**
 * phy_mdm6600_device_power_on() - power on mdm6600 device
 * @ddata: device driver data
 *
 * To get the integrated USB phy in MDM6600 takes some hoops. We must ensure
 * the shared USB bootmode GPIOs are configured, then request modem start-up,
 * reset and power-up.. And then we need to give up the shared USB bootmode
 * GPIOs as they are also used for Out of Band (OOB) wake for the USB and
 * TS 27.010 serial mux.
 */
static int phy_mdm6600_device_power_on(struct phy_mdm6600 *ddata)
{
	struct gpio_desc *mode_gpio0 = ddata->gpio[PHY_MDM6600_MODE0];
	struct gpio_desc *mode_gpio1 = ddata->gpio[PHY_MDM6600_MODE1];
	struct gpio_desc *reset_gpio = ddata->gpio[PHY_MDM6600_RESET];
	struct gpio_desc *power_gpio = ddata->gpio[PHY_MDM6600_POWER];
	int error = 0;

	/*
	 * Shared GPIOs must be low for normal USB mode. After booting,
	 * we don't need them. These can be also used to configure USB
	 * flashing mode later on based on a module parameter.
	 */
	gpiod_set_value_cansleep(mode_gpio0, 0);
	gpiod_set_value_cansleep(mode_gpio1, 0);

	/* Request start-up mode */
	phy_mdm6600_cmd(ddata, AP_STATUS_NO_BYPASS);

	/* Request a reset first */
	gpiod_set_value_cansleep(reset_gpio, 0);
	msleep(100);

	/* Toggle power GPIO to request mdm6600 to start */
	gpiod_set_value_cansleep(power_gpio, 1);
	msleep(100);
	gpiod_set_value_cansleep(power_gpio, 0);

	/*
	 * Looks like the USB PHY is at least 2.2 seconds.
	 * If we try to use it before that, we will get L3 errors
	 * from omap-usb-host trying to access the PHY. See also
	 * phy_mdm6600_init() for -EPROBE_DEFER.
	 */
	msleep(PHY_MDM6600_STARTUP_DELAY_MS);
	ddata->enabled = true;

	/* Booting up the rest of MDM6600 will take total about 8 seconds */
	dev_info(ddata->dev, "Waiting for power up request to complete..\n");
	if (wait_for_completion_timeout(&ddata->ack,
					msecs_to_jiffies(8000))) {
		dev_info(ddata->dev, "Powered up OK\n");
	} else {
		ddata->enabled = false;
		error = -ETIMEDOUT;
		dev_err(ddata->dev, "Timed out powering up\n");
	}

	/* Give up shared GPIOs now, they will be used for OOB wake */
	devm_gpiod_put(ddata->dev, mode_gpio0);
	ddata->gpio[PHY_MDM6600_MODE0] = ERR_PTR(-ENODEV);
	devm_gpiod_put(ddata->dev, mode_gpio1);
	ddata->gpio[PHY_MDM6600_MODE0] = ERR_PTR(-ENODEV);

	return error;
}

/**
 * phy_mdm6600_device_power_off() - power off mdm6600 device
 * @ddata: device driver data
 */
static void phy_mdm6600_device_power_off(struct phy_mdm6600 *ddata)
{
	struct gpio_desc *reset_gpio =
		ddata->gpio[PHY_MDM6600_RESET];

	ddata->enabled = false;
	phy_mdm6600_cmd(ddata, AP_STATUS_BP_SHUTDOWN_REQ);
	msleep(100);

	if (reset_gpio >= 0)
		gpiod_set_value_cansleep(reset_gpio, 1);

	dev_info(ddata->dev, "Waiting for power down request to complete.. ");
	if (wait_for_completion_timeout(&ddata->ack,
					msecs_to_jiffies(5000))) {
		dev_info(ddata->dev, "Powered down OK\n");
	} else {
		dev_err(ddata->dev, "Timed out powering down\n");
	}
}

static void phy_mdm6600_deferred_power_on(struct work_struct *work)
{
	struct phy_mdm6600 *ddata;
	int error;

	ddata = container_of(work, struct phy_mdm6600, bootup_work.work);

	error = phy_mdm6600_device_power_on(ddata);
	if (error)
		dev_err(ddata->dev, "Device not functional\n");
}

#ifdef CONFIG_OF
static const struct of_device_id phy_mdm6600_id_table[] = {
	{ .compatible = "motorola,mapphone-mdm6600", },
	{},
};
MODULE_DEVICE_TABLE(of, phy_mdm6600_id_table);
#endif

static int phy_mdm6600_probe(struct platform_device *pdev)
{
	struct phy_mdm6600 *ddata;
	struct usb_otg *otg;
	const struct of_device_id *of_id;
	int error;

	of_id = of_match_device(of_match_ptr(phy_mdm6600_id_table),
				&pdev->dev);
	if (!of_id)
		return -EINVAL;

	ddata = devm_kzalloc(&pdev->dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	INIT_DELAYED_WORK(&ddata->bootup_work,
			  phy_mdm6600_deferred_power_on);
	INIT_DELAYED_WORK(&ddata->status_work, phy_mdm6600_status);
	init_completion(&ddata->ack);

	otg = devm_kzalloc(&pdev->dev, sizeof(*otg), GFP_KERNEL);
	if (!otg)
		return -ENOMEM;

	ddata->dev = &pdev->dev;
	ddata->phy.dev = ddata->dev;
	ddata->phy.label = "phy_mdm6600";
	ddata->phy.otg = otg;
	ddata->phy.type = USB_PHY_TYPE_USB2;
	otg->usb_phy = &ddata->phy;

	platform_set_drvdata(pdev, ddata);

	error = phy_mdm6600_init_lines(ddata);
	if (error)
		return error;

	phy_mdm6600_init_irq(ddata);

	ddata->generic_phy = devm_phy_create(ddata->dev, NULL, &gpio_usb_ops);
	if (IS_ERR(ddata->generic_phy)) {
		error = PTR_ERR(ddata->generic_phy);
		goto cleanup;
	}

	phy_set_drvdata(ddata->generic_phy, ddata);

	ddata->phy_provider =
		devm_of_phy_provider_register(ddata->dev,
					      of_phy_simple_xlate);
	if (IS_ERR(ddata->phy_provider)) {
		error = PTR_ERR(ddata->phy_provider);
		goto cleanup;
	}

	schedule_delayed_work(&ddata->bootup_work, 0);

	/*
	 * See phy_mdm6600_device_power_on(). We should be able
	 * to remove this eventually when ohci-platform can deal
	 * with -EPROBE_DEFER.
	 */
	msleep(PHY_MDM6600_STARTUP_DELAY_MS + 500);

	usb_add_phy_dev(&ddata->phy);

	return 0;

cleanup:
	phy_mdm6600_device_power_off(ddata);
	return error;
}

static int phy_mdm6600_remove(struct platform_device *pdev)
{
	struct phy_mdm6600 *ddata = platform_get_drvdata(pdev);
	struct gpio_desc *reset_gpio = ddata->gpio[PHY_MDM6600_RESET];

	if (!IS_ERR(reset_gpio))
		gpiod_set_value_cansleep(reset_gpio, 1);

	phy_mdm6600_device_power_off(ddata);

	cancel_delayed_work_sync(&ddata->bootup_work);
	cancel_delayed_work_sync(&ddata->status_work);

	return 0;
}

static struct platform_driver phy_mdm6600_driver = {
	.probe = phy_mdm6600_probe,
	.remove = phy_mdm6600_remove,
	.driver = {
		.name = "phy-mapphone-mdm6600",
		.of_match_table = of_match_ptr(phy_mdm6600_id_table),
	},
};

module_platform_driver(phy_mdm6600_driver);

MODULE_ALIAS("platform:gpio_usb");
MODULE_AUTHOR("Tony Lindgren <tony@atomide.com>");
MODULE_DESCRIPTION("generic gpio usb phy driver");
MODULE_LICENSE("GPL v2");
