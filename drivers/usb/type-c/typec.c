/*
 * USB Type-C class
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Heikki Krogerus <heikki.krogerus@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb/typec.h>

#define to_typec_port(p) container_of(p, struct typec_port, dev)

static DEFINE_IDA(typec_index_ida);

/* -------------------------------- */

int typec_connect(struct typec_port *port)
{
	kobject_uevent(&port->dev.kobj, KOBJ_CHANGE);

	return 0;
}
EXPORT_SYMBOL_GPL(typec_connect);

void typec_disconnect(struct typec_port *port)
{
	kobject_uevent(&port->dev.kobj, KOBJ_CHANGE);
}
EXPORT_SYMBOL_GPL(typec_disconnect);

/* -------------------------------- */

static ssize_t alternate_mode_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t size)
{
	struct typec_port *port = to_typec_port(dev);
	struct typec_alt_mode alt_mode;
	int ret;

	if (!port->cap->set_alt_mode) {
		dev_warn(dev, "entering Alternate Modes not supported\n");
		return -EOPNOTSUPP;
	}

	if (!port->connected)
		return -ENXIO;

	if (sscanf(buf, "0x%hx,%u", &alt_mode.svid, &alt_mode.mid) != 2)
		return -EINVAL;

	mutex_lock(&port->lock);
	ret = port->cap->set_alt_mode(port, &alt_mode);
	mutex_unlock(&port->lock);
	if (ret)
		return ret;

	return size;
}

static ssize_t alternate_mode_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	if (!port->cur_alt_mode)
		return sprintf(buf, "none\n");

	/* REVISIT: SIDs in human readable form? */
	return sprintf(buf, "0x%hx,%u\n", port->cur_alt_mode->svid,
		       port->cur_alt_mode->mid);
}
static DEVICE_ATTR_RW(alternate_mode);

static ssize_t alternate_modes_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct typec_port *port = to_typec_port(dev);
	struct typec_alt_mode *alt_mode;
	int len = 0;

	if (!port->cap->alt_modes)
		return sprintf(buf, "none\n");

	/* REVISIT: SIDs in human readable form? */
	for (alt_mode = port->cap->alt_modes; alt_mode->svid; alt_mode++)
		len += sprintf(buf + len, "0x%hx,%u\n", alt_mode->svid,
			       alt_mode->mid);

	buf[len - 1] = '\0';
	return len;
}
static DEVICE_ATTR_RO(alternate_modes);

static ssize_t partner_alt_modes_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct typec_port *port = to_typec_port(dev);
	struct typec_alt_mode *alt_mode;
	int len = 0;

	if (!port->connected)
		return -ENXIO;

	if (!port->partner_alt_modes)
		return sprintf(buf, "none\n");

	/* REVISIT: SIDs in human readable form? */
	for (alt_mode = port->partner_alt_modes; alt_mode->svid; alt_mode++)
		len += sprintf(buf + len, "0x%hx,%u\n", alt_mode->svid,
			       alt_mode->mid);

	buf[len - 1] = '\0';
	return len;
}
static DEVICE_ATTR_RO(partner_alt_modes);

static const char * const typec_partner_types[] = {
	[TYPEC_PARTNER_NONE] = "unknown",
	[TYPEC_PARTNER_USB] = "USB",
	[TYPEC_PARTNER_CHARGER] = "Charger",
	[TYPEC_PARTNER_ALTMODE] = "Alternate Mode",
	[TYPEC_PARTNER_AUDIO] = "Audio Accessory",
	[TYPEC_PARTNER_DEBUG] = "Debug Accessroy",
};

static ssize_t partner_type_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	if (!port->connected)
		return -ENXIO;

	return sprintf(buf, "%s\n", typec_partner_types[port->partner_type]);
}
static DEVICE_ATTR_RO(partner_type);

static ssize_t data_role_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t size)
{
	struct typec_port *port = to_typec_port(dev);
	enum typec_data_role role;
	int ret;

	if (port->cap->type != TYPEC_PORT_DRP) {
		dev_dbg(dev, "data role swap only supported with DRP ports\n");
		return -EOPNOTSUPP;
	}

	if (!port->cap->dr_swap) {
		dev_warn(dev, "data role swapping not supported\n");
		return -EOPNOTSUPP;
	}

	if (!port->connected)
		return -ENXIO;

	if (!strncmp(buf, "host", 4))
		role = TYPEC_HOST;
	else if (!strncmp(buf, "device", 6))
		role = TYPEC_DEVICE;
	else
		return -EINVAL;

	if (port->data_role == role)
		goto out;

	mutex_lock(&port->lock);
	ret = port->cap->dr_swap(port);
	mutex_unlock(&port->lock);
	if (ret)
		return ret;
out:
	return size;
}

static ssize_t data_role_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	switch (port->cap->type) {
	case TYPEC_PORT_DFP:
		return sprintf(buf, "host\n");
	case TYPEC_PORT_UFP:
		return sprintf(buf, "device\n");
	case TYPEC_PORT_DRP:
		return sprintf(buf, "%s\n", port->data_role == TYPEC_HOST ?
			       "host" : "device");
	default:
		return sprintf(buf, "unknown\n");
	};
}
static DEVICE_ATTR_RW(data_role);

static ssize_t data_roles_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	if (port->cap->type == TYPEC_PORT_DRP)
		return sprintf(buf, "host, device\n");

	return data_role_show(dev, attr, buf);
}
static DEVICE_ATTR_RO(data_roles);

static ssize_t power_role_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t size)
{
	struct typec_port *port = to_typec_port(dev);
	enum typec_pwr_role role;
	int ret;

	if (!port->cap->usb_pd) {
		dev_dbg(dev, "power role swap only supported with USB PD\n");
		return -EOPNOTSUPP;
	}

	if (!port->cap->pr_swap) {
		dev_warn(dev, "power role swapping not supported\n");
		return -EOPNOTSUPP;
	}

	if (!port->connected)
		return -ENXIO;

	if (port->pwr_opmode != TYPEC_PWR_MODE_PD) {
		dev_dbg(dev, "partner unable to swap power role\n");
		return -EIO;
	}

	if (!strncmp(buf, "source", 6))
		role = TYPEC_PWR_SOURCE;
	else if (!strncmp(buf, "sink", 4))
		role = TYPEC_PWR_SINK;
	else
		return -EINVAL;

	if (port->pwr_role == role)
		return size;

	mutex_lock(&port->lock);
	ret = port->cap->pr_swap(port);
	mutex_unlock(&port->lock);
	if (ret)
		return ret;

	return size;
}

static ssize_t power_role_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	switch (port->pwr_role) {
	case TYPEC_PWR_SOURCE:
		return sprintf(buf, "source\n");
	case TYPEC_PWR_SINK:
		return sprintf(buf, "sink\n");
	default:
		return sprintf(buf, "unknown\n");
	};
}
static DEVICE_ATTR_RW(power_role);

static ssize_t power_roles_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	if (port->cap->usb_pd || port->cap->type == TYPEC_PORT_DRP)
		return sprintf(buf, "source, sink\n");

	return power_role_show(dev, attr, buf);
}
static DEVICE_ATTR_RO(power_roles);

static const char * const typec_pwr_opmodes[] = {
	[TYPEC_PWR_MODE_USB] = "USB",
	[TYPEC_PWR_MODE_BC1_2] = "BC1.2",
	[TYPEC_PWR_MODE_1_5A] = "USB Type-C 1.5A",
	[TYPEC_PWR_MODE_3_0A] = "USB Type-C 3.0A",
	[TYPEC_PWR_MODE_PD] = "USB PD",
};

static ssize_t power_operation_mode_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%s\n", typec_pwr_opmodes[port->pwr_opmode]);
}
static DEVICE_ATTR_RO(power_operation_mode);

static ssize_t connected_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%s\n", port->connected ? "yes" : "no");
}
static DEVICE_ATTR_RO(connected);

static ssize_t usb_pd_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%ssupported\n", port->cap->usb_pd ? "" : "not ");
}
static DEVICE_ATTR_RO(usb_pd);

static ssize_t audio_accessory_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%ssupported\n", port->cap->audio_accessory ?
		       "" : "not ");
}
static DEVICE_ATTR_RO(audio_accessory);

static ssize_t debug_accessory_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%ssupported\n", port->cap->debug_accessory ?
		       "" : "not ");
}
static DEVICE_ATTR_RO(debug_accessory);

/* REVISIT: Consider creating the partner dependent sysfs files at runtime. */
static struct attribute *typec_attrs[] = {
	&dev_attr_alternate_mode.attr,
	&dev_attr_alternate_modes.attr,
	&dev_attr_partner_alt_modes.attr,
	&dev_attr_partner_type.attr,
	&dev_attr_data_role.attr,
	&dev_attr_data_roles.attr,
	&dev_attr_power_role.attr,
	&dev_attr_power_roles.attr,
	&dev_attr_power_operation_mode.attr,
	&dev_attr_connected.attr,
	&dev_attr_usb_pd.attr,
	&dev_attr_audio_accessory.attr,
	&dev_attr_debug_accessory.attr,
	NULL,
};
ATTRIBUTE_GROUPS(typec);

static int typec_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	int ret;

	ret = add_uevent_var(env, "TYPEC_PORT=%s", dev_name(dev));
	if (ret)
		dev_err(dev, "failed to add uevent TYPEC_PORT\n");

	return ret;
}

static void typec_release(struct device *dev)
{
	struct typec_port *port = to_typec_port(dev);

	ida_simple_remove(&typec_index_ida, port->id);
	kfree(port);
}

static struct class typec_class = {
	.name = "type-c",
	.dev_uevent = typec_uevent,
	.dev_groups = typec_groups,
	.dev_release = typec_release,
};

struct typec_port *typec_register_port(struct device *dev,
				       struct typec_capability *cap)
{
	struct typec_port *port;
	int ret;
	int id;

	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return ERR_PTR(-ENOMEM);

	id = ida_simple_get(&typec_index_ida, 0, 0, GFP_KERNEL);
	if (id < 0) {
		kfree(port);
		return ERR_PTR(id);
	}

	port->id = id;
	port->cap = cap;
	port->dev.class = &typec_class;
	port->dev.parent = dev;
	dev_set_name(&port->dev, "usbc%d", id);
	mutex_init(&port->lock);

	ret = device_register(&port->dev);
	if (ret) {
		ida_simple_remove(&typec_index_ida, id);
		put_device(&port->dev);
		kfree(port);
		return ERR_PTR(ret);
	}

	return port;
}
EXPORT_SYMBOL_GPL(typec_register_port);

void typec_unregister_port(struct typec_port *port)
{
	device_unregister(&port->dev);
}
EXPORT_SYMBOL_GPL(typec_unregister_port);

static int __init typec_init(void)
{
	return class_register(&typec_class);
}
subsys_initcall(typec_init);

static void __exit typec_exit(void)
{
	return class_unregister(&typec_class);
}
module_exit(typec_exit);

MODULE_AUTHOR("Heikki Krogerus <heikki.krogerus@linux.intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("USB Type-C Connector Class");
