
#ifndef __LINUX_USB_TYPEC_H
#define __LINUX_USB_TYPEC_H

#include <linux/types.h>

struct typec_port;

enum typec_data_role {
	TYPEC_PORT_DFP,
	TYPEC_PORT_UFP,
	TYPEC_PORT_DRP,
};

enum typec_partner_type {
	TYPEC_PARTNER_USB,
	TYPEC_PARTNER_CHARGER,
	TYPEC_PARTNER_ALTMODE,
	TYPEC_PARTNER_AUDIO,
	TYPEC_PARTNER_DEBUG,
};

enum typec_plug_type {
	USB_PLUG_NONE,
	USB_PLUG_TYPE_A,
	USB_PLUG_TYPE_B,
	USB_PLUG_TYPE_C,
	USB_PLUG_CAPTIVE,
};

enum typec_usb_role {
	TYPEC_DEVICE,
	TYPEC_HOST,
};

enum typec_pwr_role {
	TYPEC_PWR_SINK,
	TYPEC_PWR_SOURCE,
};

enum typec_pwr_opmode {
	TYPEC_PWR_MODE_USB,
	TYPEC_PWR_MODE_BC1_2,
	TYPEC_PWR_MODE_1_5A,
	TYPEC_PWR_MODE_3_0A,
	TYPEC_PWR_MODE_PD,
};

/*
 * struct typec_mode - Individual Mode of an Alternate Mode
 * @vdo: VDO returned by Discover Modes USB PD command
 * @desc: Mode description
 * @active: Tells if the mode is currently entered or not
 * @index: Index of the mode
 * @group_name: Name for the sysfs folder in form "mode<index>"
 * @group: The sysfs group (folder) for the mode
 * @attrs: The attributes for the sysfs group
 * @vdo_attr: Device attribute to expose the VDO of the mode
 * @desc_attr: Device attribute to expose the description of the mode
 * @acctive_attr: Device attribute to expose active of the mode
 *
 * Details about a mode of an Alternate Mode which a connector, cable plug or
 * partner supports. Every mode will have it's own sysfs group. The details are
 * the VDO returned by discover modes command, description for the mode and
 * active flag telling is the mode currently active or not.
 */
struct typec_mode {
	u32			vdo;
	char			*desc;
	unsigned int		active:1;

	int			index;
	char			group_name[8];
	struct attribute_group	group;
	struct attribute	*attrs[4];
	struct device_attribute vdo_attr;
	struct device_attribute desc_attr;
	struct device_attribute active_attr;
};

/*
 * struct typec_altmode - USB Type-C Alternate Mode
 * @dev: struct device instance
 * @name: Name for the Alternate Mode (optional)
 * @svid: Standard or Vendor ID
 * @n_modes: Number of modes
 * @modes: Array of modes supported by the Alternat Mode
 * @mode_groups: The modes as attribute groups to be exposed in sysfs
 *
 * Representation of an Alternate Mode that has SVID assigned by USB-IF. The
 * array of modes will list the modes of a particular SVID that are supported by
 * a connector, partner of a cable plug.
 */
struct typec_altmode {
	struct device		dev;
	char			*name;

	u16			svid;
	int			n_modes;
	struct typec_mode	*modes;

	const struct attribute_group **mode_groups;
};

#define to_altmode(d) container_of(d, struct typec_altmode, dev)

struct typec_port *typec_altmode2port(struct typec_altmode *);

int typec_register_altmodes(struct device *, struct typec_altmode *);
void typec_unregister_altmodes(struct typec_altmode *);

/*
 * struct typec_plug - USB Type-C Cable Plug
 * @dev: struct device instance
 * @index: 1 for the plug connected to DFP and 2 for the plug connected to UFP
 * @alt_modes: Alternate Modes the cable plug supports (null terminated)
 *
 * Represents USB Type-C Cable Plug.
 */
struct typec_plug {
	struct device		dev;
	int			index;
	struct typec_altmode	*alt_modes;
};

/*
 * struct typec_cable - USB Type-C Cable
 * @dev: struct device instance
 * @type: The plug type from USB PD Cable VDO
 * @active: Is the cable active or passive
 * @sop_pp_controller: Tells whether both cable plugs are configurable or not
 * @plug: The two plugs in the cable.
 *
 * Represents USB Type-C Cable attached to USB Type-C port. Two plugs are
 * created if the cable has SOP Double Prime controller as defined in USB PD
 * specification. Otherwise only one will be created if the cable is active. For
 * passive cables no plugs are created.
 */
struct typec_cable {
	struct device		dev;
	enum typec_plug_type	type;
	unsigned int		active:1;
	unsigned int		sop_pp_controller:1;
	/* REVISIT: What else needed? */

	struct typec_plug	plug[2];
};

/*
 * struct typec_partner - USB Type-C Partner
 * @dev: struct device instance
 * @type: Normal USB device, charger, Alternate Mode or Accessory
 * @alt_modes: Alternate Modes the partner supports (null terminated)
 *
 * Details about a partner that is attached to USB Type-C port.
 */
struct typec_partner {
	struct device		dev;
	enum typec_partner_type	type;
	struct typec_altmode	*alt_modes;
};

/*
 * struct typec_capability - USB Type-C Port Capabilities
 * @role: DFP (Host-only), UFP (Device-only) or DRP (Dual Role)
 * @usb_pd: USB Power Delivery support
 * @alt_modes: Alternate Modes the connector supports (null terminated)
 * @audio_accessory: Audio Accessory Adapter Mode support
 * @debug_accessory: Debug Accessory Mode support
 * @fix_role: Set a fixed data role for DRP port
 * @dr_swap: Data Role Swap support
 * @pr_swap: Power Role Swap support
 * @vconn_swap: VCONN Swap support
 * @activate_mode: Enter/exit given Alternate Mode
 *
 * Static capabilities of a single USB Type-C port.
 */
struct typec_capability {
	enum typec_data_role	role;
	unsigned int		usb_pd:1;
	struct typec_altmode	*alt_modes;
	unsigned int		audio_accessory:1;
	unsigned int		debug_accessory:1;

	int			(*fix_role)(struct typec_port *,
					    enum typec_data_role);

	int			(*dr_swap)(struct typec_port *);
	int			(*pr_swap)(struct typec_port *);
	int			(*vconn_swap)(struct typec_port *);

	int			(*activate_mode)(struct typec_altmode *,
						 int mode, int activate);
};

/*
 * struct typec_connection - Details about USB Type-C port connection event
 * @partner: The attached partner
 * @cable: The attached cable
 * @usb_role: Initial USB data role (host or device)
 * @pwr_role: Initial Power role (source or sink)
 * @vconn_role: Initial VCONN role (source or sink)
 * @pwr_opmode: The power mode of the connection
 *
 * All the relevant details about a connection event. Wrapper that is passed to
 * typec_connect(). The context is copied when typec_connect() is called and the
 * structure is not used for anything else.
 */
struct typec_connection {
	struct typec_partner	*partner;
	struct typec_cable	*cable;

	enum typec_usb_role	usb_role;
	enum typec_pwr_role	pwr_role;
	enum typec_pwr_role	vconn_role;
	enum typec_pwr_opmode	pwr_opmode;
};

struct typec_port *typec_register_port(struct device *dev,
				       struct typec_capability *cap);
void typec_unregister_port(struct typec_port *port);

int typec_connect(struct typec_port *port, struct typec_connection *con);
void typec_disconnect(struct typec_port *port);

/* REVISIT: are these needed? */
struct device *typec_port2dev(struct typec_port *port);
struct typec_port *typec_dev2port(struct device *dev);

#endif /* __LINUX_USB_TYPEC_H */
