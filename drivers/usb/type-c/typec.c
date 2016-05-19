/*
 * USB Type-C Connector Class
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

struct typec_port {
	unsigned int		id;
	struct device		dev;
	struct mutex		lock; /* FIXME: Not in use yet. */

	enum typec_usb_role	usb_role;
	enum typec_pwr_role	pwr_role;
	enum typec_pwr_role	vconn_role;
	enum typec_pwr_opmode	pwr_opmode;

	struct typec_partner	*partner;
	struct typec_cable	*cable;

	unsigned int		connected:1;

	int			n_altmode;

	enum typec_data_role	fixed_role;
	const struct typec_capability *cap;
};

#define to_typec_port(p) container_of(p, struct typec_port, dev)

static DEFINE_IDA(typec_index_ida);

static struct class typec_class = {
	.name = "type-c",
};

/* -------------------------------- */
/* Type-C Partners */

static void typec_dev_release(struct device *dev)
{
}

static const char * const typec_partner_types[] = {
	[TYPEC_PARTNER_USB] = "USB",
	[TYPEC_PARTNER_CHARGER] = "Charger",
	[TYPEC_PARTNER_ALTMODE] = "Alternate Mode",
	[TYPEC_PARTNER_AUDIO] = "Audio Accessory",
	[TYPEC_PARTNER_DEBUG] = "Debug Accessory",
};

static ssize_t partner_type_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct typec_partner *partner = container_of(dev, struct typec_partner,
						     dev);

	return sprintf(buf, "%s\n", typec_partner_types[partner->type]);
}

static struct device_attribute dev_attr_partner_type = {
	.attr = {
		.name = "type",
		.mode = S_IRUGO,
	},
	.show = partner_type_show,
};

static struct attribute *typec_partner_attrs[] = {
	&dev_attr_partner_type.attr,
	NULL
};

static struct attribute_group typec_partner_group = {
	.attrs = typec_partner_attrs,
};

static const struct attribute_group *typec_partner_groups[] = {
	&typec_partner_group,
	NULL
};

static struct device_type typec_partner_dev_type = {
	.name = "typec_partner_device",
	.groups = typec_partner_groups,
	.release = typec_dev_release,
};

static int
typec_add_partner(struct typec_port *port, struct typec_partner *partner)
{
	struct device *dev = &partner->dev;
	struct device *parent;
	int ret;

	/*
	 * REVISIT: Maybe it would be better to make the port always as the
	 * parent of the partner? Or not even that. Would it be enough to just
	 * create the symlink to the partner like we do below in any case?
	 */
	if (port->cable) {
		if (port->cable->active) {
			if (port->cable->sop_pp_controller)
				parent = &port->cable->plug[1].dev;
			else
				parent = &port->cable->plug[0].dev;
		} else {
			parent = &port->cable->dev;
		}
	} else {
		parent = &port->dev;
	}

	dev->class = &typec_class;
	dev->parent = parent;
	dev->type = &typec_partner_dev_type;
	dev_set_name(dev, "%s-partner", dev_name(&port->dev));

	ret = device_register(dev);
	if (ret) {
		put_device(dev);
		return ret;
	}

	ret = typec_register_altmodes(dev, partner->alt_modes);
	if (ret) {
		device_unregister(dev);
		return ret;
	}

	/* REVISIT: Creating symlink for the port device for now. */
	ret = sysfs_create_link(&port->dev.kobj, &dev->kobj, "partner");
	if (ret)
		dev_WARN(&port->dev, "failed to create link to %s (%d)\n",
			 dev_name(dev), ret);

	port->partner = partner;
	return 0;
}

static void typec_remove_partner(struct typec_port *port)
{
	sysfs_remove_link(&port->dev.kobj, "partner");
	typec_unregister_altmodes(port->partner->alt_modes);
	device_unregister(&port->partner->dev);
}

/* -------------------------------- */
/* Type-C Cable Plugs */

static struct device_type typec_plug_dev_type = {
	.name = "type_plug_device",
	.release = typec_dev_release,
};

static int
typec_add_plug(struct typec_port *port, struct typec_plug *plug)
{
	struct device *dev = &plug->dev;
	char name[6];
	int ret;

	sprintf(name, "plug%d", plug->index);

	dev->class = &typec_class;
	dev->parent = &port->cable->dev;
	dev->type = &typec_plug_dev_type;
	dev_set_name(dev, "%s-%s", dev_name(&port->dev), name);

	ret = device_register(dev);
	if (ret) {
		put_device(dev);
		return ret;
	}

	ret = typec_register_altmodes(dev, plug->alt_modes);
	if (ret) {
		device_unregister(dev);
		return ret;
	}

	/* REVISIT: Is this useful? */
	ret = sysfs_create_link(&port->dev.kobj, &dev->kobj, name);
	if (ret)
		dev_WARN(&port->dev, "failed to create link to %s (%d)\n",
			 dev_name(dev), ret);

	return 0;
}

static void typec_remove_plug(struct typec_plug *plug)
{
	struct typec_port *port = to_typec_port(plug->dev.parent->parent);
	char name[6];

	sprintf(name, "plug%d", plug->index);
	sysfs_remove_link(&port->dev.kobj, name);
	typec_unregister_altmodes(plug->alt_modes);
	device_unregister(&plug->dev);
}

static ssize_t
active_cable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct typec_cable *cable = container_of(dev, struct typec_cable, dev);

	return sprintf(buf, "%d\n", cable->active);
}

static struct device_attribute dev_attr_active_cable = {
	.attr = {
		.name = "active",
		.mode = S_IRUGO,
	},
	.show = active_cable_show,
};

static const char * const typec_plug_types[] = {
	[USB_PLUG_NONE] = "unknown",
	[USB_PLUG_TYPE_A] = "Type-A",
	[USB_PLUG_TYPE_B] = "Type-B",
	[USB_PLUG_TYPE_C] = "Type-C",
	[USB_PLUG_CAPTIVE] = "Captive",
};

static ssize_t
cable_plug_type_show(struct device *dev, struct device_attribute *attr,
		     char *buf)
{
	struct typec_cable *cable = container_of(dev, struct typec_cable, dev);

	return sprintf(buf, "%s\n", typec_plug_types[cable->type]);
}

static struct device_attribute dev_attr_plug_type = {
	.attr = {
		.name = "plug_type",
		.mode = S_IRUGO,
	},
	.show = cable_plug_type_show,
};

static struct attribute *typec_cable_attrs[] = {
	&dev_attr_active_cable.attr,
	&dev_attr_plug_type.attr,
	NULL
};

static struct attribute_group typec_cable_group = {
	.attrs = typec_cable_attrs,
};

static const struct attribute_group *typec_cable_groups[] = {
	&typec_cable_group,
	NULL
};

static struct device_type typec_cable_dev_type = {
	.name = "typec_cable_device",
	.groups = typec_cable_groups,
	.release = typec_dev_release,
};

static int typec_add_cable(struct typec_port *port, struct typec_cable *cable)
{
	struct device *dev = &cable->dev;
	int ret;

	dev->class = &typec_class;
	/* REVISIT: We could have just the symlink also for the cable. */
	dev->parent = &port->dev;
	dev->type = &typec_cable_dev_type;
	dev_set_name(dev, "%s-cable", dev_name(&port->dev));

	ret = device_register(dev);
	if (ret) {
		put_device(dev);
		return ret;
	}

	/* Plug1 */
	if (!cable->active)
		return 0;

	cable->plug[0].index = 1;
	ret = typec_add_plug(port, &cable->plug[0]);
	if (ret) {
		device_unregister(dev);
		return ret;
	}

	/* Plug2 */
	if (!cable->sop_pp_controller)
		return 0;

	cable->plug[1].index = 2;
	ret = typec_add_plug(port, &cable->plug[1]);
	if (ret) {
		typec_remove_plug(&cable->plug[0]);
		device_unregister(dev);
		return ret;
	}

	port->cable = cable;
	return 0;
}

static void typec_remove_cable(struct typec_port *port)
{
	if (port->cable->active) {
		typec_remove_plug(&port->cable->plug[0]);
		if (port->cable->sop_pp_controller)
			typec_remove_plug(&port->cable->plug[1]);
	}
	device_unregister(&port->cable->dev);
}

/* -------------------------------- */

int typec_connect(struct typec_port *port, struct typec_connection *con)
{
	int ret;

	/* FIXME: bus_type for typec? Note that we will in any case have bus for
	 * the alternate modes. typec bus would be only dealing with the cable
	 * and partner. */

	if (!con->partner && !con->cable)
		return -EINVAL;

	port->connected = 1;
	port->usb_role = con->usb_role;
	port->pwr_role = con->pwr_role;
	port->vconn_role = con->vconn_role;
	port->pwr_opmode = con->pwr_opmode;

	kobject_uevent(&port->dev.kobj, KOBJ_CHANGE);

	if (con->cable) {
		ret = typec_add_cable(port, con->cable);
		if (ret)
			return ret;
	}

	if (con->partner) {
		ret = typec_add_partner(port, con->partner);
		if (ret) {
			if (con->cable)
				typec_remove_cable(port);
			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(typec_connect);

void typec_disconnect(struct typec_port *port)
{
	if (port->partner)
		typec_remove_partner(port);

	if (port->cable)
		typec_remove_cable(port);

	port->connected = 0;
	port->partner = NULL;
	port->cable = NULL;

	port->pwr_opmode = TYPEC_PWR_MODE_USB;

	if (port->fixed_role == TYPEC_PORT_DFP) {
		port->usb_role = TYPEC_HOST;
		port->pwr_role = TYPEC_PWR_SOURCE;
		port->vconn_role = TYPEC_PWR_SOURCE;
	} else {
		/* Device mode as default also with DRP ports */
		port->usb_role = TYPEC_DEVICE;
		port->pwr_role = TYPEC_PWR_SINK;
		port->vconn_role = TYPEC_PWR_SINK;
	}

	kobject_uevent(&port->dev.kobj, KOBJ_CHANGE);
}
EXPORT_SYMBOL_GPL(typec_disconnect);

struct device *typec_port2dev(struct typec_port *port)
{
	return &port->dev;
}
EXPORT_SYMBOL_GPL(typec_port2dev);

struct typec_port *typec_dev2port(struct device *dev)
{
	return to_typec_port(dev);
}
EXPORT_SYMBOL_GPL(typec_dev2port);

/* -------------------------------- */
/* Alternate Modes */

/*
 * typec_altmode2port - Alternate Mode to USB Type-C port
 * @alt: The Alternate Mode
 *
 * Returns the port that the cable plug or partner with @alt is connected to.
 * The function is helper only for cable plug and partner Alternate Modes. With
 * Type-C port Alternate Modes the function returns NULL.
 */
struct typec_port *typec_altmode2port(struct typec_altmode *alt)
{
	if (alt->dev.parent->type == &typec_plug_dev_type)
		return to_typec_port(alt->dev.parent->parent->parent);
	if (alt->dev.parent->type == &typec_partner_dev_type)
		return to_typec_port(alt->dev.parent->parent);

	return NULL;
}
EXPORT_SYMBOL_GPL(typec_altmode2port);

static void typec_altmode_release(struct device *dev)
{
	struct typec_altmode *alt = to_altmode(dev);

	kfree(alt->mode_groups);
}

static ssize_t
typec_altmode_vdo_show(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	struct typec_mode *mode = container_of(attr, struct typec_mode,
					       vdo_attr);

	return sprintf(buf, "0x%08x\n", mode->vdo);
}

static ssize_t
typec_altmode_desc_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct typec_mode *mode = container_of(attr, struct typec_mode,
					       desc_attr);

	return sprintf(buf, "%s\n", mode->desc);
}

static ssize_t
typec_altmode_active_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct typec_mode *mode = container_of(attr, struct typec_mode,
					       active_attr);

	return sprintf(buf, "%d\n", mode->active);
}

static void typec_init_modes(struct typec_altmode *alt)
{
	struct typec_mode *mode = alt->modes;
	int i;

	for (i = 0; i < alt->n_modes; i++, mode++) {
		mode->index = i;
		sprintf(mode->group_name, "mode%d", i);

		sysfs_attr_init(&mode->vdo_attr.attr);
		mode->vdo_attr.attr.name = "vdo";
		mode->vdo_attr.attr.mode = S_IRUGO;
		mode->vdo_attr.show = typec_altmode_vdo_show;

		sysfs_attr_init(&mode->desc_attr.attr);
		mode->desc_attr.attr.name = "description";
		mode->desc_attr.attr.mode = S_IRUGO;
		mode->desc_attr.show = typec_altmode_desc_show;

		sysfs_attr_init(&mode->active_attr.attr);
		mode->active_attr.attr.name = "active";
		mode->active_attr.attr.mode = S_IRUGO;
		mode->active_attr.show = typec_altmode_active_show;

		mode->attrs[0] = &mode->vdo_attr.attr;
		mode->attrs[1] = &mode->desc_attr.attr;
		mode->attrs[2] = &mode->active_attr.attr;

		mode->group.attrs = mode->attrs;
		mode->group.name = mode->group_name;

		alt->mode_groups[i] = &mode->group;
	}
}

static int typec_add_altmode(struct device *parent, struct typec_altmode *alt)
{
	struct device *dev = &alt->dev;
	int ret;

	alt->mode_groups = kcalloc(alt->n_modes + 1,
				   sizeof(struct attibute_group *), GFP_KERNEL);
	if (!alt->mode_groups)
		return -ENOMEM;

	typec_init_modes(alt);

	dev->groups = alt->mode_groups;
	dev->release = typec_altmode_release;

	dev->parent = parent;
	/* TODO: dev->bus = &typec_altmode_bus; */

	if (alt->name)
		dev_set_name(dev, "%s.%s", dev_name(parent), alt->name);
	else
		dev_set_name(dev, "%s.svid:%04x", dev_name(parent), alt->svid);

	ret = device_register(dev);
	if (ret) {
		put_device(dev);
		kfree(alt->mode_groups);
		return ret;
	}

	return 0;
}

int typec_register_altmodes(struct device *parent,
			    struct typec_altmode *alt_modes)
{
	struct typec_altmode *alt;
	int index;
	int ret;

	if (!alt_modes)
		return 0;

	for (alt = alt_modes, index = 0; alt->svid; alt++, index++) {
		ret = typec_add_altmode(parent, alt);
		if (ret)
			goto err;
	}

	return 0;
err:
	for (alt = alt_modes + index; index; alt--, index--)
		device_unregister(&alt->dev);

	return ret;
}
EXPORT_SYMBOL_GPL(typec_register_altmodes);

void typec_unregister_altmodes(struct typec_altmode *alt_modes)
{
	struct typec_altmode *alt;

	for (alt = alt_modes; alt->svid; alt++)
		device_unregister(&alt->dev);
}
EXPORT_SYMBOL_GPL(typec_unregister_altmodes);

/* -------------------------------- */

static ssize_t
current_usb_data_role_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t size)
{
	struct typec_port *port = to_typec_port(dev);
	enum typec_usb_role role;
	int ret;

	if (port->cap->role != TYPEC_PORT_DRP) {
		dev_dbg(dev, "data role swap only supported with DRP ports\n");
		return -EOPNOTSUPP;
	}

	if (!port->cap->dr_swap) {
		dev_warn(dev, "data role swapping not supported\n");
		return -EOPNOTSUPP;
	}

	if (!strncmp(buf, "host", 4))
		role = TYPEC_HOST;
	else if (!strncmp(buf, "device", 6))
		role = TYPEC_DEVICE;
	else
		return -EINVAL;

	if (port->usb_role == role || !port->partner)
		return size;

	ret = port->cap->dr_swap(port);
	if (ret)
		return ret;

	return size;
}

static ssize_t
current_usb_data_role_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	if (port->usb_role == TYPEC_DEVICE)
		return sprintf(buf, "device\n");

	return sprintf(buf, "host\n");
}
static DEVICE_ATTR_RW(current_usb_data_role);

static const char * const typec_data_roles[] = {
	[TYPEC_PORT_DFP] = "DFP",
	[TYPEC_PORT_UFP] = "UFP",
	[TYPEC_PORT_DRP] = "DRP",
};

static ssize_t supported_data_roles_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%s\n", typec_data_roles[port->cap->role]);
}
static DEVICE_ATTR_RO(supported_data_roles);

static ssize_t
current_data_role_store(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t size)
{
	struct typec_port *port = to_typec_port(dev);
	enum typec_data_role role;
	int ret;

	if (port->cap->role != TYPEC_PORT_DRP)
		return -EOPNOTSUPP;

	if (!port->cap->fix_role)
		return -EOPNOTSUPP;

	if (!strcmp(buf, "DFP"))
		role = TYPEC_PORT_DFP;
	else if (!strcmp(buf, "UFP"))
		role = TYPEC_PORT_UFP;
	else if (!strcmp(buf, "DRP"))
		role = TYPEC_PORT_DRP;
	else
		return -EINVAL;

	if (port->fixed_role == role)
		return size;

	ret = port->cap->fix_role(port, role);
	if (ret)
		return ret;

	return size;
}

static ssize_t
current_data_role_show(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%s\n", typec_data_roles[port->fixed_role]);
}
static DEVICE_ATTR_RW(current_data_role);

static ssize_t current_power_role_store(struct device *dev,
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

	if (port->pwr_role == role || !port->partner)
		return size;

	ret = port->cap->pr_swap(port);
	if (ret)
		return ret;

	return size;
}

static ssize_t current_power_role_show(struct device *dev,
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
static DEVICE_ATTR_RW(current_power_role);

static ssize_t supported_power_roles_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	if (port->cap->usb_pd || port->cap->role == TYPEC_PORT_DRP)
		return sprintf(buf, "source, sink\n");

	return current_power_role_show(dev, attr, buf);
}
static DEVICE_ATTR_RO(supported_power_roles);

static const char * const typec_pwr_opmodes[] = {
	[TYPEC_PWR_MODE_USB] = "USB",
	[TYPEC_PWR_MODE_BC1_2] = "BC1.2",
	[TYPEC_PWR_MODE_1_5A] = "USB Type-C 1.5A",
	[TYPEC_PWR_MODE_3_0A] = "USB Type-C 3.0A",
	[TYPEC_PWR_MODE_PD] = "USB Power Delivery",
};

static ssize_t power_operation_mode_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%s\n", typec_pwr_opmodes[port->pwr_opmode]);
}
static DEVICE_ATTR_RO(power_operation_mode);

static ssize_t supports_audio_accessory_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%d\n", port->cap->audio_accessory);
}
static DEVICE_ATTR_RO(supports_audio_accessory);

static ssize_t supports_debug_accessory_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%d\n", port->cap->debug_accessory);
}
static DEVICE_ATTR_RO(supports_debug_accessory);

static ssize_t supports_usb_power_delivery_show(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%d\n", port->cap->usb_pd);
}
static DEVICE_ATTR_RO(supports_usb_power_delivery);

static struct attribute *typec_attrs[] = {
	&dev_attr_current_data_role.attr,
	&dev_attr_current_power_role.attr,
	&dev_attr_current_usb_data_role.attr,
	&dev_attr_power_operation_mode.attr,
	&dev_attr_supported_data_roles.attr,
	&dev_attr_supported_power_roles.attr,
	&dev_attr_supports_audio_accessory.attr,
	&dev_attr_supports_debug_accessory.attr,
	&dev_attr_supports_usb_power_delivery.attr,
	NULL,
};

static const struct attribute_group typec_group = {
	.attrs = typec_attrs,
};

static ssize_t number_of_alternate_modes_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%d\n", port->n_altmode);
}
static DEVICE_ATTR_RO(number_of_alternate_modes);

static struct attribute *altmode_attrs[] = {
	&dev_attr_number_of_alternate_modes.attr,
	NULL,
};

static const struct attribute_group altmode_group = {
	.name = "supported_alternate_modes",
	.attrs = altmode_attrs,
};

static const struct attribute_group *typec_groups[] = {
	&typec_group,
	&altmode_group,
	NULL,
};

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

static struct device_type typec_port_dev_type = {
	.name = "typec_port",
	.groups = typec_groups,
	.uevent = typec_uevent,
	.release = typec_release,
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
	port->dev.type = &typec_port_dev_type;
	port->dev.class = &typec_class;
	port->dev.parent = dev;
	dev_set_name(&port->dev, "usbc%d", id);
	mutex_init(&port->lock);

	port->fixed_role = port->cap->role;

	ret = device_register(&port->dev);
	if (ret) {
		ida_simple_remove(&typec_index_ida, id);
		put_device(&port->dev);
		kfree(port);
		return ERR_PTR(ret);
	}

	/*
	 * The alternate modes that the port supports must be created before
	 * registering the port. They are just linked to the port here.
	 */
	if (cap->alt_modes) {
		struct typec_altmode *alt;

		for (alt = cap->alt_modes; alt->svid; alt++) {
			ret = sysfs_add_link_to_group(&port->dev.kobj,
						"supported_alternate_modes",
						&alt->dev.kobj,
						alt->name ? alt->name :
						dev_name(&alt->dev));
			if (ret) {
				dev_WARN(&port->dev,
					 "failed to create sysfs symlink\n");
			} else {
				port->n_altmode++;
			}
		}
	}

	return port;
}
EXPORT_SYMBOL_GPL(typec_register_port);

void typec_unregister_port(struct typec_port *port)
{
	if (port->connected)
		typec_disconnect(port);

	if (port->cap->alt_modes) {
		struct typec_altmode *alt;

		for (alt = port->cap->alt_modes; alt->svid; alt++)
			sysfs_remove_link_from_group(&port->dev.kobj,
						     "alternate_modes",
						     alt->name ? alt->name :
						     dev_name(&alt->dev));
	}
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
	class_unregister(&typec_class);
}
module_exit(typec_exit);

MODULE_AUTHOR("Heikki Krogerus <heikki.krogerus@linux.intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("USB Type-C Connector Class");
