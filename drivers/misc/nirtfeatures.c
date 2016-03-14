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

#include <acpi/acpi.h>

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

#define to_nirtfeatures(dev)	acpi_driver_data(to_acpi_device(dev))

/*
 *=====================================================================
 * ACPI NI physical interface element support
 *=====================================================================
 */
#define MAX_NAMELEN		64
#define MAX_NODELEN		128
#define MIN_PIE_CAPS_VERSION	2
#define MAX_PIE_CAPS_VERSION	2

enum nirtfeatures_pie_class {
	PIE_CLASS_INPUT = 0,
	PIE_CLASS_OUTPUT = 1
};

enum nirtfeatures_pie_type {
	PIE_TYPE_UNKNOWN = 0,
	PIE_TYPE_SWITCH = 1,
	PIE_TYPE_LED = 2
};

struct nirtfeatures_pie_descriptor {
	char name[MAX_NAMELEN];
	enum nirtfeatures_pie_class pie_class;
	enum nirtfeatures_pie_type pie_type;
	bool is_user_visible;
	unsigned int notification_value;
};

struct nirtfeatures_pie_descriptor_led_color {
	char name[MAX_NAMELEN];
	int brightness_range_low;
	int brightness_range_high;
};

struct nirtfeatures_pie_location {
	unsigned int element;
	unsigned int subelement;
};

/* Structures */

struct nirtfeatures {
	struct acpi_device *acpi_device;
	u16 io_base;
	spinlock_t lock;
	u8 revision[5];
	const char *bpstring;
};

struct nirtfeatures_led {
	struct led_classdev cdev;
	struct nirtfeatures *nirtfeatures;
	struct nirtfeatures_pie_location pie_location;
	char name_string[MAX_NODELEN];
	u8 address;
	u8 mask;
	u8 pattern_hi_addr;
	u8 pattern_lo_addr;
	struct list_head node;
};

static LIST_HEAD(nirtfeatures_led_pie_list);

struct nirtfeatures_switch {
	struct input_dev *cdev;
	struct nirtfeatures *nirtfeatures;
	struct nirtfeatures_pie_descriptor pie_descriptor;
	struct nirtfeatures_pie_location pie_location;
	char name_string[MAX_NODELEN];
	char phys_location_string[MAX_NODELEN];
	struct list_head node;
};

static LIST_HEAD(nirtfeatures_switch_pie_list);

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

/*
 *=====================================================================
 * ACPI NI physical interface element support
 *=====================================================================
 */

/* Note that callers of this function are responsible for deallocating
 * the buffer allocated by acpi_evaluate_object() by calling
 * kfree() on the pointer passed back in result_buffer.
 */
static int
nirtfeatures_call_acpi_method(struct acpi_device *device,
			      const char *method_name, int argc,
			      union acpi_object *argv, acpi_size *result_size,
			      void **result_buffer)
{
	acpi_status acpi_ret;
	acpi_handle acpi_hdl;
	struct acpi_object_list acpi_params;
	struct acpi_buffer acpi_result = { ACPI_ALLOCATE_BUFFER, NULL };

	if (!device || !result_size || !result_buffer)
		return -EINVAL;

	acpi_ret = acpi_get_handle(device->handle, (acpi_string)method_name,
				   &acpi_hdl);
	if (ACPI_FAILURE(acpi_ret)) {
		dev_err(&device->dev, "ACPI get handle for %s failed (%d)\n",
			method_name, acpi_ret);
		return -1;
	}

	acpi_params.count = argc;
	acpi_params.pointer = argv;

	acpi_ret = acpi_evaluate_object(acpi_hdl, NULL, &acpi_params,
					&acpi_result);
	if (ACPI_FAILURE(acpi_ret)) {
		dev_err(&device->dev, "ACPI evaluate for %s failed (%d)\n",
			method_name, acpi_ret);
		return -1;
	}

	*result_size = acpi_result.length;
	*result_buffer = acpi_result.pointer;

	return 0;
}

/* This is the generic PIE set state wrapper. It invokes the PIES
 * ACPI method to modify the state of the given PIE.
 */
static int nirtfeatures_pie_set_state(struct nirtfeatures *nirtfeatures,
				      unsigned int element,
				      unsigned int subelement, int state)
{
	union acpi_object pies_args[3];
	acpi_size result_size;
	void *result_buffer;
	union acpi_object *acpi_buffer;
	int err;

	if (!nirtfeatures)
		return -EINVAL;

	pies_args[0].type = ACPI_TYPE_INTEGER;
	pies_args[0].integer.value = element;
	pies_args[1].type = ACPI_TYPE_INTEGER;
	pies_args[1].integer.value = subelement;
	pies_args[2].type = ACPI_TYPE_INTEGER;
	pies_args[2].integer.value = state;

	/* evaluate PIES(element, subelement, value) ACPI method */
	err = nirtfeatures_call_acpi_method(nirtfeatures->acpi_device, "PIES",
					    3, &pies_args[0], &result_size,
					    &result_buffer);
	if (err)
		return err;

	acpi_buffer = (union acpi_object *)result_buffer;
	if (acpi_buffer->type == ACPI_TYPE_INTEGER)
		err = (int)acpi_buffer->integer.value;
	kfree(result_buffer);

	return err;
}

/* This is the generic PIE get state wrapper. It invokes the PIEG
 * ACPI method to query the state of the given PIE.
 */
static int nirtfeatures_pie_get_state(struct nirtfeatures *nirtfeatures,
				      unsigned int element,
				      unsigned int subelement, int *result)
{
	union acpi_object pies_args[2];
	acpi_size result_size;
	void *result_buffer;
	union acpi_object *acpi_buffer;
	int err;

	if (!nirtfeatures || !result)
		return -EINVAL;

	pies_args[0].type = ACPI_TYPE_INTEGER;
	pies_args[0].integer.value = element;
	pies_args[1].type = ACPI_TYPE_INTEGER;
	pies_args[1].integer.value = subelement;

	/* evaluate PIEG(element, subelement) ACPI method */
	err = nirtfeatures_call_acpi_method(nirtfeatures->acpi_device, "PIEG",
					    2, &pies_args[0], &result_size,
					    &result_buffer);
	if (err)
		return err;

	acpi_buffer = (union acpi_object *)result_buffer;
	if (acpi_buffer->type == ACPI_TYPE_INTEGER)
		*result = (int)acpi_buffer->integer.value;
	kfree(result_buffer);

	return 0;
}

/* This function enables or disables notifications for a particular
 * input class PIE.
 */
static int
nirtfeatures_pie_enable_notifications(struct nirtfeatures *nirtfeatures,
				      unsigned int element,
				      unsigned int subelement, int enable)
{
	union acpi_object pies_args[3];
	acpi_size result_size;
	void *result_buffer;
	union acpi_object *acpi_buffer;
	int err;

	if (!nirtfeatures)
		return -EINVAL;

	pies_args[0].type = ACPI_TYPE_INTEGER;
	pies_args[0].integer.value = element;
	pies_args[1].type = ACPI_TYPE_INTEGER;
	pies_args[1].integer.value = subelement;
	pies_args[2].type = ACPI_TYPE_INTEGER;
	pies_args[2].integer.value = enable;

	/* evaluate PIEF(element, subelement, enable) ACPI method */
	err = nirtfeatures_call_acpi_method(nirtfeatures->acpi_device, "PIEF",
					    3, &pies_args[0], &result_size,
					    &result_buffer);
	if (err)
		return err;

	acpi_buffer = (union acpi_object *)result_buffer;
	if (acpi_buffer->type == ACPI_TYPE_INTEGER)
		err = (int)acpi_buffer->integer.value;
	kfree(result_buffer);

	return err;
}

/* This is the set_brightness callback for a PIE-enumerated LED.
 */
static void
nirtfeatures_led_pie_brightness_set(struct led_classdev *led_cdev,
				    enum led_brightness brightness)
{
	struct nirtfeatures_led *led = (struct nirtfeatures_led *)led_cdev;

	spin_lock(&led->nirtfeatures->lock);

	/* Delegate the control of the PIE to the ACPI method. */
	if (nirtfeatures_pie_set_state(led->nirtfeatures,
				       led->pie_location.element,
				       led->pie_location.subelement,
				       brightness)) {
		dev_err(&led->nirtfeatures->acpi_device->dev,
			"set brightness failed for %s\n", led->name_string);
	}

	spin_unlock(&led->nirtfeatures->lock);
}

/* This is the get_brightness callback for a PIE-enumerated LED.
 */
static enum
led_brightness nirtfeatures_led_pie_brightness_get(struct led_classdev
						   *led_cdev)
{
	struct nirtfeatures_led *led = (struct nirtfeatures_led *)led_cdev;
	int state = 0;

	spin_lock(&led->nirtfeatures->lock);

	if (nirtfeatures_pie_get_state(led->nirtfeatures,
				       led->pie_location.element,
				       led->pie_location.subelement, &state)) {
		dev_err(&led->nirtfeatures->acpi_device->dev,
			"get brightness failed for %s\n", led->name_string);
	}

	spin_unlock(&led->nirtfeatures->lock);
	return state;
}

/* Parse a PIE LED color caps package and populate the
 * corresponding nirtfeatures_pie_descriptor_led_color structure.
 */
static int
nirtfeatures_parse_led_pie_color(unsigned int pie_caps_version,
				 struct nirtfeatures_pie_descriptor_led_color
				 *led_color_desc,
				 union acpi_object *acpi_buffer)
{
	union acpi_object *elements;
	unsigned int i;

	if (!led_color_desc || !acpi_buffer)
		return -EINVAL;

	elements = acpi_buffer->package.elements;

	if (elements[0].type != ACPI_TYPE_BUFFER ||
	    elements[1].type != ACPI_TYPE_INTEGER ||
	    elements[2].type != ACPI_TYPE_INTEGER)
		return -EINVAL;

	/* element 0 of a PIE LED color caps package is the name */
	for (i = 0; i < elements[0].buffer.length; i++) {
		/* get pointer to Nth Unicode character in name */
		unsigned short *unicode_char = (unsigned short *)
				(elements[0].buffer.pointer + (2 * i));
		/* naive convert to ASCII */
		led_color_desc->name[i] = (char)*unicode_char & 0xff;
	}

	/* element 1 is the brightness min value */
	led_color_desc->brightness_range_low = (int)elements[1].integer.value;

	/* element 2 is the brightness max value */
	led_color_desc->brightness_range_high = (int)elements[2].integer.value;

	return 0;
}

/* Parse a PIE LED caps package and create an LED class device
 * with the appropriate metadata.
 */
static int nirtfeatures_parse_led_pie(struct nirtfeatures *nirtfeatures,
				      unsigned int pie_caps_version,
				      unsigned int pie_element,
				      struct nirtfeatures_pie_descriptor *pie,
				      union acpi_object *acpi_buffer)
{
	struct nirtfeatures_pie_descriptor_led_color led_descriptor;
	struct nirtfeatures_led *led_dev;
	struct led_classdev *cdev;
	unsigned int num_colors;
	unsigned int i;
	int err;

	if (!nirtfeatures || !pie || !acpi_buffer ||
	    acpi_buffer->type != ACPI_TYPE_PACKAGE ||
	    acpi_buffer->package.elements[0].type != ACPI_TYPE_INTEGER)
		return -EINVAL;

	/* element 0 is the number of colors */
	num_colors = (unsigned)acpi_buffer->package.elements[0].integer.value;

	/* parse color caps and create LED class device */
	for (i = 0; i < num_colors; i++) {
		if (nirtfeatures_parse_led_pie_color(pie_caps_version,
						     &led_descriptor,
						     &(acpi_buffer->package.
						       elements[i + 1])))
			return -EINVAL;

		/* create an LED class device for this LED */
		led_dev = devm_kzalloc(&nirtfeatures->acpi_device->dev,
				       sizeof(*led_dev), GFP_KERNEL);
		if (!led_dev)
			return -ENOMEM;

		/*
		 * for compatibility with existing LVRT support, PIEs beginning
		 * with 'user' should not affix the uservisible attribute to
		 * their name
		 */
		if (strncasecmp(pie->name, "user", strlen("user")) != 0 &&
		    strncasecmp(pie->name, "wifi", strlen("wifi")) != 0)
			snprintf(led_dev->name_string, MAX_NODELEN,
				 "nilrt:%s:%s:uservisible=%d", pie->name,
				 led_descriptor.name, pie->is_user_visible);
		else
			snprintf(led_dev->name_string, MAX_NODELEN,
				 "nilrt:%s:%s", pie->name,
				 led_descriptor.name);

		cdev = &led_dev->cdev;
		cdev->name = led_dev->name_string;
		cdev->brightness = led_descriptor.brightness_range_low;
		cdev->max_brightness = led_descriptor.brightness_range_high;
		cdev->brightness_set = nirtfeatures_led_pie_brightness_set;
		cdev->brightness_get = nirtfeatures_led_pie_brightness_get;
		led_dev->nirtfeatures = nirtfeatures;
		led_dev->pie_location.element = pie_element;
		led_dev->pie_location.subelement = i;

		err =
		    devm_led_classdev_register(&nirtfeatures->acpi_device->dev,
					       cdev);
		if (err)
			return err;

		list_add_tail(&led_dev->node, &nirtfeatures_led_pie_list);
	}

	return 0;
}

/* Parse a PIE switch caps package and create an input class device
 * with the appropriate metadata.
 */
static int
nirtfeatures_parse_switch_pie(struct nirtfeatures *nirtfeatures,
			      unsigned int pie_caps_version,
			      unsigned int pie_element,
			      struct nirtfeatures_pie_descriptor *pie,
			      union acpi_object *acpi_buffer)
{
	struct nirtfeatures_switch *switch_dev;
	union acpi_object *elements;
	unsigned int num_states;
	unsigned int *states;
	unsigned int i;
	int err = 0;

	if (!nirtfeatures || !pie || !acpi_buffer ||
	    acpi_buffer->type != ACPI_TYPE_PACKAGE)
		return -EINVAL;

	elements = acpi_buffer->package.elements;

	/* element 0 is the number of states */
	if (elements[0].type != ACPI_TYPE_INTEGER)
		return -EINVAL;
	num_states = (unsigned)elements[0].integer.value;

	/* allocate storage for switch descriptor */
	states = kmalloc_array(num_states, sizeof(*states), GFP_KERNEL);
	if (!states)
		return -ENOMEM;

	/* parse individual states in elements 1..N-1 */
	for (i = 0; i < num_states; i++) {
		if (elements[i + 1].type != ACPI_TYPE_INTEGER) {
			err = -EINVAL;
			goto exit;
		}

		states[i] = (int)elements[i + 1].integer.value;
	}

	/* create an input class device for this switch */
	switch_dev = devm_kzalloc(&nirtfeatures->acpi_device->dev,
				  sizeof(*switch_dev), GFP_KERNEL);
	if (!switch_dev) {
		err = -ENOMEM;
		goto exit;
	}

	switch_dev->cdev =
	    devm_input_allocate_device(&nirtfeatures->acpi_device->dev);
	if (!switch_dev->cdev) {
		err = -ENOMEM;
		goto exit;
	}

	switch_dev->nirtfeatures = nirtfeatures;
	switch_dev->pie_location.element = pie_element;
	switch_dev->pie_location.subelement = 0;
	memcpy(&switch_dev->pie_descriptor, pie, sizeof(*pie));

	snprintf(switch_dev->name_string, MAX_NODELEN,
		 "nilrt:%s:uservisible=%d:states=(", pie->name,
		 pie->is_user_visible);
	for (i = 0; i < num_states; i++) {
		char temp[4] = { '\0' };

		sprintf(temp, "%d%c", states[i],
			(i < num_states - 1) ? ',' : ')');
		strncat(switch_dev->name_string, temp, MAX_NODELEN);
	}

	snprintf(switch_dev->phys_location_string, MAX_NODELEN, "nilrt/%s/%s",
		 nirtfeatures->bpstring, pie->name);

	switch_dev->cdev->name = switch_dev->name_string;
	switch_dev->cdev->phys = switch_dev->phys_location_string;
	switch_dev->cdev->id.bustype = BUS_HOST;
	switch_dev->cdev->id.vendor = 0x3923;
	switch_dev->cdev->id.product = pie->pie_type;
	switch_dev->cdev->id.version = pie_caps_version;
	switch_dev->cdev->dev.parent = &nirtfeatures->acpi_device->dev;
	switch_dev->cdev->evbit[0] = BIT_MASK(EV_KEY);
	set_bit(BTN_0, switch_dev->cdev->keybit);

	err = input_register_device(switch_dev->cdev);
	if (err) {
		input_free_device(switch_dev->cdev);
		goto exit;
	}

	/* if this PIE supports notifications, enable them now */
	if (pie->notification_value != 0) {
		err = nirtfeatures_pie_enable_notifications(nirtfeatures,
							    pie_element, 0, 1);
		if (err) {
			input_unregister_device(switch_dev->cdev);
			goto exit;
		}
	}

	/* add the new device to our list of switch PIEs */
	list_add_tail(&switch_dev->node, &nirtfeatures_switch_pie_list);
	goto exit;

exit:
	kfree(states);
	return err;
}

/* Parse a single PIE caps package from the PIEC buffer, determine the
 * type of PIE it is, then dispatch to the appropriate parsing routine.
 */
static int nirtfeatures_parse_pie(struct nirtfeatures *nirtfeatures,
				  unsigned int pie_caps_version,
				  unsigned int pie_element,
				  union acpi_object *acpi_buffer)
{
	struct nirtfeatures_pie_descriptor pie;
	union acpi_object *elements;
	unsigned int i;

	if (!nirtfeatures || !acpi_buffer ||
	    acpi_buffer->type != ACPI_TYPE_PACKAGE ||
	    acpi_buffer->package.count != 6)
		return -EINVAL;

	elements = acpi_buffer->package.elements;
	if (elements[0].type != ACPI_TYPE_BUFFER ||
	    elements[1].type != ACPI_TYPE_INTEGER ||
	    elements[2].type != ACPI_TYPE_INTEGER ||
	    elements[4].type != ACPI_TYPE_INTEGER ||
	    elements[5].type != ACPI_TYPE_INTEGER)
		return -EINVAL;

	/* element 0 of the package is the name */
	for (i = 0; i < elements[0].buffer.length && i < MAX_NAMELEN; i++) {
		/* get pointer to Nth Unicode character in name */
		unsigned short *unicode_char = (unsigned short *)
				(elements[0].buffer.pointer + (2 * i));
		/* naive convert to ASCII */
		pie.name[i] = (char)*unicode_char & 0xff;
	}

	/* element 1 of the package is the PIE class */
	pie.pie_class = (enum nirtfeatures_pie_class)
			elements[1].integer.value;

	/* element 2 of the package is the PIE type */
	pie.pie_type = (enum nirtfeatures_pie_type)
		       elements[2].integer.value;

	/* element 4 of an package is the visible flag */
	pie.is_user_visible = (elements[4].integer.value != 0);

	/* element 5 of the package is the notification value */
	pie.notification_value = elements[5].integer.value;

	/* parse the type-specific descriptor in element 3 */
	switch (pie.pie_type) {
	case PIE_TYPE_LED:
		if (nirtfeatures_parse_led_pie(nirtfeatures,
					       pie_caps_version, pie_element,
					       &pie, &(elements[3])))
			return -EINVAL;
		break;
	case PIE_TYPE_SWITCH:
		if (nirtfeatures_parse_switch_pie(nirtfeatures,
						  pie_caps_version,
						  pie_element, &pie,
						  &(elements[3])))
			return -EINVAL;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

/* Populate the list of physical interface elements from the table in
 * the DSDT and then generate the appropriate class devices.
 */
static int nirtfeatures_populate_pies(struct nirtfeatures *nirtfeatures)
{
	acpi_size result_size;
	void *result_buffer;
	union acpi_object *acpi_buffer;
	union acpi_object *elements;
	unsigned int pie_caps_version;
	unsigned int i;
	unsigned int err = 0;
	int ret;

	if (!nirtfeatures)
		return -EINVAL;

	/* get the PIE descriptor buffer from DSDT */
	ret = nirtfeatures_call_acpi_method(nirtfeatures->acpi_device, "PIEC",
					    0, NULL, &result_size,
					    &result_buffer);
	if (ret)
		return ret;

	acpi_buffer = (union acpi_object *)result_buffer;
	if (acpi_buffer->type != ACPI_TYPE_PACKAGE) {
		err = -EINVAL;
		goto exit;
	}

	elements = acpi_buffer->package.elements;
	if (elements[0].type != ACPI_TYPE_INTEGER ||
	    elements[1].type != ACPI_TYPE_INTEGER) {
		err = -EINVAL;
		goto exit;
	}

	/* the first element of the package is the caps version */
	pie_caps_version = (unsigned int)elements[0].integer.value;

	if (pie_caps_version < MIN_PIE_CAPS_VERSION ||
	    pie_caps_version > MAX_PIE_CAPS_VERSION) {
		dev_err(&nirtfeatures->acpi_device->dev,
			"invalid PIE caps version\n");
		err = -EINVAL;
		goto exit;
	}

	/* element 1 is not needed, parse elements 2..N as PIE descriptors */
	for (i = 2; i < acpi_buffer->package.count; i++) {
		err = nirtfeatures_parse_pie(nirtfeatures,
					     pie_caps_version, i - 2,
					     &(elements[i]));
		if (err)
			break;
	}

exit:
	kfree(result_buffer);
	return err;
}

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

	return 0;
}

static void nirtfeatures_remove_switch_pies(struct nirtfeatures *nirtfeatures)
{
	struct nirtfeatures_switch *cdev_iter;
	struct nirtfeatures_switch *temp;

	spin_lock(&nirtfeatures->lock);

	/* walk the list of switch devices and unregister/free each one */
	list_for_each_entry_safe(cdev_iter, temp,
				 &nirtfeatures_switch_pie_list, node) {
		/* disable notifications for this PIE if supported */
		if (cdev_iter->pie_descriptor.notification_value != 0) {
			nirtfeatures_pie_enable_notifications(nirtfeatures,
							      cdev_iter->
							      pie_location.
							      element,
							      cdev_iter->
							      pie_location.
							      subelement, 0);
		}
		input_unregister_device(cdev_iter->cdev);
		input_free_device(cdev_iter->cdev);
		kfree(cdev_iter);
	}

	spin_unlock(&nirtfeatures->lock);
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

/* Process a notification from ACPI, which typically occurs when a switch
 * PIE is signalling a change of state via its GPE.
 */
static void nirtfeatures_acpi_notify(struct acpi_device *device, u32 event)
{
	/* find the switch PIE for which this notification was generated,
	 * and push an event into its associated input subsystem node
	 */
	struct nirtfeatures *nirtfeatures = acpi_driver_data(device);
	struct nirtfeatures_switch *iter;
	int state;

	spin_lock(&nirtfeatures->lock);

	list_for_each_entry(iter, &nirtfeatures_switch_pie_list, node) {
		if (event == iter->pie_descriptor.notification_value) {
			/* query instantaneous switch state */
			if (!nirtfeatures_pie_get_state(iter->nirtfeatures,
			    iter->pie_location.element,
			    iter->pie_location.subelement,
			    &state)) {
				/* push current state of switch */
				input_report_key(iter->cdev, BTN_0, !!state);
				input_sync(iter->cdev);
			}
			spin_unlock(&nirtfeatures->lock);
			return;
		}
	}

	spin_unlock(&nirtfeatures->lock);

	dev_err(&device->dev, "no input found for notification (event %02X)\n",
		event);
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
		break;
	case NIRTF_PLATFORM_MISC_ID_WINGHEAD:
		nirtfeatures->bpstring = "Winghead";
		break;
	default:
		dev_err(&nirtfeatures->acpi_device->dev,
			"Unrecognized backplane type %u\n", bpinfo);
		nirtfeatures->bpstring = "Unknown";
		break;
	}

	spin_lock_init(&nirtfeatures->lock);

	err = nirtfeatures_populate_pies(nirtfeatures);
	if (err) {
		dev_err(&device->dev, "could not populate PIEs\n");
		return err;
	}

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
	struct nirtfeatures *nirtfeatures = acpi_driver_data(device);

	sysfs_remove_files(&device->dev.kobj, nirtfeatures_attrs);
	nirtfeatures_remove_switch_pies(nirtfeatures);

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
		.notify = nirtfeatures_acpi_notify,
	},
};

module_acpi_driver(nirtfeatures_acpi_driver);

MODULE_DEVICE_TABLE(acpi, nirtfeatures_device_ids);
MODULE_DESCRIPTION("NI RT Features");
MODULE_AUTHOR("Jeff Westfahl <jeff.westfahl@ni.com>");
MODULE_LICENSE("GPL");
