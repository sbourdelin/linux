/*
 * USB Type-C Connector Class
 *
 * Copyright (C) 2017, Intel Corporation
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

/* XXX: Once we have a header for USB Power Delivery, this belongs there */
#define ALTMODE_MAX_N_MODES	7

struct typec_mode {
	int			index;
	u32			vdo;
	char			*desc;
	enum typec_port_type	roles;

	struct typec_altmode	*alt_mode;

	unsigned int		active:1;

	char			group_name[6];
	struct attribute_group	group;
	struct attribute	*attrs[5];
	struct device_attribute vdo_attr;
	struct device_attribute desc_attr;
	struct device_attribute active_attr;
	struct device_attribute roles_attr;
};

struct typec_altmode {
	struct device			dev;
	u16				svid;
	int				n_modes;
	struct typec_mode		modes[ALTMODE_MAX_N_MODES];
	const struct attribute_group	*mode_groups[ALTMODE_MAX_N_MODES];
};

struct typec_plug {
	struct device			dev;
	enum typec_plug_index		index;
};

struct typec_cable {
	struct device			dev;
	u16				pd_revision;
	enum typec_plug_type		type;
	u32				vdo;
	unsigned int			active:1;
};

struct typec_partner {
	struct device			dev;
	u16				pd_revision;
	u32				vdo;
	enum typec_accessory		accessory;
};

struct typec_port {
	unsigned int			id;
	struct device			dev;

	int				prefer_role;
	enum typec_data_role		data_role;
	enum typec_role			pwr_role;
	enum typec_role			vconn_role;
	enum typec_pwr_opmode		pwr_opmode;

	const struct typec_capability	*cap;
};

#define to_typec_port(_dev_) container_of(_dev_, struct typec_port, dev)
#define to_typec_plug(_dev_) container_of(_dev_, struct typec_plug, dev)
#define to_typec_cable(_dev_) container_of(_dev_, struct typec_cable, dev)
#define to_typec_partner(_dev_) container_of(_dev_, struct typec_partner, dev)
#define to_altmode(_dev_) container_of(_dev_, struct typec_altmode, dev)

static const struct device_type typec_partner_dev_type;
static const struct device_type typec_cable_dev_type;
static const struct device_type typec_plug_dev_type;
static const struct device_type typec_port_dev_type;

#define is_typec_partner(_dev_) (_dev_->type == &typec_partner_dev_type)
#define is_typec_cable(_dev_) (_dev_->type == &typec_cable_dev_type)
#define is_typec_plug(_dev_) (_dev_->type == &typec_plug_dev_type)
#define is_typec_port(_dev_) (_dev_->type == &typec_port_dev_type)

static DEFINE_IDA(typec_index_ida);
static struct class *typec_class;

/* Common attributes */

static const char * const typec_accessory_modes[] = {
	[TYPEC_ACCESSORY_NONE]	= "None",
	[TYPEC_ACCESSORY_AUDIO]	= "Audio Adapter Accessory Mode",
	[TYPEC_ACCESSORY_DEBUG]	= "Debug Accessory Mode",
};

static ssize_t usb_power_delivery_revision_show(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	u16 rev = 0;

	if (is_typec_partner(dev)) {
		struct typec_partner *p = to_typec_partner(dev);

		rev = p->pd_revision;
	} else if (is_typec_cable(dev)) {
		struct typec_cable *p = to_typec_cable(dev);

		rev = p->pd_revision;
	} else if (is_typec_port(dev)) {
		struct typec_port *p = to_typec_port(dev);

		rev = p->cap->pd_revision;
	}

	return sprintf(buf, "%d\n", (rev >> 8) & 0xff);
}
static DEVICE_ATTR_RO(usb_power_delivery_revision);

static ssize_t
vdo_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u32 vdo = 0;

	if (is_typec_partner(dev)) {
		struct typec_partner *p = to_typec_partner(dev);

		vdo = p->vdo;
	} else if (is_typec_cable(dev)) {
		struct typec_cable *p = to_typec_cable(dev);

		vdo = p->vdo;
	}

	return sprintf(buf, "0x%08x\n", vdo);
}
static DEVICE_ATTR_RO(vdo);

/* ------------------------------------------------------------------------- */
/* Alternate Modes */

/**
 * typec_altmode_update_active - Report Enter/Exit mode
 * @alt: Handle to the alternate mode
 * @mode: Mode index
 * @active: True when the mode has been entered
 *
 * If a partner or cable plug executes Enter/Exit Mode command successfully, the
 * drivers use this routine to report the updated state of the mode.
 */
void typec_altmode_update_active(struct typec_altmode *alt, int mode,
				 bool active)
{
	struct typec_mode *m = &alt->modes[mode];
	char dir[6];

	if (m->active == active)
		return;

	m->active = active;
	snprintf(dir, 6, "mode%d", mode);
	sysfs_notify(&alt->dev.kobj, dir, "active");
	kobject_uevent(&alt->dev.kobj, KOBJ_CHANGE);
}
EXPORT_SYMBOL_GPL(typec_altmode_update_active);

/**
 * typec_altmode2port - Alternate Mode to USB Type-C port
 * @alt: The Alternate Mode
 *
 * Returns handle to the port that a cable plug or partner with @alt is
 * connected to.
 */
struct typec_port *typec_altmode2port(struct typec_altmode *alt)
{
	if (is_typec_plug(alt->dev.parent))
		return to_typec_port(alt->dev.parent->parent->parent);
	if (is_typec_partner(alt->dev.parent))
		return to_typec_port(alt->dev.parent->parent);
	if (is_typec_port(alt->dev.parent))
		return to_typec_port(alt->dev.parent);

	return NULL;
}
EXPORT_SYMBOL_GPL(typec_altmode2port);

static void typec_altmode_release(struct device *dev)
{
	struct typec_altmode *alt = to_altmode(dev);
	int i;

	for (i = 0; i < alt->n_modes; i++)
		kfree(alt->modes[i].desc);
	kfree(alt);
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

	return sprintf(buf, "%s\n", mode->desc ? mode->desc : "");
}

static ssize_t
typec_altmode_active_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct typec_mode *mode = container_of(attr, struct typec_mode,
					       active_attr);

	return sprintf(buf, "%d\n", mode->active);
}

static ssize_t
typec_altmode_active_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t size)
{
	struct typec_mode *mode = container_of(attr, struct typec_mode,
					       active_attr);
	struct typec_port *port = typec_altmode2port(mode->alt_mode);
	bool activate;
	int ret;

	if (!port->cap->activate_mode)
		return -EOPNOTSUPP;

	ret = kstrtobool(buf, &activate);
	if (ret)
		return ret;

	ret = port->cap->activate_mode(port->cap, mode->index, activate);
	if (ret)
		return ret;

	return size;
}

static ssize_t
typec_altmode_roles_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct typec_mode *mode = container_of(attr, struct typec_mode,
					       roles_attr);
	ssize_t ret;

	switch (mode->roles) {
	case TYPEC_PORT_DFP:
		ret =  sprintf(buf, "source\n");
		break;
	case TYPEC_PORT_UFP:
		ret = sprintf(buf, "sink\n");
		break;
	case TYPEC_PORT_DRP:
	default:
		ret = sprintf(buf, "source\nsink\n");
		break;
	}
	return ret;
}

static inline void typec_init_modes(struct typec_altmode *alt,
				    struct typec_mode_desc *desc, bool is_port)
{
	int i;

	for (i = 0; i < alt->n_modes; i++, desc++) {
		struct typec_mode *mode = &alt->modes[i];

		/* Not considering the human readable description critical */
		mode->desc = kstrdup(desc->desc, GFP_KERNEL);
		if (desc->desc && !mode->desc)
			dev_err(&alt->dev, "failed to copy mode%d desc\n", i);

		mode->alt_mode = alt;
		mode->vdo = desc->vdo;
		mode->roles = desc->roles;
		mode->index = desc->index;
		sprintf(mode->group_name, "mode%d", desc->index);

		sysfs_attr_init(&mode->vdo_attr.attr);
		mode->vdo_attr.attr.name = "vdo";
		mode->vdo_attr.attr.mode = 0444;
		mode->vdo_attr.show = typec_altmode_vdo_show;

		sysfs_attr_init(&mode->desc_attr.attr);
		mode->desc_attr.attr.name = "description";
		mode->desc_attr.attr.mode = 0444;
		mode->desc_attr.show = typec_altmode_desc_show;

		sysfs_attr_init(&mode->active_attr.attr);
		mode->active_attr.attr.name = "active";
		mode->active_attr.attr.mode = 0644;
		mode->active_attr.show = typec_altmode_active_show;
		mode->active_attr.store = typec_altmode_active_store;

		mode->attrs[0] = &mode->vdo_attr.attr;
		mode->attrs[1] = &mode->desc_attr.attr;
		mode->attrs[2] = &mode->active_attr.attr;

		/* With ports, list the roles that the mode is supported with */
		if (is_port) {
			sysfs_attr_init(&mode->roles_attr.attr);
			mode->roles_attr.attr.name = "supported_roles";
			mode->roles_attr.attr.mode = 0444;
			mode->roles_attr.show = typec_altmode_roles_show;

			mode->attrs[3] = &mode->roles_attr.attr;
		}

		mode->group.attrs = mode->attrs;
		mode->group.name = mode->group_name;

		alt->mode_groups[i] = &mode->group;
	}
}

static struct typec_altmode
*typec_register_altmode(struct device *parent, struct typec_altmode_desc *desc)
{
	struct typec_altmode *alt;
	int ret;

	alt = kzalloc(sizeof(*alt), GFP_KERNEL);
	if (!alt)
		return NULL;

	alt->svid = desc->svid;
	alt->n_modes = desc->n_modes;
	typec_init_modes(alt, desc->modes, is_typec_port(parent));

	alt->dev.parent = parent;
	alt->dev.groups = alt->mode_groups;
	alt->dev.release = typec_altmode_release;
	dev_set_name(&alt->dev, "%s.svid:%04x", dev_name(parent), alt->svid);

	ret = device_register(&alt->dev);
	if (ret) {
		int i;

		dev_err(parent, "failed to register alternate mode (%d)\n",
			ret);

		put_device(&alt->dev);

		for (i = 0; i < alt->n_modes; i++)
			kfree(alt->modes[i].desc);
		kfree(alt);
		return NULL;
	}

	return alt;
}

/**
 * typec_unregister_altmode - Unregister Alternate Mode
 * @alt: The alternate mode to be unregistered
 *
 * Unregister device created with typec_partner_register_altmode(),
 * typec_plug_register_altmode() or typec_port_register_altmode().
 */
void typec_unregister_altmode(struct typec_altmode *alt)
{
	if (alt)
		device_unregister(&alt->dev);
}
EXPORT_SYMBOL_GPL(typec_unregister_altmode);

/* ------------------------------------------------------------------------- */
/* Type-C Partners */

static ssize_t accessory_mode_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct typec_partner *p = to_typec_partner(dev);

	if (p->accessory == TYPEC_ACCESSORY_NONE)
		return 0;

	return sprintf(buf, "%s\n", typec_accessory_modes[p->accessory]);
}
static DEVICE_ATTR_RO(accessory_mode);

static struct attribute *typec_partner_attrs[] = {
	&dev_attr_vdo.attr,
	&dev_attr_accessory_mode.attr,
	&dev_attr_usb_power_delivery_revision.attr,
	NULL
};
ATTRIBUTE_GROUPS(typec_partner);

static void typec_partner_release(struct device *dev)
{
	struct typec_partner *partner = to_typec_partner(dev);

	kfree(partner);
}

static const struct device_type typec_partner_dev_type = {
	.name = "typec_partner_device",
	.groups = typec_partner_groups,
	.release = typec_partner_release,
};

/**
 * typec_partner_register_altmode - Register USB Type-C Partner Alternate Mode
 * @partner: USB Type-C Partner that supports the alternate mode
 * @desc: Description of the alternate mode
 *
 * This routine is used to register each alternate mode individually that
 * @partner has listed in response to Discover SVIDs command. The modes for a
 * SVID listed in response to Discover Modes command need to be listed in an
 * array in @desc.
 *
 * Returns handle to the alternate mode on success or NULL on failure.
 */
struct typec_altmode
*typec_partner_register_altmode(struct typec_partner *partner,
				struct typec_altmode_desc *desc)
{
	return typec_register_altmode(&partner->dev, desc);
}
EXPORT_SYMBOL_GPL(typec_partner_register_altmode);

/**
 * typec_register_partner - Register a USB Type-C Partner
 * @port: The USB Type-C Port the partner is connected to
 * @desc: Description of the partner
 *
 * Registers a device for USB Type-C Partner described in @desc.
 *
 * Returns handle to the partner on success or NULL on failure.
 */
struct typec_partner *typec_register_partner(struct typec_port *port,
					     struct typec_partner_desc *desc)
{
	struct typec_partner *partner = NULL;
	int ret;

	partner = kzalloc(sizeof(*partner), GFP_KERNEL);
	if (!partner)
		return NULL;

	partner->vdo = desc->vdo;
	partner->accessory = desc->accessory;
	partner->pd_revision = desc->pd_revision;

	partner->dev.class = typec_class;
	partner->dev.parent = &port->dev;
	partner->dev.type = &typec_partner_dev_type;
	dev_set_name(&partner->dev, "%s-partner", dev_name(&port->dev));

	ret = device_register(&partner->dev);
	if (ret) {
		dev_err(&port->dev, "failed to register partner (%d)\n", ret);
		put_device(&partner->dev);
		kfree(partner);
		return NULL;
	}

	return partner;
}
EXPORT_SYMBOL_GPL(typec_register_partner);

/**
 * typec_unregister_partner - Unregister a USB Type-C Partner
 * @partner: The partner to be unregistered
 *
 * Unregister device created with typec_register_partner().
 */
void typec_unregister_partner(struct typec_partner *partner)
{
	if (partner)
		device_unregister(&partner->dev);
}
EXPORT_SYMBOL_GPL(typec_unregister_partner);

/* ------------------------------------------------------------------------- */
/* Type-C Cable Plugs */

static void typec_plug_release(struct device *dev)
{
	struct typec_plug *plug = to_typec_plug(dev);

	kfree(plug);
}

static const struct device_type typec_plug_dev_type = {
	.name = "typec_plug_device",
	.release = typec_plug_release,
};

/**
 * typec_plug_register_altmode - Register USB Type-C Cable Plug Alternate Mode
 * @plug: USB Type-C Cable Plug that supports the alternate mode
 * @desc: Description of the alternate mode
 *
 * This routine is used to register each alternate mode individually that @plug
 * has listed in response to Discover SVIDs command. The modes for a SVID that
 * the plug lists in response to Discover Modes command need to be listed in an
 * array in @desc.
 *
 * Returns handle to the alternate mode on success or NULL on failure.
 */
struct typec_altmode
*typec_plug_register_altmode(struct typec_plug *plug,
			     struct typec_altmode_desc *desc)
{
	return typec_register_altmode(&plug->dev, desc);
}
EXPORT_SYMBOL_GPL(typec_plug_register_altmode);

/**
 * typec_register_plug - Register a USB Type-C Cable Plug
 * @cable: USB Type-C Cable with the plug
 * @desc: Description of the cable plug
 *
 * Registers a device for USB Type-C Cable Plug described in @desc. A USB Type-C
 * Cable Plug represents a plug with electronics in it that can response to USB
 * Power Delivery SOP Prime or SOP Double Prime packages.
 *
 * Returns handle to the cable plug on success or NULL on failure.
 */
struct typec_plug *typec_register_plug(struct typec_cable *cable,
				       struct typec_plug_desc *desc)
{
	struct typec_plug *plug = NULL;
	char name[8];
	int ret;

	plug = kzalloc(sizeof(*plug), GFP_KERNEL);
	if (!plug)
		return NULL;

	sprintf(name, "plug%d", desc->index);

	plug->index = desc->index;
	plug->dev.class = typec_class;
	plug->dev.parent = &cable->dev;
	plug->dev.type = &typec_plug_dev_type;
	dev_set_name(&plug->dev, "%s-%s", dev_name(cable->dev.parent), name);

	ret = device_register(&plug->dev);
	if (ret) {
		dev_err(&cable->dev, "failed to register plug (%d)\n", ret);
		put_device(&plug->dev);
		kfree(plug);
		return NULL;
	}

	return plug;
}
EXPORT_SYMBOL_GPL(typec_register_plug);

/**
 * typec_unregister_plug - Unregister a USB Type-C Cable Plug
 * @plug: The cable plug to be unregistered
 *
 * Unregister device created with typec_register_plug().
 */
void typec_unregister_plug(struct typec_plug *plug)
{
	if (plug)
		device_unregister(&plug->dev);
}
EXPORT_SYMBOL_GPL(typec_unregister_plug);

/* Type-C Cables */

static ssize_t
active_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct typec_cable *cable = to_typec_cable(dev);

	return sprintf(buf, "%d\n", cable->active);
}
static DEVICE_ATTR_RO(active);

static const char * const typec_plug_types[] = {
	[USB_PLUG_NONE]		= "Unknown",
	[USB_PLUG_TYPE_A]	= "Type-A",
	[USB_PLUG_TYPE_B]	= "Type-B",
	[USB_PLUG_TYPE_C]	= "Type-C",
	[USB_PLUG_CAPTIVE]	= "Captive",
};

static ssize_t plug_type_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct typec_cable *cable = to_typec_cable(dev);

	return sprintf(buf, "%s\n", typec_plug_types[cable->type]);
}
static DEVICE_ATTR_RO(plug_type);

static struct attribute *typec_cable_attrs[] = {
	&dev_attr_active.attr,
	&dev_attr_plug_type.attr,
	&dev_attr_usb_power_delivery_revision.attr,
	NULL
};
ATTRIBUTE_GROUPS(typec_cable);

static void typec_cable_release(struct device *dev)
{
	struct typec_cable *cable = to_typec_cable(dev);

	kfree(cable);
}

static const struct device_type typec_cable_dev_type = {
	.name = "typec_cable_device",
	.groups = typec_cable_groups,
	.release = typec_cable_release,
};

/**
 * typec_register_cable - Register a USB Type-C Cable
 * @port: The USB Type-C Port the cable is connected to
 * @desc: Description of the cable
 *
 * Registers a device for USB Type-C Cable described in @desc. The cable will be
 * parent for the optional cable plug devises.
 *
 * Returns handle to the cable on success or NULL on failure.
 */
struct typec_cable *typec_register_cable(struct typec_port *port,
					 struct typec_cable_desc *desc)
{
	struct typec_cable *cable = NULL;
	int ret;

	cable = kzalloc(sizeof(*cable), GFP_KERNEL);
	if (!cable)
		return NULL;

	cable->type = desc->type;
	cable->vdo = desc->vdo;
	cable->active = desc->active;
	cable->pd_revision = desc->pd_revision;

	cable->dev.class = typec_class;
	cable->dev.parent = &port->dev;
	cable->dev.type = &typec_cable_dev_type;
	dev_set_name(&cable->dev, "%s-cable", dev_name(&port->dev));

	ret = device_register(&cable->dev);
	if (ret) {
		dev_err(&port->dev, "failed to register cable (%d)\n", ret);
		put_device(&cable->dev);
		kfree(cable);
		return NULL;
	}

	return cable;
}
EXPORT_SYMBOL_GPL(typec_register_cable);

/**
 * typec_unregister_cable - Unregister a USB Type-C Cable
 * @cable: The cable to be unregistered
 *
 * Unregister device created with typec_register_cable().
 */
void typec_unregister_cable(struct typec_cable *cable)
{
	if (cable)
		device_unregister(&cable->dev);
}
EXPORT_SYMBOL_GPL(typec_unregister_cable);

/* ------------------------------------------------------------------------- */
/* USB Type-C ports */

/* --------------------------------------- */
/* Driver callbacks to report role updates */

/**
 * typec_set_data_role - Report data role change
 * @port: The USB Type-C Port where the role was changed
 * @role: The new data role
 *
 * This routine is used by the port drivers to report data role changes.
 */
void typec_set_data_role(struct typec_port *port, enum typec_data_role role)
{
	if (port->data_role == role)
		return;

	port->data_role = role;
	sysfs_notify(&port->dev.kobj, NULL, "current_data_role");
	kobject_uevent(&port->dev.kobj, KOBJ_CHANGE);
}
EXPORT_SYMBOL_GPL(typec_set_data_role);

/**
 * typec_set_pwr_role - Report power role change
 * @port: The USB Type-C Port where the role was changed
 * @role: The new data role
 *
 * This routine is used by the port drivers to report power role changes.
 */
void typec_set_pwr_role(struct typec_port *port, enum typec_role role)
{
	if (port->pwr_role == role)
		return;

	port->pwr_role = role;
	sysfs_notify(&port->dev.kobj, NULL, "current_power_role");
	kobject_uevent(&port->dev.kobj, KOBJ_CHANGE);
}
EXPORT_SYMBOL_GPL(typec_set_pwr_role);

/**
 * typec_set_pwr_role - Report VCONN source change
 * @port: The USB Type-C Port which VCONN role changed
 * @role: Source when @port is sourcing VCONN, or Sink when it's not
 *
 * This routine is used by the port drivers to report if the VCONN source is
 * changes.
 */
void typec_set_vconn_role(struct typec_port *port, enum typec_role role)
{
	if (port->vconn_role == role)
		return;

	port->vconn_role = role;
	sysfs_notify(&port->dev.kobj, NULL, "vconn_source");
	kobject_uevent(&port->dev.kobj, KOBJ_CHANGE);
}
EXPORT_SYMBOL_GPL(typec_set_vconn_role);

/**
 * typec_set_pwr_opmode - Report changed power operation mode
 * @port: The USB Type-C Port where the mode was changed
 * @opmode: New power operation mode
 *
 * This routine is used by the port drivers to report changed power operation
 * mode in @port. The modes are USB (default), 1.5A, 3.0A as defined in USB
 * Type-C specification, and "USB Power Delivery" when the power levels are
 * negotiated with methods defined in USB Power Delivery specification.
 */
void typec_set_pwr_opmode(struct typec_port *port,
			  enum typec_pwr_opmode opmode)
{
	if (port->pwr_opmode == opmode)
		return;

	port->pwr_opmode = opmode;
	sysfs_notify(&port->dev.kobj, NULL, "power_operation_mode");
	kobject_uevent(&port->dev.kobj, KOBJ_CHANGE);
}
EXPORT_SYMBOL_GPL(typec_set_pwr_opmode);

/* --------------------------------------- */

static const char * const typec_roles[] = {
	[TYPEC_SINK]	= "sink",
	[TYPEC_SOURCE]	= "source",
};

static const char * const typec_data_roles[] = {
	[TYPEC_DEVICE]	= "device",
	[TYPEC_HOST]	= "host",
};

static ssize_t
preferred_role_store(struct device *dev, struct device_attribute *attr,
		     const char *buf, size_t size)
{
	struct typec_port *port = to_typec_port(dev);
	int role;
	int ret;

	if (port->cap->type != TYPEC_PORT_DRP) {
		dev_dbg(dev, "Preferred role only supported with DRP ports\n");
		return -EOPNOTSUPP;
	}

	if (!port->cap->try_role) {
		dev_dbg(dev, "Setting preferred role not supported\n");
		return -EOPNOTSUPP;
	}

	role = sysfs_match_string(typec_roles, buf);
	if (role < 0) {
		if (sysfs_streq(buf, "none"))
			role = TYPEC_NO_PREFERRED_ROLE;
		else
			return -EINVAL;
	}

	ret = port->cap->try_role(port->cap, role);
	if (ret)
		return ret;

	port->prefer_role = role;
	return size;
}

static ssize_t
preferred_role_show(struct device *dev, struct device_attribute *attr,
		    char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	if (port->cap->type != TYPEC_PORT_DRP)
		return 0;

	if (port->prefer_role < 0)
		return 0;

	return sprintf(buf, "%s\n", typec_roles[port->prefer_role]);
}
static DEVICE_ATTR_RW(preferred_role);

static ssize_t
current_data_role_store(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t size)
{
	struct typec_port *port = to_typec_port(dev);
	int ret;

	if (port->cap->type != TYPEC_PORT_DRP) {
		dev_dbg(dev, "data role swap only supported with DRP ports\n");
		return -EOPNOTSUPP;
	}

	if (!port->cap->dr_set) {
		dev_dbg(dev, "data role swapping not supported\n");
		return -EOPNOTSUPP;
	}

	ret = sysfs_match_string(typec_data_roles, buf);
	if (ret < 0)
		return ret;

	ret = port->cap->dr_set(port->cap, ret);
	if (ret)
		return ret;

	return size;
}

static ssize_t
current_data_role_show(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%s\n", typec_data_roles[port->data_role]);
}
static DEVICE_ATTR_RW(current_data_role);

static ssize_t supported_data_roles_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	if (port->cap->type == TYPEC_PORT_DRP)
		return sprintf(buf, "host\ndevice\n");

	return sprintf(buf, "%s\n", typec_data_roles[port->data_role]);
}
static DEVICE_ATTR_RO(supported_data_roles);

static ssize_t current_power_role_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct typec_port *port = to_typec_port(dev);
	int ret = size;

	if (!port->cap->pd_revision) {
		dev_dbg(dev, "power role swap only supported with USB PD\n");
		return -EOPNOTSUPP;
	}

	if (!port->cap->pr_set) {
		dev_dbg(dev, "power role swapping not supported\n");
		return -EOPNOTSUPP;
	}

	if (port->pwr_opmode != TYPEC_PWR_MODE_PD) {
		dev_dbg(dev, "partner unable to swap power role\n");
		return -EIO;
	}

	ret = sysfs_match_string(typec_roles, buf);
	if (ret < 0)
		return ret;

	ret = port->cap->pr_set(port->cap, ret);
	if (ret)
		return ret;

	return size;
}

static ssize_t current_power_role_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%s\n", typec_roles[port->pwr_role]);
}
static DEVICE_ATTR_RW(current_power_role);

static ssize_t supported_power_roles_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	if (port->cap->pd_revision || port->cap->type == TYPEC_PORT_DRP)
		return sprintf(buf, "source\nsink\n");

	return sprintf(buf, "%s\n", typec_roles[port->pwr_role]);
}
static DEVICE_ATTR_RO(supported_power_roles);

static const char * const typec_pwr_opmodes[] = {
	[TYPEC_PWR_MODE_USB]	= "USB",
	[TYPEC_PWR_MODE_1_5A]	= "USB Type-C 1.5A",
	[TYPEC_PWR_MODE_3_0A]	= "USB Type-C 3.0A",
	[TYPEC_PWR_MODE_PD]	= "USB Power Delivery",
};

static ssize_t power_operation_mode_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%s\n", typec_pwr_opmodes[port->pwr_opmode]);
}
static DEVICE_ATTR_RO(power_operation_mode);

static ssize_t vconn_source_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t size)
{
	struct typec_port *port = to_typec_port(dev);
	enum typec_role role;
	int ret;

	if (!port->cap->pd_revision) {
		dev_dbg(dev, "vconn swap only supported with USB PD\n");
		return -EOPNOTSUPP;
	}

	if (!port->cap->vconn_set) {
		dev_dbg(dev, "vconn swapping not supported\n");
		return -EOPNOTSUPP;
	}

	if (sysfs_streq(buf, "1"))
		role = TYPEC_SOURCE;
	else if (sysfs_streq(buf, "0"))
		role = TYPEC_SINK;
	else
		return -EINVAL;

	ret = port->cap->vconn_set(port->cap, role);
	if (ret)
		return ret;

	return size;
}

static ssize_t vconn_source_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct typec_port *port = to_typec_port(dev);

	return sprintf(buf, "%d\n", port->vconn_role == TYPEC_SOURCE ? 1 : 0);
}
static DEVICE_ATTR_RW(vconn_source);

static ssize_t supported_accessory_modes_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct typec_port *port = to_typec_port(dev);
	ssize_t ret = 0;
	int i;

	if (!port->cap->accessory)
		return 0;

	for (i = 0; port->cap->accessory[i]; i++)
		ret += sprintf(buf + ret, "%s\n",
			       typec_accessory_modes[port->cap->accessory[i]]);
	return ret;
}
static DEVICE_ATTR_RO(supported_accessory_modes);

static ssize_t usb_typec_revision_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct typec_port *port = to_typec_port(dev);
	u16 rev = port->cap->revision;

	return sprintf(buf, "%d.%d\n", (rev >> 8) & 0xff, rev >> 4 & 0xf);
}
static DEVICE_ATTR_RO(usb_typec_revision);

static struct attribute *typec_attrs[] = {
	&dev_attr_current_power_role.attr,
	&dev_attr_current_data_role.attr,
	&dev_attr_power_operation_mode.attr,
	&dev_attr_preferred_role.attr,
	&dev_attr_supported_accessory_modes.attr,
	&dev_attr_supported_data_roles.attr,
	&dev_attr_supported_power_roles.attr,
	&dev_attr_usb_power_delivery_revision.attr,
	&dev_attr_usb_typec_revision.attr,
	&dev_attr_vconn_source.attr,
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

static const struct device_type typec_port_dev_type = {
	.name = "typec_port",
	.groups = typec_groups,
	.uevent = typec_uevent,
	.release = typec_release,
};

/**
 * typec_port_register_altmode - Register USB Type-C Port Alternate Mode
 * @port: USB Type-C Port that supports the alternate mode
 * @desc: Description of the alternate mode
 *
 * This routine is used to register an alternate mode that @port is capable of
 * supporting.
 *
 * Returns handle to the alternate mode on success or NULL on failure.
 */
struct typec_altmode
*typec_port_register_altmode(struct typec_port *port,
			     struct typec_altmode_desc *desc)
{
	return typec_register_altmode(&port->dev, desc);
}
EXPORT_SYMBOL_GPL(typec_port_register_altmode);

/**
 * typec_register_port - Register a USB Type-C Port
 * @parent: Parent device
 * @cap: Description of the port
 *
 * Registers a device for USB Type-C Port described in @cap.
 *
 * Returns handle to the port on success or NULL on failure.
 */
struct typec_port *typec_register_port(struct device *parent,
				       const struct typec_capability *cap)
{
	struct typec_port *port;
	enum typec_role role;
	int ret;
	int id;

	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return NULL;

	id = ida_simple_get(&typec_index_ida, 0, 0, GFP_KERNEL);
	if (id < 0) {
		kfree(port);
		return NULL;
	}

	if (cap->type == TYPEC_PORT_DFP)
		role = TYPEC_SOURCE;
	else if (cap->type == TYPEC_PORT_UFP)
		role = TYPEC_SINK;
	else
		role = cap->prefer_role;

	if (role == TYPEC_SOURCE) {
		port->data_role = TYPEC_HOST;
		port->pwr_role = TYPEC_SOURCE;
		port->vconn_role = TYPEC_SOURCE;
	} else {
		port->data_role = TYPEC_DEVICE;
		port->pwr_role = TYPEC_SINK;
		port->vconn_role = TYPEC_SINK;
	}

	port->id = id;
	port->cap = cap;
	port->prefer_role = cap->prefer_role;

	port->dev.type = &typec_port_dev_type;
	port->dev.class = typec_class;
	port->dev.parent = parent;
	dev_set_name(&port->dev, "port%d", id);

	ret = device_register(&port->dev);
	if (ret) {
		dev_err(parent, "failed to register port (%d)\n", ret);
		ida_simple_remove(&typec_index_ida, id);
		put_device(&port->dev);
		kfree(port);
		return NULL;
	}

	return port;
}
EXPORT_SYMBOL_GPL(typec_register_port);

/**
 * typec_unregister_port - Unregister a USB Type-C Port
 * @port: The port to be unregistered
 *
 * Unregister device created with typec_register_port().
 */
void typec_unregister_port(struct typec_port *port)
{
	if (port)
		device_unregister(&port->dev);
}
EXPORT_SYMBOL_GPL(typec_unregister_port);

static int __init typec_init(void)
{
	typec_class = class_create(THIS_MODULE, "typec");
	if (IS_ERR(typec_class))
		return PTR_ERR(typec_class);
	return 0;
}
subsys_initcall(typec_init);

static void __exit typec_exit(void)
{
	class_destroy(typec_class);
	ida_destroy(&typec_index_ida);
}
module_exit(typec_exit);

MODULE_AUTHOR("Heikki Krogerus <heikki.krogerus@linux.intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("USB Type-C Connector Class");
