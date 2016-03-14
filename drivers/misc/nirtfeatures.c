/*
 * Copyright (C) 2016 National Instruments Corp.
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
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/leds.h>
#include <linux/module.h>

#define MODULE_NAME "nirtfeatures"

/* Register addresses */

#define NIRTF_YEAR		0x01
#define NIRTF_MONTH		0x02
#define NIRTF_DAY		0x03
#define NIRTF_HOUR		0x04
#define NIRTF_MINUTE		0x05
#define NIRTF_SCRATCH		0x06
#define NIRTF_PLATFORM_MISC	0x07
#define NIRTF_PROC_RESET_SOURCE	0x11
#define NIRTF_CONTROLLER_MODE	0x12
#define NIRTF_SYSTEM_LEDS	0x20
#define NIRTF_STATUS_LED_SHIFT1	0x21
#define NIRTF_STATUS_LED_SHIFT0	0x22
#define NIRTF_RT_LEDS		0x23

#define NIRTF_IO_SIZE		0x40

/* Register values */

#define NIRTF_PLATFORM_MISC_ID_MASK		0x07
#define NIRTF_PLATFORM_MISC_ID_MANHATTAN	0
#define NIRTF_PLATFORM_MISC_ID_HAMMERHEAD	4
#define NIRTF_PLATFORM_MISC_ID_WINGHEAD		5

#define NIRTF_CONTROLLER_MODE_NO_FPGA_SW	0x40
#define NIRTF_CONTROLLER_MODE_HARD_BOOT_N	0x20
#define NIRTF_CONTROLLER_MODE_NO_FPGA		0x10
#define NIRTF_CONTROLLER_MODE_RECOVERY		0x08
#define NIRTF_CONTROLLER_MODE_CONSOLE_OUT	0x04
#define NIRTF_CONTROLLER_MODE_IP_RESET		0x02
#define NIRTF_CONTROLLER_MODE_SAFE		0x01

#define NIRTF_SYSTEM_LEDS_STATUS_RED		0x08
#define NIRTF_SYSTEM_LEDS_STATUS_YELLOW		0x04
#define NIRTF_SYSTEM_LEDS_POWER_GREEN		0x02
#define NIRTF_SYSTEM_LEDS_POWER_YELLOW		0x01

#define NIRTF_RT_LEDS_USER2_GREEN	0x08
#define NIRTF_RT_LEDS_USER2_YELLOW	0x04
#define NIRTF_RT_LEDS_USER1_GREEN	0x02
#define NIRTF_RT_LEDS_USER1_YELLOW	0x01

#define to_nirtfeatures(dev)	acpi_driver_data(to_acpi_device(dev))

/* Structures */

struct nirtfeatures {
	struct acpi_device *acpi_device;
	u16 io_base;
	spinlock_t lock;
	u8 revision[5];
	const char *bpstring;
	struct nirtfeatures_led *extra_leds;
	unsigned num_extra_leds;
};

struct nirtfeatures_led {
	struct led_classdev cdev;
	struct nirtfeatures *nirtfeatures;
	u8 address;
	u8 mask;
	u8 pattern_hi_addr;
	u8 pattern_lo_addr;
};

/* sysfs files */

static ssize_t revision_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct nirtfeatures *nirtfeatures = to_nirtfeatures(dev);

	return sprintf(buf, "20%02X/%02X/%02X %02X:%02X\n",
		       nirtfeatures->revision[0], nirtfeatures->revision[1],
		       nirtfeatures->revision[2], nirtfeatures->revision[3],
		       nirtfeatures->revision[4]);
}
static DEVICE_ATTR_RO(revision);

static ssize_t scratch_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct nirtfeatures *nirtfeatures = to_nirtfeatures(dev);
	u8 data;

	spin_lock(&nirtfeatures->lock);

	data = inb(nirtfeatures->io_base + NIRTF_SCRATCH);

	spin_unlock(&nirtfeatures->lock);

	return sprintf(buf, "%02x\n", data);
}

static ssize_t scratch_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct nirtfeatures *nirtfeatures = to_nirtfeatures(dev);
	unsigned long tmp;

	if (kstrtoul(buf, 0, &tmp) || (tmp > 0xFF))
		return -EINVAL;

	spin_lock(&nirtfeatures->lock);

	outb((u8)tmp, nirtfeatures->io_base + NIRTF_SCRATCH);

	spin_unlock(&nirtfeatures->lock);

	return count;
}
static DEVICE_ATTR_RW(scratch);

static ssize_t backplane_id_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct nirtfeatures *nirtfeatures = to_nirtfeatures(dev);

	return sprintf(buf, "%s\n", nirtfeatures->bpstring);
}
static DEVICE_ATTR_RO(backplane_id);

static const char *const nirtfeatures_reset_source_strings[] = {
	"button", "processor", "fpga", "watchdog", "software",
};

static ssize_t reset_source_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct nirtfeatures *nirtfeatures = to_nirtfeatures(dev);
	u8 data;
	int i;

	data = inb(nirtfeatures->io_base + NIRTF_PROC_RESET_SOURCE);

	for (i = 0; i < ARRAY_SIZE(nirtfeatures_reset_source_strings); i++)
		if ((1 << i) & data)
			return sprintf(buf, "%s\n",
				       nirtfeatures_reset_source_strings[i]);

	return sprintf(buf, "poweron\n");
}
static DEVICE_ATTR_RO(reset_source);

static ssize_t mode_show(struct device *dev, char *buf, unsigned int mask)
{
	struct nirtfeatures *nirtfeatures = to_nirtfeatures(dev);
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_CONTROLLER_MODE);
	data &= mask;

	return sprintf(buf, "%u\n", !!data);
}

static ssize_t no_fpga_sw_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct nirtfeatures *nirtfeatures = to_nirtfeatures(dev);
	int ret;

	spin_lock(&nirtfeatures->lock);

	ret = mode_show(dev, buf, NIRTF_CONTROLLER_MODE_NO_FPGA_SW);

	spin_unlock(&nirtfeatures->lock);

	return ret;
}

static ssize_t no_fpga_sw_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct nirtfeatures *nirtfeatures = to_nirtfeatures(dev);
	unsigned long tmp;
	u8 data;

	if (kstrtoul(buf, 0, &tmp) || (tmp > 1))
		return -EINVAL;

	spin_lock(&nirtfeatures->lock);

	data = inb(nirtfeatures->io_base + NIRTF_CONTROLLER_MODE);

	if (tmp)
		data |= NIRTF_CONTROLLER_MODE_NO_FPGA_SW;
	else
		data &= ~NIRTF_CONTROLLER_MODE_NO_FPGA_SW;

	outb(data, nirtfeatures->io_base + NIRTF_CONTROLLER_MODE);

	spin_unlock(&nirtfeatures->lock);

	return count;
}
static DEVICE_ATTR_RW(no_fpga_sw);

static ssize_t soft_reset_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return mode_show(dev, buf, NIRTF_CONTROLLER_MODE_HARD_BOOT_N);
}
static DEVICE_ATTR_RO(soft_reset);

static ssize_t no_fpga_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return mode_show(dev, buf, NIRTF_CONTROLLER_MODE_NO_FPGA);
}
static DEVICE_ATTR_RO(no_fpga);

static ssize_t recovery_mode_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return mode_show(dev, buf, NIRTF_CONTROLLER_MODE_RECOVERY);
}
static DEVICE_ATTR_RO(recovery_mode);

static ssize_t console_out_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return mode_show(dev, buf, NIRTF_CONTROLLER_MODE_CONSOLE_OUT);
}
static DEVICE_ATTR_RO(console_out);

static ssize_t ip_reset_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return mode_show(dev, buf, NIRTF_CONTROLLER_MODE_IP_RESET);
}
static DEVICE_ATTR_RO(ip_reset);

static ssize_t safe_mode_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return mode_show(dev, buf, NIRTF_CONTROLLER_MODE_SAFE);
}
static DEVICE_ATTR_RO(safe_mode);

static const struct attribute *nirtfeatures_attrs[] = {
	&dev_attr_revision.attr,
	&dev_attr_scratch.attr,
	&dev_attr_backplane_id.attr,
	&dev_attr_reset_source.attr,
	&dev_attr_no_fpga_sw.attr,
	&dev_attr_soft_reset.attr,
	&dev_attr_no_fpga.attr,
	&dev_attr_recovery_mode.attr,
	&dev_attr_console_out.attr,
	&dev_attr_ip_reset.attr,
	&dev_attr_safe_mode.attr,
	NULL
};

/* LEDs */

static void nirtfeatures_led_brightness_set(struct led_classdev *led_cdev,
					    enum led_brightness brightness)
{
	struct nirtfeatures_led *led = (struct nirtfeatures_led *)led_cdev;
	u8 data;

	spin_lock(&led->nirtfeatures->lock);

	data = inb(led->nirtfeatures->io_base + led->address);
	data &= ~led->mask;
	if (!!brightness)
		data |= led->mask;
	outb(data, led->nirtfeatures->io_base + led->address);

	if (led->pattern_hi_addr && led->pattern_lo_addr) {
		/* Write the high byte first. */
		outb(brightness >> 8,
		     led->nirtfeatures->io_base + led->pattern_hi_addr);
		outb(brightness & 0xFF,
		     led->nirtfeatures->io_base + led->pattern_lo_addr);
	}

	spin_unlock(&led->nirtfeatures->lock);
}

static enum led_brightness
nirtfeatures_led_brightness_get(struct led_classdev *led_cdev)
{
	struct nirtfeatures_led *led = (struct nirtfeatures_led *)led_cdev;
	u8 data;

	data = inb(led->nirtfeatures->io_base + led->address);

	/*
	 * For the yellow status LED, the blink pattern used for brightness
	 * on write is write-only, so we just return on/off for all LEDs.
	 */
	return (data & led->mask) ? led_cdev->max_brightness : 0;
}

static struct nirtfeatures_led nirtfeatures_leds_common[] = {
	{
		{
			.name = "nilrt:user1:green",
		},
		.address = NIRTF_RT_LEDS,
		.mask = NIRTF_RT_LEDS_USER1_GREEN,
	},
	{
		{
			.name = "nilrt:user1:yellow",
		},
		.address = NIRTF_RT_LEDS,
		.mask = NIRTF_RT_LEDS_USER1_YELLOW,
	},
	{
		{
			.name = "nilrt:status:red",
		},
		.address = NIRTF_SYSTEM_LEDS,
		.mask = NIRTF_SYSTEM_LEDS_STATUS_RED,
	},
	{
		{
			.name = "nilrt:status:yellow",
			.max_brightness = 0xFFFF,
		},
		.address = NIRTF_SYSTEM_LEDS,
		.mask = NIRTF_SYSTEM_LEDS_STATUS_YELLOW,
		.pattern_hi_addr = NIRTF_STATUS_LED_SHIFT1,
		.pattern_lo_addr = NIRTF_STATUS_LED_SHIFT0,
	},
	{
		{
			.name = "nilrt:power:green",
		},
		.address = NIRTF_SYSTEM_LEDS,
		.mask = NIRTF_SYSTEM_LEDS_POWER_GREEN,
	},
	{
		{
			.name = "nilrt:power:yellow",
		},
		.address = NIRTF_SYSTEM_LEDS,
		.mask = NIRTF_SYSTEM_LEDS_POWER_YELLOW,
	},
};

static struct nirtfeatures_led nirtfeatures_leds_cdaq[] = {
	{
		{
			.name = "nilrt:user2:green",
		},
		.address = NIRTF_RT_LEDS,
		.mask = NIRTF_RT_LEDS_USER2_GREEN,
	},
	{
		{
			.name = "nilrt:user2:yellow",
		},
		.address = NIRTF_RT_LEDS,
		.mask = NIRTF_RT_LEDS_USER2_YELLOW,
	},
};

static int nirtfeatures_create_leds(struct nirtfeatures *nirtfeatures)
{
	int i;
	int err;

	struct nirtfeatures_led *leds = nirtfeatures_leds_common;

	for (i = 0; i < ARRAY_SIZE(nirtfeatures_leds_common); i++) {

		leds[i].nirtfeatures = nirtfeatures;

		if (leds[i].cdev.max_brightness == 0)
			leds[i].cdev.max_brightness = 1;

		leds[i].cdev.brightness_set = nirtfeatures_led_brightness_set;
		leds[i].cdev.brightness_get = nirtfeatures_led_brightness_get;

		err =
		    devm_led_classdev_register(&nirtfeatures->acpi_device->dev,
						 &leds[i].cdev);
		if (err)
			return err;
	}

	for (i = 0; i < nirtfeatures->num_extra_leds; ++i) {

		nirtfeatures->extra_leds[i].nirtfeatures = nirtfeatures;

		if (nirtfeatures->extra_leds[i].cdev.max_brightness == 0)
			nirtfeatures->extra_leds[i].cdev.max_brightness = 1;

		nirtfeatures->extra_leds[i].cdev.brightness_set =
			nirtfeatures_led_brightness_set;

		nirtfeatures->extra_leds[i].cdev.brightness_get =
			nirtfeatures_led_brightness_get;

		err = devm_led_classdev_register(
				&nirtfeatures->acpi_device->dev,
				&nirtfeatures->extra_leds[i].cdev);
		if (err)
			return err;
	}

	return 0;
}

/* ACPI driver */

static acpi_status nirtfeatures_resources(struct acpi_resource *res,
					  void *data)
{
	struct nirtfeatures *nirtfeatures = data;
	u8 io_size;

	if (res->type == ACPI_RESOURCE_TYPE_IO) {
		if (nirtfeatures->io_base != 0) {
			dev_err(&nirtfeatures->acpi_device->dev,
				"too many IO resources\n");
			return AE_ALREADY_EXISTS;
		}

		nirtfeatures->io_base = res->data.io.minimum;
		io_size = res->data.io.address_length;

		if (io_size != NIRTF_IO_SIZE) {
			dev_err(&nirtfeatures->acpi_device->dev,
				"invalid IO size 0x%02x\n", io_size);
			return AE_ERROR;
		}

		if (!devm_request_region(&nirtfeatures->acpi_device->dev,
					 nirtfeatures->io_base, io_size,
					 MODULE_NAME)) {
			dev_err(&nirtfeatures->acpi_device->dev,
				"failed to get memory region\n");
			return AE_NO_MEMORY;
		}
	}

	return AE_OK;
}

static int nirtfeatures_acpi_add(struct acpi_device *device)
{
	struct nirtfeatures *nirtfeatures;
	acpi_status acpi_ret;
	u8 bpinfo;
	int err;

	nirtfeatures = devm_kzalloc(&device->dev, sizeof(*nirtfeatures),
				    GFP_KERNEL);
	if (!nirtfeatures)
		return -ENOMEM;

	device->driver_data = nirtfeatures;
	nirtfeatures->acpi_device = device;

	acpi_ret = acpi_walk_resources(device->handle, METHOD_NAME__CRS,
				       nirtfeatures_resources, nirtfeatures);
	if (ACPI_FAILURE(acpi_ret) || nirtfeatures->io_base == 0) {
		dev_err(&device->dev, "failed to get resources\n");
		return -ENODEV;
	}

	bpinfo = inb(nirtfeatures->io_base + NIRTF_PLATFORM_MISC);
	bpinfo &= NIRTF_PLATFORM_MISC_ID_MASK;

	switch (bpinfo) {
	case NIRTF_PLATFORM_MISC_ID_MANHATTAN:
		nirtfeatures->bpstring = "Manhattan";
		break;
	case NIRTF_PLATFORM_MISC_ID_HAMMERHEAD:
		nirtfeatures->bpstring = "Hammerhead";
		nirtfeatures->extra_leds = nirtfeatures_leds_cdaq;
		nirtfeatures->num_extra_leds =
			ARRAY_SIZE(nirtfeatures_leds_cdaq);
		break;
	case NIRTF_PLATFORM_MISC_ID_WINGHEAD:
		nirtfeatures->bpstring = "Winghead";
		nirtfeatures->extra_leds = nirtfeatures_leds_cdaq;
		nirtfeatures->num_extra_leds =
			ARRAY_SIZE(nirtfeatures_leds_cdaq);
		break;
	default:
		dev_err(&nirtfeatures->acpi_device->dev,
			"Unrecognized backplane type %u\n", bpinfo);
		nirtfeatures->bpstring = "Unknown";
		break;
	}

	spin_lock_init(&nirtfeatures->lock);

	nirtfeatures->revision[0] = inb(nirtfeatures->io_base + NIRTF_YEAR);
	nirtfeatures->revision[1] = inb(nirtfeatures->io_base + NIRTF_MONTH);
	nirtfeatures->revision[2] = inb(nirtfeatures->io_base + NIRTF_DAY);
	nirtfeatures->revision[3] = inb(nirtfeatures->io_base + NIRTF_HOUR);
	nirtfeatures->revision[4] = inb(nirtfeatures->io_base + NIRTF_MINUTE);

	err = nirtfeatures_create_leds(nirtfeatures);
	if (err) {
		dev_err(&device->dev, "could not create LEDs\n");
		return err;
	}

	err = sysfs_create_files(&device->dev.kobj, nirtfeatures_attrs);
	if (err) {
		dev_err(&device->dev, "could not create sysfs attributes\n");
		return err;
	}

	dev_dbg(&nirtfeatures->acpi_device->dev,
		"%s backplane, revision 20%02X/%02X/%02X %02X:%02X, io_base 0x%04X\n",
		nirtfeatures->bpstring, nirtfeatures->revision[0],
		nirtfeatures->revision[1], nirtfeatures->revision[2],
		nirtfeatures->revision[3], nirtfeatures->revision[4],
		nirtfeatures->io_base);

	return 0;
}

static int nirtfeatures_acpi_remove(struct acpi_device *device)
{
	sysfs_remove_files(&device->dev.kobj, nirtfeatures_attrs);

	return 0;
}

static const struct acpi_device_id nirtfeatures_device_ids[] = {
	{"NIC775D", 0},
	{"", 0},
};

static struct acpi_driver nirtfeatures_acpi_driver = {
	.name = MODULE_NAME,
	.ids = nirtfeatures_device_ids,
	.ops = {
		.add = nirtfeatures_acpi_add,
		.remove = nirtfeatures_acpi_remove,
	},
};

module_acpi_driver(nirtfeatures_acpi_driver);

MODULE_DEVICE_TABLE(acpi, nirtfeatures_device_ids);
MODULE_DESCRIPTION("NI RT Features");
MODULE_AUTHOR("Jeff Westfahl <jeff.westfahl@ni.com>");
MODULE_LICENSE("GPL");
