/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __USB_TYPEC_DP_H
#define __USB_TYPEC_DP_H

#include <linux/usb/typec_altmode.h>

#define USB_TYPEC_DP_SID	0xff01
#define USB_TYPEC_DP_MODE	1

/*
 * Connector states matching the pin assignments in DisplayPort Alt Mode
 * Specification.
 *
 * These values are meant primarily to be used by the mux drivers, but they are
 * also used as the "value" part in the alternate mode notification chain, so
 * receivers of those notifications will always see them.
 *
 * Note. DisplayPort USB Type-C Alt Mode Specification version 1.0b deprecated
 * pin assignments A, B and F, but they are still defined here for legacy
 * purposes.
 */
enum {
	TYPEC_DP_STATE_A = TYPEC_STATE_MODAL,	/* Not supported after v1.0b */
	TYPEC_DP_STATE_B,			/* Not supported after v1.0b */
	TYPEC_DP_STATE_C,
	TYPEC_DP_STATE_D,
	TYPEC_DP_STATE_E,
	TYPEC_DP_STATE_F,			/* Not supported after v1.0b */
};

/*
 * struct typec_displayport_data - DisplayPort Alt Mode specific data
 * @status: Status Update command VDO content
 * @conf: Configure command VDO content
 *
 * This structure is delivered as the data part with the notifications. It
 * contains the VDOs from the two DisplayPort Type-C alternate mode specific
 * commands: Status Update and Configure.
 *
 * @status will show for example the status of the HPD signal.
 */
struct typec_displayport_data {
	u32 status;
	u32 conf;
};

/* DisplayPort Status Update VDO bits */
#define DP_STATUS_CONNECTION(_status_)	((_status_) & 3)
#define   DP_STATUS_CON_DISABLED	0
#define   DP_STATUS_CON_DFP_D		1
#define   DP_STATUS_CON_UFP_D		2
#define   DP_STATUS_CON_BOTH		3
#define DP_STATUS_POWER_LOW		BIT(2)
#define DP_STATUS_ENABLED		BIT(3)
#define DP_STATUS_PREFER_MULTI_FUNC	BIT(4)
#define DP_STATUS_SWITCH_TO_USB		BIT(5)
#define DP_STATUS_EXIT_DP_MODE		BIT(6)
#define DP_STATUS_HPD_STATE		BIT(7) /* 0 = HPD_Low, 1 = HPD_High */
#define DP_STATUS_IRQ_HPD		BIT(8)

/* DisplayPort Configurations VDO bits */
#define DP_CONF_CURRENTLY(_conf_)	((_conf_) & 3)
#define DP_CONF_UFP_U_AS_DFP_D		BIT(0)
#define DP_CONF_UFP_U_AS_UFP_D		BIT(1)
#define DP_CONF_SIGNALING_DP		BIT(2)
#define DP_CONF_SIGNALING_GEN_2		BIT(3) /* Reserved after v1.0b */
#define DP_CONF_PIN_ASSIGNEMENT_SHIFT	8
#define DP_CONF_PIN_ASSIGNEMENT_MASK	GENMASK(15, 8)

#endif /* __USB_TYPEC_DP_H */
