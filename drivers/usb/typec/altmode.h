#ifndef __USB_TYPEC_ALTMODE_H__
#define __USB_TYPEC_ALTMODE_H__

#include <linux/device.h>
#include <linux/usb/typec.h>

struct typec_altmode_ops;

struct typec_mode {
	int				index;
	u32				vdo;
	char				*desc;
	enum typec_port_type		roles;

	unsigned int			active:1;

	struct typec_altmode		*alt_mode;

	char				group_name[6];
	struct attribute_group		group;
	struct attribute		*attrs[5];
	struct device_attribute		vdo_attr;
	struct device_attribute		desc_attr;
	struct device_attribute		active_attr;
	struct device_attribute		roles_attr;
};

struct typec_altmode {
	struct device			dev;
	u16				svid;
	int				n_modes;

	struct typec_mode		modes[ALTMODE_MAX_MODES];
	const struct attribute_group	*mode_groups[ALTMODE_MAX_MODES];

	struct typec_altmode			*partner;
	struct typec_altmode			*plug[2];
	const struct typec_altmode_ops		*ops;
};

#define to_altmode(d) container_of(d, struct typec_altmode, dev)

extern struct bus_type typec_altmode_bus;
extern const struct device_type typec_altmode_dev_type;

#endif /* __USB_TYPEC_ALTMODE_H__ */
