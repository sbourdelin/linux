
#ifndef __LINUX_USB_TYPEC_H
#define __LINUX_USB_TYPEC_H

#include <linux/types.h>

/* USB Type-C Specification releases */
#define USB_TYPEC_REV_1_0	0x100 /* 1.0 */
#define USB_TYPEC_REV_1_1	0x110 /* 1.1 */
#define USB_TYPEC_REV_1_2	0x120 /* 1.2 */

struct typec_altmode;
struct typec_partner;
struct typec_cable;
struct typec_plug;
struct typec_port;

enum typec_port_type {
	TYPEC_PORT_DFP,
	TYPEC_PORT_UFP,
	TYPEC_PORT_DRP,
};

enum typec_plug_type {
	USB_PLUG_NONE,
	USB_PLUG_TYPE_A,
	USB_PLUG_TYPE_B,
	USB_PLUG_TYPE_C,
	USB_PLUG_CAPTIVE,
};

enum typec_data_role {
	TYPEC_DEVICE,
	TYPEC_HOST,
};

enum typec_role {
	TYPEC_SINK,
	TYPEC_SOURCE,
};

enum typec_pwr_opmode {
	TYPEC_PWR_MODE_USB,
	TYPEC_PWR_MODE_1_5A,
	TYPEC_PWR_MODE_3_0A,
	TYPEC_PWR_MODE_PD,
};

enum typec_accessory {
	TYPEC_ACCESSORY_NONE,
	TYPEC_ACCESSORY_AUDIO,
	TYPEC_ACCESSORY_DEBUG,
};

/*
 * struct typec_mode_desc - Individual Mode of an Alternate Mode
 * @index: Index of the Mode within the SVID
 * @vdo: VDO returned by Discover Modes USB PD command
 * @desc: Optional human readable description of the mode
 * @roles: Only for ports. DRP if the mode is available in both roles
 *
 * Description of a mode of an Alternate Mode which a connector, cable plug or
 * partner supports. Every mode will have it's own sysfs group. The details are
 * the VDO returned by discover modes command, description for the mode and
 * active flag telling has the mode being entered or not.
 */
struct typec_mode_desc {
	int			index;
	u32			vdo;
	char			*desc;
	/* Only used with ports */
	enum typec_port_type	roles;
};

/*
 * struct typec_altmode_desc - USB Type-C Alternate Mode Descriptor
 * @svid: Standard or Vendor ID
 * @n_modes: Number of modes
 * @modes: Array of modes supported by the Alternate Mode
 *
 * Representation of an Alternate Mode that has SVID assigned by USB-IF. The
 * array of modes will list the modes of a particular SVID that are supported by
 * a connector, partner of a cable plug.
 */
struct typec_altmode_desc {
	u16			svid;
	int			n_modes;
	struct typec_mode_desc	*modes;
};

struct typec_altmode
*typec_partner_register_altmode(struct typec_partner *partner,
				struct typec_altmode_desc *desc);
struct typec_altmode
*typec_plug_register_altmode(struct typec_plug *plug,
			     struct typec_altmode_desc *desc);
struct typec_altmode
*typec_port_register_altmode(struct typec_port *port,
			     struct typec_altmode_desc *desc);
void typec_unregister_altmode(struct typec_altmode *altmode);

struct typec_port *typec_altmode2port(struct typec_altmode *alt);

void typec_altmode_update_active(struct typec_altmode *alt, int mode,
				 bool active);

enum typec_plug_index {
	TYPEC_PLUG_SOP_P,
	TYPEC_PLUG_SOP_PP,
};

/*
 * struct typec_plug_desc - USB Type-C Cable Plug Descriptor
 * @index: SOP Prime for the plug connected to DFP and SOP Double Prime for the
 *         plug connected to UFP
 *
 * Represents USB Type-C Cable Plug.
 */
struct typec_plug_desc {
	enum typec_plug_index	index;
};

/*
 * struct typec_cable_desc - USB Type-C Cable Descriptor
 * @pd_revision: USB Power Delivery Specification revision
 * @type: The plug type from USB PD Cable VDO
 * @active: Is the cable active or passive
 *
 * Represents USB Type-C Cable attached to USB Type-C port.
 */
struct typec_cable_desc {
	u16			pd_revision; /* 0300H = "3.0" */
	enum typec_plug_type	type;
	u32			vdo;
	unsigned int		active:1;
};

/*
 * struct typec_partner_desc - USB Type-C Partner Descriptor
 * @pd_revision: USB Power Delivery Specification revision (0 = no USB PD)
 * @vdo: VDO returned by Discover Identity USB PD command
 * @accessory: Audio, Debug or none.
 *
 * Details about a partner that is attached to USB Type-C port.
 */
struct typec_partner_desc {
	u16			pd_revision; /* 0300H = "3.0" */
	u32			vdo;
	enum typec_accessory	accessory;
};

/*
 * struct typec_capability - USB Type-C Port Capabilities
 * @role: DFP (Host-only), UFP (Device-only) or DRP (Dual Role)
 * @revision: USB Type-C Specification release. Binary coded decimal
 * @pd_revision: USB Power Delivery Specification revision if supported
 * @prefer_role: Initial role preference
 * @accessory: Supported Accessory Modes (NULL terminated array)
 * @try_role: Set data role preference for DRP port
 * @dr_set: Set Data Role
 * @pr_set: Set Power Role
 * @vconn_set: Set VCONN Role
 * @activate_mode: Enter/exit given Alternate Mode
 *
 * Static capabilities of a single USB Type-C port.
 */
struct typec_capability {
	enum typec_port_type	type;
	u16			revision; /* 0120H = "1.2" */
	u16			pd_revision; /* 0300H = "3.0" */
	int			prefer_role;
	enum typec_accessory	*accessory;

	int		(*try_role)(const struct typec_capability *,
				    int role);

	int		(*dr_set)(const struct typec_capability *,
				  enum typec_data_role);
	int		(*pr_set)(const struct typec_capability *,
				  enum typec_role);
	int		(*vconn_set)(const struct typec_capability *,
				     enum typec_role);

	int		(*activate_mode)(const struct typec_capability *,
					 int mode, int activate);
};

/* Specific to try_role(). Indicates the user want's to clear the preference. */
#define TYPEC_NO_PREFERRED_ROLE	(-1)

struct typec_port *typec_register_port(struct device *parent,
				       const struct typec_capability *cap);
void typec_unregister_port(struct typec_port *port);

struct typec_partner *typec_register_partner(struct typec_port *port,
					     struct typec_partner_desc *desc);
void typec_unregister_partner(struct typec_partner *partner);

struct typec_cable *typec_register_cable(struct typec_port *port,
					 struct typec_cable_desc *desc);
void typec_unregister_cable(struct typec_cable *cable);

struct typec_plug *typec_register_plug(struct typec_cable *cable,
				       struct typec_plug_desc *desc);
void typec_unregister_plug(struct typec_plug *plug);

void typec_set_data_role(struct typec_port *port, enum typec_data_role role);
void typec_set_pwr_role(struct typec_port *port, enum typec_role role);
void typec_set_vconn_role(struct typec_port *port, enum typec_role role);
void typec_set_pwr_opmode(struct typec_port *port, enum typec_pwr_opmode mode);

#endif /* __LINUX_USB_TYPEC_H */
