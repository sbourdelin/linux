// SPDX-License-Identifier: GPL-2.0
/*
 * wilco_ec_sysfs - Sysfs attributes for Wilco Embedded Controller
 *
 * Copyright 2018 Google LLC
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/sysfs.h>
#include "wilco_ec.h"
#include "wilco_ec_legacy.h"
#include "wilco_ec_properties.h"

#define WILCO_EC_ATTR_RO(_name)						\
__ATTR(_name, 0444, wilco_ec_##_name##_show, NULL)

#define WILCO_EC_ATTR_WO(_name)						\
__ATTR(_name, 0200, NULL, wilco_ec_##_name##_store)

#define WILCO_EC_ATTR_RW(_name)						\
__ATTR(_name, 0644, wilco_ec_##_name##_show, wilco_ec_##_name##_store)

/* Make top-level attributes, which will live inside GOOG000C:00/ */

static struct device_attribute version_attr = WILCO_EC_ATTR_RO(version);
static struct device_attribute stealth_attr = WILCO_EC_ATTR_WO(stealth_mode);
#ifdef CONFIG_WILCO_EC_SYSFS_RAW
static struct device_attribute raw_attr = WILCO_EC_ATTR_RW(raw);
#endif

static struct attribute *wilco_ec_toplevel_attrs[] = {
	&version_attr.attr,
	&stealth_attr.attr,
#ifdef CONFIG_WILCO_EC_SYSFS_RAW
	&raw_attr.attr,
#endif
	NULL
};

ATTRIBUTE_GROUPS(wilco_ec_toplevel);

/* Make property attributes, which will live inside GOOG000C:00/properties/  */

BOOLEAN_PROPERTY_RW_ATTRIBUTE(OP_SET, bool_prop_attr_global_mic_mute_led,
			      global_mic_mute_led, PID_GLOBAL_MIC_MUTE_LED);
BOOLEAN_PROPERTY_RW_ATTRIBUTE(OP_SET, bool_prop_attr_fn_lock, fn_lock,
			      PID_FN_LOCK);
BOOLEAN_PROPERTY_RW_ATTRIBUTE(OP_SET, bool_prop_attr_nic, nic, PID_NIC);
BOOLEAN_PROPERTY_RW_ATTRIBUTE(OP_SET, bool_prop_attr_ext_usb_port_en,
			      ext_usb_port_en, PID_EXT_USB_PORT_EN);
BOOLEAN_PROPERTY_WO_ATTRIBUTE(OP_SYNC, bool_prop_attr_wireless_sw_wlan,
			      wireless_sw_wlan, PID_WIRELESS_SW_WLAN);
BOOLEAN_PROPERTY_RW_ATTRIBUTE(OP_SET,
			      bool_prop_attr_auto_boot_on_trinity_dock_attach,
			      auto_boot_on_trinity_dock_attach,
			      PID_AUTO_BOOT_ON_TRINITY_DOCK_ATTACH);
BOOLEAN_PROPERTY_RW_ATTRIBUTE(OP_SET, bool_prop_attr_ich_azalia_en,
			      ich_azalia_en, PID_ICH_AZALIA_EN);
BOOLEAN_PROPERTY_RW_ATTRIBUTE(OP_SET, bool_prop_attr_sign_of_life_kbbl,
			      sign_of_life_kbbl, PID_SIGN_OF_LIFE_KBBL);

struct attribute *wilco_ec_property_attrs[] = {
	&bool_prop_attr_global_mic_mute_led.kobj_attr.attr,
	&bool_prop_attr_fn_lock.kobj_attr.attr,
	&bool_prop_attr_nic.kobj_attr.attr,
	&bool_prop_attr_ext_usb_port_en.kobj_attr.attr,
	&bool_prop_attr_wireless_sw_wlan.kobj_attr.attr,
	&bool_prop_attr_auto_boot_on_trinity_dock_attach.kobj_attr.attr,
	&bool_prop_attr_ich_azalia_en.kobj_attr.attr,
	&bool_prop_attr_sign_of_life_kbbl.kobj_attr.attr,
	NULL
};

ATTRIBUTE_GROUPS(wilco_ec_property);
struct kobject *prop_dir_kobj;


/**
 * wilco_ec_sysfs_init() - Initialize the sysfs directories and attributes
 * @dev: The device representing the EC
 *
 * Creates the sysfs directory structure and populates it with all attributes.
 * If there is a problem it will clean up the entire filesystem.
 *
 * Return 0 on success, -ENOMEM on failure creating directories or attibutes.
 */
int wilco_ec_sysfs_init(struct wilco_ec_device *ec)
{
	struct device *dev = ec->dev;
	int ret;

	// add the top-level attributes
	ret = sysfs_create_groups(&dev->kobj, wilco_ec_toplevel_groups);
	if (ret)
		goto err;

	// add the directory for properties
	prop_dir_kobj = kobject_create_and_add("properties", &dev->kobj);
	if (!prop_dir_kobj)
		goto rm_toplevel_attrs;

	// add the property attributes into the properties directory
	ret = sysfs_create_groups(prop_dir_kobj, wilco_ec_property_groups);
	if (ret)
		goto rm_properties_dir;

	return 0;

/* Go upwards through the directory structure, cleaning up */
rm_properties_dir:
	kobject_put(prop_dir_kobj);
rm_toplevel_attrs:
	sysfs_remove_groups(&dev->kobj, wilco_ec_toplevel_groups);
err:
	dev_err(dev, "Failed to create sysfs filesystem!");
	return -ENOMEM;
}

void wilco_ec_sysfs_remove(struct wilco_ec_device *ec)
{
	struct device *dev = ec->dev;

	/* go upwards through the directory structure */
	sysfs_remove_groups(prop_dir_kobj, wilco_ec_property_groups);
	kobject_put(prop_dir_kobj);
	sysfs_remove_groups(&dev->kobj, wilco_ec_toplevel_groups);
}
