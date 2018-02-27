/*-*-linux-c-*-*/

/*
  Copyright (C) 2007,2008 Jonathan Woithe <jwoithe@just42.net>
  Copyright (C) 2008 Peter Gruber <nokos@gmx.net>
  Copyright (C) 2008 Tony Vroon <tony@linx.net>
  Based on earlier work:
    Copyright (C) 2003 Shane Spencer <shane@bogomip.com>
    Adrian Yee <brewt-fujitsu@brewt.org>

  Templated from msi-laptop.c and thinkpad_acpi.c which is copyright
  by its respective authors.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
  02110-1301, USA.
 */

/*
 * fujitsu-laptop.c - Fujitsu laptop support, providing access to additional
 * features made available on a range of Fujitsu laptops including the
 * P2xxx/P5xxx/S6xxx/S7xxx series.
 *
 * This driver implements a vendor-specific backlight control interface for
 * Fujitsu laptops and provides support for hotkeys present on certain Fujitsu
 * laptops.
 *
 * This driver has been tested on a Fujitsu Lifebook S6410, S7020 and
 * P8010.  It should work on most P-series and S-series Lifebooks, but
 * YMMV.
 *
 * The module parameter use_alt_lcd_levels switches between different ACPI
 * brightness controls which are used by different Fujitsu laptops.  In most
 * cases the correct method is automatically detected. "use_alt_lcd_levels=1"
 * is applicable for a Fujitsu Lifebook S6410 if autodetection fails.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/dmi.h>
#include <linux/backlight.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/kfifo.h>
#include <linux/leds.h>
#include <linux/platform_device.h>
#include <acpi/video.h>

#define FUJITSU_DRIVER_VERSION		"0.6.0"

#define FUJITSU_LCD_N_LEVELS		8

#define ACPI_FUJITSU_CLASS		"fujitsu"
#define ACPI_FUJITSU_BL_HID		"FUJ02B1"
#define ACPI_FUJITSU_BL_DRIVER_NAME	"Fujitsu laptop FUJ02B1 ACPI brightness driver"
#define ACPI_FUJITSU_BL_DEVICE_NAME	"Fujitsu FUJ02B1"
#define ACPI_FUJITSU_LAPTOP_HID		"FUJ02E3"
#define ACPI_FUJITSU_LAPTOP_DRIVER_NAME	"Fujitsu laptop FUJ02E3 ACPI hotkeys driver"
#define ACPI_FUJITSU_LAPTOP_DEVICE_NAME	"Fujitsu FUJ02E3"

#define ACPI_FUJITSU_NOTIFY_CODE	0x80

/* FUNC interface - responses */
#define UNSUPPORTED_CMD			BIT(31)

/* FUNC interface - function selectors */
#define FUNC_BACKLIGHT			(BIT(12) | BIT(2))
#define FUNC_BUTTONS			(BIT(12) | BIT(1))
#define FUNC_FLAGS			BIT(12)
#define FUNC_LEDS			(BIT(12) | BIT(0))

/* FUNC interface - operations */
#define OP_GET				BIT(1)
#define OP_GET_CAPS			0
#define OP_GET_EVENTS			BIT(0)
#define OP_GET_EXT			BIT(2)
#define OP_SET				BIT(0)
#define OP_SET_EXT			(BIT(2) | BIT(0))

/* Constants related to FUNC_BACKLIGHT */
#define FEAT_BACKLIGHT_POWER		BIT(2)
#define STATE_BACKLIGHT_OFF		(BIT(0) | BIT(1))
#define STATE_BACKLIGHT_ON		0

/* Constants related to FUNC_BUTTONS */
#define EVENT_HK1			0x410
#define EVENT_HK2			0x411
#define EVENT_HK3			0x412
#define EVENT_HK4			0x413
#define EVENT_HK5			0x420

#define HOTKEY_RINGBUFFER_SIZE		16

/* Constant related to FUNC_FLAGS */
#define FLAG_DOCK			BIT(9)
#define FLAG_LID			BIT(8)
#define FLAG_RFKILL			BIT(5)

/* Constants related to FUNC_LEDS */
#define FEAT_KEYBOARD_LAMPS		BIT(8)
#define FEAT_LOGOLAMP_ALWAYS		BIT(14)
#define FEAT_LOGOLAMP_POWERON		BIT(13)
#define STATE_LED_OFF			BIT(0)
#define STATE_LED_ON			(BIT(0) | BIT(16) | BIT(17))

#define FEAT_RADIO_LED			BIT(5)
#define STATE_RADIO_LED_OFF		0
#define STATE_RADIO_LED_ON		BIT(5)

#define FEAT_ECO_LED			BIT(16)
#define STATE_ECO_LED_ON		BIT(19)

/* Module parameters */
static int use_alt_lcd_levels = -1;
static bool disable_brightness_adjust;

/* Device controlling the backlight and associated keys */
struct fujitsu_bl {
	struct input_dev *input;
	char phys[32];
	struct backlight_device *bl_device;
	unsigned int max_brightness;
	unsigned int brightness_level;
};

static struct fujitsu_bl *fujitsu_bl;

/* Device used to access hotkeys and other features on the laptop */
struct fujitsu_laptop {
	struct input_dev *input;
	char phys[32];
	struct platform_device *pf_device;
	struct kfifo fifo;
	spinlock_t fifo_lock;
	int flags_supported;
	int flags_state;
};

static struct acpi_device *fext;

/* Fujitsu ACPI interface function */

static int call_fext_func(struct acpi_device *device,
			  int func, int op, int feature, int state)
{
	union acpi_object params[4] = {
		{ .integer.type = ACPI_TYPE_INTEGER, .integer.value = func },
		{ .integer.type = ACPI_TYPE_INTEGER, .integer.value = op },
		{ .integer.type = ACPI_TYPE_INTEGER, .integer.value = feature },
		{ .integer.type = ACPI_TYPE_INTEGER, .integer.value = state }
	};
	struct acpi_object_list arg_list = { 4, params };
	unsigned long long value;
	acpi_status status;

	status = acpi_evaluate_integer(device->handle, "FUNC", &arg_list,
				       &value);
	if (ACPI_FAILURE(status)) {
		acpi_handle_err(device->handle, "Failed to evaluate FUNC\n");
		return -ENODEV;
	}

	acpi_handle_debug(device->handle,
			  "FUNC 0x%x (args 0x%x, 0x%x, 0x%x) returned 0x%x\n",
			  func, op, feature, state, (int)value);
	return value;
}

static int fext_backlight(struct acpi_device *device,
			  int op, int feature, int state)
{
	return call_fext_func(device, FUNC_BACKLIGHT, op, feature, state);
}

static int fext_buttons(struct acpi_device *device,
			int op, int feature, int state)
{
	return call_fext_func(device, FUNC_BUTTONS, op, feature, state);
}

static int fext_flags(struct acpi_device *device,
		      int op, int feature, int state)
{
	return call_fext_func(device, FUNC_FLAGS, op, feature, state);
}

static int fext_leds(struct acpi_device *device,
		     int op, int feature, int state)
{
	return call_fext_func(device, FUNC_LEDS, op, feature, state);
}

/* Hardware access for LCD brightness control */

static int set_lcd_level(struct acpi_device *device, int level)
{
	struct fujitsu_bl *priv = acpi_driver_data(device);
	acpi_status status;
	char *method;

	switch (use_alt_lcd_levels) {
	case -1:
		if (acpi_has_method(device->handle, "SBL2"))
			method = "SBL2";
		else
			method = "SBLL";
		break;
	case 1:
		method = "SBL2";
		break;
	default:
		method = "SBLL";
		break;
	}

	acpi_handle_debug(device->handle, "set lcd level via %s [%d]\n", method,
			  level);

	if (level < 0 || level >= priv->max_brightness)
		return -EINVAL;

	status = acpi_execute_simple_method(device->handle, method, level);
	if (ACPI_FAILURE(status)) {
		acpi_handle_err(device->handle, "Failed to evaluate %s\n",
				method);
		return -ENODEV;
	}

	priv->brightness_level = level;

	return 0;
}

static int get_lcd_level(struct acpi_device *device)
{
	struct fujitsu_bl *priv = acpi_driver_data(device);
	unsigned long long state = 0;
	acpi_status status = AE_OK;

	acpi_handle_debug(device->handle, "get lcd level via GBLL\n");

	status = acpi_evaluate_integer(device->handle, "GBLL", NULL, &state);
	if (ACPI_FAILURE(status))
		return 0;

	priv->brightness_level = state & 0x0fffffff;

	return priv->brightness_level;
}

static int get_max_brightness(struct acpi_device *device)
{
	struct fujitsu_bl *priv = acpi_driver_data(device);
	unsigned long long state = 0;
	acpi_status status = AE_OK;

	acpi_handle_debug(device->handle, "get max lcd level via RBLL\n");

	status = acpi_evaluate_integer(device->handle, "RBLL", NULL, &state);
	if (ACPI_FAILURE(status))
		return -1;

	priv->max_brightness = state;

	return priv->max_brightness;
}

/* Backlight device stuff */

static int bl_get_brightness(struct backlight_device *b)
{
	struct acpi_device *device = bl_get_data(b);

	return b->props.power == FB_BLANK_POWERDOWN ? 0 : get_lcd_level(device);
}

static int bl_update_status(struct backlight_device *b)
{
	struct acpi_device *device = bl_get_data(b);

	if (fext) {
		if (b->props.power == FB_BLANK_POWERDOWN)
			fext_backlight(fext, OP_SET, FEAT_BACKLIGHT_POWER,
				       STATE_BACKLIGHT_OFF);
		else
			fext_backlight(fext, OP_SET, FEAT_BACKLIGHT_POWER,
				       STATE_BACKLIGHT_ON);
	}

	return set_lcd_level(device, b->props.brightness);
}

static const struct backlight_ops fujitsu_bl_ops = {
	.get_brightness = bl_get_brightness,
	.update_status = bl_update_status,
};

static ssize_t lid_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct fujitsu_laptop *priv = dev_get_drvdata(dev);

	if (!(priv->flags_supported & FLAG_LID))
		return sprintf(buf, "unknown\n");
	if (priv->flags_state & FLAG_LID)
		return sprintf(buf, "open\n");
	else
		return sprintf(buf, "closed\n");
}

static ssize_t dock_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct fujitsu_laptop *priv = dev_get_drvdata(dev);

	if (!(priv->flags_supported & FLAG_DOCK))
		return sprintf(buf, "unknown\n");
	if (priv->flags_state & FLAG_DOCK)
		return sprintf(buf, "docked\n");
	else
		return sprintf(buf, "undocked\n");
}

static ssize_t radios_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct fujitsu_laptop *priv = dev_get_drvdata(dev);

	if (!(priv->flags_supported & FLAG_RFKILL))
		return sprintf(buf, "unknown\n");
	if (priv->flags_state & FLAG_RFKILL)
		return sprintf(buf, "on\n");
	else
		return sprintf(buf, "killed\n");
}

static DEVICE_ATTR_RO(lid);
static DEVICE_ATTR_RO(dock);
static DEVICE_ATTR_RO(radios);

static struct attribute *fujitsu_pf_attributes[] = {
	&dev_attr_lid.attr,
	&dev_attr_dock.attr,
	&dev_attr_radios.attr,
	NULL
};

static const struct attribute_group fujitsu_pf_attribute_group = {
	.attrs = fujitsu_pf_attributes
};

static struct platform_driver fujitsu_pf_driver = {
	.driver = {
		   .name = "fujitsu-laptop",
		   }
};

/* ACPI device for LCD brightness control */

static const struct key_entry keymap_backlight[] = {
	{ KE_KEY, true, { KEY_BRIGHTNESSUP } },
	{ KE_KEY, false, { KEY_BRIGHTNESSDOWN } },
	{ KE_END, 0 }
};

static int acpi_fujitsu_bl_input_setup(struct acpi_device *device)
{
	struct fujitsu_bl *priv = acpi_driver_data(device);
	int ret;

	priv->input = devm_input_allocate_device(&device->dev);
	if (!priv->input)
		return -ENOMEM;

	snprintf(priv->phys, sizeof(priv->phys), "%s/video/input0",
		 acpi_device_hid(device));

	priv->input->name = acpi_device_name(device);
	priv->input->phys = priv->phys;
	priv->input->id.bustype = BUS_HOST;
	priv->input->id.product = 0x06;

	ret = sparse_keymap_setup(priv->input, keymap_backlight, NULL);
	if (ret)
		return ret;

	return input_register_device(priv->input);
}

static int fujitsu_backlight_register(struct acpi_device *device)
{
	struct fujitsu_bl *priv = acpi_driver_data(device);
	const struct backlight_properties props = {
		.brightness = priv->brightness_level,
		.max_brightness = priv->max_brightness - 1,
		.type = BACKLIGHT_PLATFORM
	};
	struct backlight_device *bd;

	bd = devm_backlight_device_register(&device->dev, "fujitsu-laptop",
					    &device->dev, device,
					    &fujitsu_bl_ops, &props);
	if (IS_ERR(bd))
		return PTR_ERR(bd);

	priv->bl_device = bd;

	return 0;
}

static int acpi_fujitsu_bl_add(struct acpi_device *device)
{
	struct fujitsu_bl *priv;
	int ret;

	if (acpi_video_get_backlight_type() != acpi_backlight_vendor)
		return -ENODEV;

	priv = devm_kzalloc(&device->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	fujitsu_bl = priv;
	strcpy(acpi_device_name(device), ACPI_FUJITSU_BL_DEVICE_NAME);
	strcpy(acpi_device_class(device), ACPI_FUJITSU_CLASS);
	device->driver_data = priv;

	pr_info("ACPI: %s [%s]\n",
		acpi_device_name(device), acpi_device_bid(device));

	if (get_max_brightness(device) <= 0)
		priv->max_brightness = FUJITSU_LCD_N_LEVELS;
	get_lcd_level(device);

	ret = acpi_fujitsu_bl_input_setup(device);
	if (ret)
		return ret;

	return fujitsu_backlight_register(device);
}

/* Brightness notify */

static void acpi_fujitsu_bl_notify(struct acpi_device *device, u32 event)
{
	struct fujitsu_bl *priv = acpi_driver_data(device);
	int oldb, newb;

	if (event != ACPI_FUJITSU_NOTIFY_CODE) {
		acpi_handle_info(device->handle, "unsupported event [0x%x]\n",
				 event);
		sparse_keymap_report_event(priv->input, -1, 1, true);
		return;
	}

	oldb = priv->brightness_level;
	get_lcd_level(device);
	newb = priv->brightness_level;

	acpi_handle_debug(device->handle,
			  "brightness button event [%i -> %i]\n", oldb, newb);

	if (oldb == newb)
		return;

	if (!disable_brightness_adjust)
		set_lcd_level(device, newb);

	sparse_keymap_report_event(priv->input, oldb < newb, 1, true);
}

/* ACPI device for hotkey handling */

static const struct key_entry keymap_default[] = {
	{ KE_KEY, EVENT_HK1, { KEY_PROG1 } },
	{ KE_KEY, EVENT_HK2, { KEY_PROG2 } },
	{ KE_KEY, EVENT_HK3, { KEY_PROG3 } },
	{ KE_KEY, EVENT_HK4, { KEY_PROG4 } },
	{ KE_KEY, EVENT_HK5, { KEY_RFKILL } },
	{ KE_KEY, BIT(26),   { KEY_TOUCHPAD_TOGGLE } },
	{ KE_END, 0 }
};

static const struct key_entry keymap_s64x0[] = {
	{ KE_KEY, EVENT_HK1, { KEY_SCREENLOCK } },	/* "Lock" */
	{ KE_KEY, EVENT_HK2, { KEY_HELP } },		/* "Mobility Center */
	{ KE_KEY, EVENT_HK3, { KEY_PROG3 } },
	{ KE_KEY, EVENT_HK4, { KEY_PROG4 } },
	{ KE_END, 0 }
};

static const struct key_entry keymap_p8010[] = {
	{ KE_KEY, EVENT_HK1, { KEY_HELP } },		/* "Support" */
	{ KE_KEY, EVENT_HK2, { KEY_PROG2 } },
	{ KE_KEY, EVENT_HK3, { KEY_SWITCHVIDEOMODE } },	/* "Presentation" */
	{ KE_KEY, EVENT_HK4, { KEY_WWW } },		/* "WWW" */
	{ KE_END, 0 }
};

static const struct key_entry *keymap = keymap_default;

static int fujitsu_laptop_dmi_keymap_override(const struct dmi_system_id *id)
{
	pr_info("Identified laptop model '%s'\n", id->ident);
	keymap = id->driver_data;
	return 1;
}

static const struct dmi_system_id fujitsu_laptop_dmi_table[] = {
	{
		.callback = fujitsu_laptop_dmi_keymap_override,
		.ident = "Fujitsu Siemens S6410",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU SIEMENS"),
			DMI_MATCH(DMI_PRODUCT_NAME, "LIFEBOOK S6410"),
		},
		.driver_data = (void *)keymap_s64x0
	},
	{
		.callback = fujitsu_laptop_dmi_keymap_override,
		.ident = "Fujitsu Siemens S6420",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU SIEMENS"),
			DMI_MATCH(DMI_PRODUCT_NAME, "LIFEBOOK S6420"),
		},
		.driver_data = (void *)keymap_s64x0
	},
	{
		.callback = fujitsu_laptop_dmi_keymap_override,
		.ident = "Fujitsu LifeBook P8010",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU"),
			DMI_MATCH(DMI_PRODUCT_NAME, "LifeBook P8010"),
		},
		.driver_data = (void *)keymap_p8010
	},
	{}
};

static int acpi_fujitsu_laptop_input_setup(struct acpi_device *device)
{
	struct fujitsu_laptop *priv = acpi_driver_data(device);
	int ret;

	priv->input = devm_input_allocate_device(&device->dev);
	if (!priv->input)
		return -ENOMEM;

	snprintf(priv->phys, sizeof(priv->phys), "%s/input0",
		 acpi_device_hid(device));

	priv->input->name = acpi_device_name(device);
	priv->input->phys = priv->phys;
	priv->input->id.bustype = BUS_HOST;

	dmi_check_system(fujitsu_laptop_dmi_table);
	ret = sparse_keymap_setup(priv->input, keymap, NULL);
	if (ret)
		return ret;

	return input_register_device(priv->input);
}

static int fujitsu_laptop_platform_add(struct acpi_device *device)
{
	struct fujitsu_laptop *priv = acpi_driver_data(device);
	int ret;

	priv->pf_device = platform_device_alloc("fujitsu-laptop", -1);
	if (!priv->pf_device)
		return -ENOMEM;

	platform_set_drvdata(priv->pf_device, priv);

	ret = platform_device_add(priv->pf_device);
	if (ret)
		goto err_put_platform_device;

	ret = sysfs_create_group(&priv->pf_device->dev.kobj,
				 &fujitsu_pf_attribute_group);
	if (ret)
		goto err_del_platform_device;

	return 0;

err_del_platform_device:
	platform_device_del(priv->pf_device);
err_put_platform_device:
	platform_device_put(priv->pf_device);

	return ret;
}

static void fujitsu_laptop_platform_remove(struct acpi_device *device)
{
	struct fujitsu_laptop *priv = acpi_driver_data(device);

	sysfs_remove_group(&priv->pf_device->dev.kobj,
			   &fujitsu_pf_attribute_group);
	platform_device_unregister(priv->pf_device);
}

static int logolamp_set(struct led_classdev *cdev,
			enum led_brightness brightness)
{
	struct acpi_device *device = to_acpi_device(cdev->dev->parent);
	int poweron = STATE_LED_ON, always = STATE_LED_ON;
	int ret;

	if (brightness < LED_HALF)
		poweron = STATE_LED_OFF;

	if (brightness < LED_FULL)
		always = STATE_LED_OFF;

	ret = fext_leds(device, OP_SET, FEAT_LOGOLAMP_POWERON, poweron);
	if (ret < 0)
		return ret;

	return fext_leds(device, OP_SET, FEAT_LOGOLAMP_ALWAYS, always);
}

static enum led_brightness logolamp_get(struct led_classdev *cdev)
{
	struct acpi_device *device = to_acpi_device(cdev->dev->parent);
	int ret;

	ret = fext_leds(device, OP_GET, FEAT_LOGOLAMP_ALWAYS, 0x0);
	if (ret == STATE_LED_ON)
		return LED_FULL;

	ret = fext_leds(device, OP_GET, FEAT_LOGOLAMP_POWERON, 0x0);
	if (ret == STATE_LED_ON)
		return LED_HALF;

	return LED_OFF;
}

static int kblamps_set(struct led_classdev *cdev,
		       enum led_brightness brightness)
{
	struct acpi_device *device = to_acpi_device(cdev->dev->parent);

	if (brightness >= LED_FULL)
		return fext_leds(device, OP_SET, FEAT_KEYBOARD_LAMPS,
				 STATE_LED_ON);
	else
		return fext_leds(device, OP_SET, FEAT_KEYBOARD_LAMPS,
				 STATE_LED_OFF);
}

static enum led_brightness kblamps_get(struct led_classdev *cdev)
{
	struct acpi_device *device = to_acpi_device(cdev->dev->parent);
	enum led_brightness brightness = LED_OFF;

	if (fext_leds(device, OP_GET, FEAT_KEYBOARD_LAMPS, 0x0) == STATE_LED_ON)
		brightness = LED_FULL;

	return brightness;
}

static int radio_led_set(struct led_classdev *cdev,
			 enum led_brightness brightness)
{
	struct acpi_device *device = to_acpi_device(cdev->dev->parent);

	if (brightness >= LED_FULL)
		return fext_flags(device, OP_SET_EXT, FEAT_RADIO_LED,
				  STATE_RADIO_LED_ON);
	else
		return fext_flags(device, OP_SET_EXT, FEAT_RADIO_LED,
				  STATE_RADIO_LED_OFF);
}

static enum led_brightness radio_led_get(struct led_classdev *cdev)
{
	struct acpi_device *device = to_acpi_device(cdev->dev->parent);
	enum led_brightness brightness = LED_OFF;

	if (fext_flags(device, OP_GET_EXT, 0x0, 0x0) & STATE_RADIO_LED_ON)
		brightness = LED_FULL;

	return brightness;
}

static int eco_led_set(struct led_classdev *cdev,
		       enum led_brightness brightness)
{
	struct acpi_device *device = to_acpi_device(cdev->dev->parent);
	int curr;

	curr = fext_leds(device, OP_GET, FEAT_ECO_LED, 0x0);
	if (brightness >= LED_FULL)
		return fext_leds(device, OP_SET, FEAT_ECO_LED,
				 curr | STATE_ECO_LED_ON);
	else
		return fext_leds(device, OP_SET, FEAT_ECO_LED,
				 curr & ~STATE_ECO_LED_ON);
}

static enum led_brightness eco_led_get(struct led_classdev *cdev)
{
	struct acpi_device *device = to_acpi_device(cdev->dev->parent);
	enum led_brightness brightness = LED_OFF;

	if (fext_leds(device, OP_GET, FEAT_ECO_LED, 0x0) & STATE_ECO_LED_ON)
		brightness = LED_FULL;

	return brightness;
}

static int acpi_fujitsu_laptop_leds_register(struct acpi_device *device)
{
	struct fujitsu_laptop *priv = acpi_driver_data(device);
	struct led_classdev *led;
	int ret;

	if (fext_leds(device, OP_GET_CAPS, 0x0, 0x0) & FEAT_LOGOLAMP_POWERON) {
		led = devm_kzalloc(&device->dev, sizeof(*led), GFP_KERNEL);
		if (!led)
			return -ENOMEM;

		led->name = "fujitsu::logolamp";
		led->brightness_set_blocking = logolamp_set;
		led->brightness_get = logolamp_get;
		ret = devm_led_classdev_register(&device->dev, led);
		if (ret)
			return ret;
	}

	if ((fext_leds(device, OP_GET_CAPS, 0x0, 0x0) & FEAT_KEYBOARD_LAMPS) &&
	    (fext_buttons(device, OP_GET_CAPS, 0x0, 0x0) == 0x0)) {
		led = devm_kzalloc(&device->dev, sizeof(*led), GFP_KERNEL);
		if (!led)
			return -ENOMEM;

		led->name = "fujitsu::kblamps";
		led->brightness_set_blocking = kblamps_set;
		led->brightness_get = kblamps_get;
		ret = devm_led_classdev_register(&device->dev, led);
		if (ret)
			return ret;
	}

	/*
	 * Some Fujitsu laptops have a radio toggle button in place of a slide
	 * switch and all such machines appear to also have an RF LED.  Based on
	 * comparing DSDT tables of four Fujitsu Lifebook models (E744, E751,
	 * S7110, S8420; the first one has a radio toggle button, the other
	 * three have slide switches), bit 17 of flags_supported (the value
	 * returned by method S000 of ACPI device FUJ02E3) seems to indicate
	 * whether given model has a radio toggle button.
	 */
	if (priv->flags_supported & BIT(17)) {
		led = devm_kzalloc(&device->dev, sizeof(*led), GFP_KERNEL);
		if (!led)
			return -ENOMEM;

		led->name = "fujitsu::radio_led";
		led->brightness_set_blocking = radio_led_set;
		led->brightness_get = radio_led_get;
		led->default_trigger = "rfkill-any";
		ret = devm_led_classdev_register(&device->dev, led);
		if (ret)
			return ret;
	}

	/* Support for eco led is not always signaled in bit corresponding
	 * to the bit used to control the led. According to the DSDT table,
	 * bit 14 seems to indicate presence of said led as well.
	 * Confirm by testing the status.
	 */
	if ((fext_leds(device, OP_GET_CAPS, 0x0, 0x0) & BIT(14)) &&
	    (fext_leds(device, OP_GET, FEAT_ECO_LED, 0x0) != UNSUPPORTED_CMD)) {
		led = devm_kzalloc(&device->dev, sizeof(*led), GFP_KERNEL);
		if (!led)
			return -ENOMEM;

		led->name = "fujitsu::eco_led";
		led->brightness_set_blocking = eco_led_set;
		led->brightness_get = eco_led_get;
		ret = devm_led_classdev_register(&device->dev, led);
		if (ret)
			return ret;
	}

	return 0;
}

static int acpi_fujitsu_laptop_add(struct acpi_device *device)
{
	struct fujitsu_laptop *priv;
	int ret, i = 0;

	priv = devm_kzalloc(&device->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	WARN_ONCE(fext, "More than one FUJ02E3 ACPI device was found.  Driver may not work as intended.");
	fext = device;

	strcpy(acpi_device_name(device), ACPI_FUJITSU_LAPTOP_DEVICE_NAME);
	strcpy(acpi_device_class(device), ACPI_FUJITSU_CLASS);
	device->driver_data = priv;

	/* kfifo */
	spin_lock_init(&priv->fifo_lock);
	ret = kfifo_alloc(&priv->fifo, HOTKEY_RINGBUFFER_SIZE * sizeof(int),
			  GFP_KERNEL);
	if (ret)
		return ret;

	pr_info("ACPI: %s [%s]\n",
		acpi_device_name(device), acpi_device_bid(device));

	while (fext_buttons(device, OP_GET_EVENTS, 0x0, 0x0) != 0 &&
	       i++ < HOTKEY_RINGBUFFER_SIZE)
		; /* No action, result is discarded */
	acpi_handle_debug(device->handle, "Discarded %i ringbuffer entries\n",
			  i);

	priv->flags_supported = fext_flags(device, OP_GET_CAPS, 0x0, 0x0);

	/* Make sure our bitmask of supported functions is cleared if the
	   RFKILL function block is not implemented, like on the S7020. */
	if (priv->flags_supported == UNSUPPORTED_CMD)
		priv->flags_supported = 0;

	if (priv->flags_supported)
		priv->flags_state = fext_flags(device, OP_GET_EXT, 0x0, 0x0);

	/* Suspect this is a keymap of the application panel, print it */
	acpi_handle_info(device->handle, "BTNI: [0x%x]\n",
			 fext_buttons(device, OP_GET_CAPS, 0x0, 0x0));

	/* Sync backlight power status */
	if (fujitsu_bl && fujitsu_bl->bl_device &&
	    acpi_video_get_backlight_type() == acpi_backlight_vendor) {
		if (fext_backlight(fext, OP_GET, FEAT_BACKLIGHT_POWER,
				   0x0) == STATE_BACKLIGHT_OFF)
			fujitsu_bl->bl_device->props.power = FB_BLANK_POWERDOWN;
		else
			fujitsu_bl->bl_device->props.power = FB_BLANK_UNBLANK;
	}

	ret = acpi_fujitsu_laptop_input_setup(device);
	if (ret)
		goto err_free_fifo;

	ret = acpi_fujitsu_laptop_leds_register(device);
	if (ret)
		goto err_free_fifo;

	ret = fujitsu_laptop_platform_add(device);
	if (ret)
		goto err_free_fifo;

	return 0;

err_free_fifo:
	kfifo_free(&priv->fifo);

	return ret;
}

static int acpi_fujitsu_laptop_remove(struct acpi_device *device)
{
	struct fujitsu_laptop *priv = acpi_driver_data(device);

	fujitsu_laptop_platform_remove(device);

	kfifo_free(&priv->fifo);

	return 0;
}

static void acpi_fujitsu_laptop_press(struct acpi_device *device, int scancode)
{
	struct fujitsu_laptop *priv = acpi_driver_data(device);
	int ret;

	ret = kfifo_in_locked(&priv->fifo, (unsigned char *)&scancode,
			      sizeof(scancode), &priv->fifo_lock);
	if (ret != sizeof(scancode)) {
		dev_info(&priv->input->dev, "Could not push scancode [0x%x]\n",
			 scancode);
		return;
	}
	sparse_keymap_report_event(priv->input, scancode, 1, false);
	dev_dbg(&priv->input->dev, "Push scancode into ringbuffer [0x%x]\n",
		scancode);
}

static void acpi_fujitsu_laptop_release(struct acpi_device *device)
{
	struct fujitsu_laptop *priv = acpi_driver_data(device);
	int scancode, ret;

	while (true) {
		ret = kfifo_out_locked(&priv->fifo, (unsigned char *)&scancode,
				       sizeof(scancode), &priv->fifo_lock);
		if (ret != sizeof(scancode))
			return;
		sparse_keymap_report_event(priv->input, scancode, 0, false);
		dev_dbg(&priv->input->dev,
			"Pop scancode from ringbuffer [0x%x]\n", scancode);
	}
}

static void acpi_fujitsu_laptop_notify(struct acpi_device *device, u32 event)
{
	struct fujitsu_laptop *priv = acpi_driver_data(device);
	int scancode, i = 0;
	unsigned int irb;

	if (event != ACPI_FUJITSU_NOTIFY_CODE) {
		acpi_handle_info(device->handle, "Unsupported event [0x%x]\n",
				 event);
		sparse_keymap_report_event(priv->input, -1, 1, true);
		return;
	}

	if (priv->flags_supported)
		priv->flags_state = fext_flags(device, OP_GET_EXT, 0x0, 0x0);

	while ((irb = fext_buttons(device, OP_GET_EVENTS, 0x0, 0x0)) != 0 &&
	       i++ < HOTKEY_RINGBUFFER_SIZE) {
		scancode = irb & 0x4ff;
		if (sparse_keymap_entry_from_scancode(priv->input, scancode))
			acpi_fujitsu_laptop_press(device, scancode);
		else if (scancode == 0)
			acpi_fujitsu_laptop_release(device);
		else
			acpi_handle_info(device->handle,
					 "Unknown GIRB result [%x]\n", irb);
	}

	/* On some models (first seen on the Skylake-based Lifebook
	 * E736/E746/E756), the touchpad toggle hotkey (Fn+F4) is
	 * handled in software; its state is queried using FUNC_FLAGS
	 */
	if ((priv->flags_supported & BIT(26)) &&
	    (fext_flags(device, OP_GET_EVENTS, 0x0, 0x0) & BIT(26)))
		sparse_keymap_report_event(priv->input, BIT(26), 1, true);
}

/* Initialization */

static const struct acpi_device_id fujitsu_bl_device_ids[] = {
	{ACPI_FUJITSU_BL_HID, 0},
	{"", 0},
};

static struct acpi_driver acpi_fujitsu_bl_driver = {
	.name = ACPI_FUJITSU_BL_DRIVER_NAME,
	.class = ACPI_FUJITSU_CLASS,
	.ids = fujitsu_bl_device_ids,
	.ops = {
		.add = acpi_fujitsu_bl_add,
		.notify = acpi_fujitsu_bl_notify,
		},
};

static const struct acpi_device_id fujitsu_laptop_device_ids[] = {
	{ACPI_FUJITSU_LAPTOP_HID, 0},
	{"", 0},
};

static struct acpi_driver acpi_fujitsu_laptop_driver = {
	.name = ACPI_FUJITSU_LAPTOP_DRIVER_NAME,
	.class = ACPI_FUJITSU_CLASS,
	.ids = fujitsu_laptop_device_ids,
	.ops = {
		.add = acpi_fujitsu_laptop_add,
		.remove = acpi_fujitsu_laptop_remove,
		.notify = acpi_fujitsu_laptop_notify,
		},
};

static const struct acpi_device_id fujitsu_ids[] __used = {
	{ACPI_FUJITSU_BL_HID, 0},
	{ACPI_FUJITSU_LAPTOP_HID, 0},
	{"", 0}
};
MODULE_DEVICE_TABLE(acpi, fujitsu_ids);

static int __init fujitsu_init(void)
{
	int ret;

	ret = acpi_bus_register_driver(&acpi_fujitsu_bl_driver);
	if (ret)
		return ret;

	/* Register platform stuff */

	ret = platform_driver_register(&fujitsu_pf_driver);
	if (ret)
		goto err_unregister_acpi;

	/* Register laptop driver */

	ret = acpi_bus_register_driver(&acpi_fujitsu_laptop_driver);
	if (ret)
		goto err_unregister_platform_driver;

	pr_info("driver " FUJITSU_DRIVER_VERSION " successfully loaded\n");

	return 0;

err_unregister_platform_driver:
	platform_driver_unregister(&fujitsu_pf_driver);
err_unregister_acpi:
	acpi_bus_unregister_driver(&acpi_fujitsu_bl_driver);

	return ret;
}

static void __exit fujitsu_cleanup(void)
{
	acpi_bus_unregister_driver(&acpi_fujitsu_laptop_driver);

	platform_driver_unregister(&fujitsu_pf_driver);

	acpi_bus_unregister_driver(&acpi_fujitsu_bl_driver);

	pr_info("driver unloaded\n");
}

module_init(fujitsu_init);
module_exit(fujitsu_cleanup);

module_param(use_alt_lcd_levels, int, 0644);
MODULE_PARM_DESC(use_alt_lcd_levels, "Interface used for setting LCD brightness level (-1 = auto, 0 = force SBLL, 1 = force SBL2)");
module_param(disable_brightness_adjust, bool, 0644);
MODULE_PARM_DESC(disable_brightness_adjust, "Disable LCD brightness adjustment");

MODULE_AUTHOR("Jonathan Woithe, Peter Gruber, Tony Vroon");
MODULE_DESCRIPTION("Fujitsu laptop extras support");
MODULE_VERSION(FUJITSU_DRIVER_VERSION);
MODULE_LICENSE("GPL");
