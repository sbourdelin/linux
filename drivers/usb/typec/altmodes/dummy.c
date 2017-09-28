/**
 * USB Typec-C DisplayPort Alternate Mode driver
 *
 * Copyright (C) 2017 Intel Corporation
 * Author: Heikki Krogerus <heikki.krogerus@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/* This is an example of an USB Type-C alternate mode driver.  */

#include <linux/module.h>
#include <linux/usb/pd_vdo.h>
#include <linux/usb/typec_altmode.h>

#define DUMMY_SVID	0xffff

/* Dummy vendor specific commands */
#define CMD_DUMMY1		VDO_CMD_VENDOR(1)
#define CMD_DUMMY2		VDO_CMD_VENDOR(2)

/* Dummy pin configurations */
enum {
	DUMMY_CONF_USB,
	DUMMY_CONF_A,
	DUMMY_CONF_B,
};

static void dummy_attention(struct typec_altmode *altmode,
			    u32 header, u32 *vdo, int count)
{
	/* Process attention.. */
}

static void dummy_altmode_vdm(struct typec_altmode *altmode,
			      u32 header, u32 *vdo, int count)
{
	int cmd_type = PD_VDO_CMDT(header);
	int cmd = PD_VDO_CMD(header);
	u32 message[2];

	switch (cmd_type) {
	case CMDT_INIT:
		/* Dummy altmode driver supports currently only DFP */

		if (cmd == CMD_ATTENTION)
			dummy_attention(altmode, header, vdo, count);
		break;
	case CMDT_RSP_ACK:
		switch (cmd) {
		case CMD_DISCOVER_MODES:
			/* We could store the modes here. */
			break;
		case CMD_ENTER_MODE:
			/* Prepare the platform for pin configuration A */
			if (typec_altmode_notify(altmode, DUMMY_CONF_A, NULL)) {
				/* Exit Mode? */
				break;
			}

			/* Queue dummy1 command for pin configuration A */
			message[0] = VDO(DUMMY_SVID, 1, CMD_DUMMY1);
			message[1] = 0x12345678 | DUMMY_CONF_A;
			typec_altmode_send_vdm(altmode, message[0],
					       &message[1], 2);

			break;
		case CMD_EXIT_MODE:
			/*
			 * Tell the the platform to put the port to go back to
			 * USB mode
			 */
			typec_altmode_notify(altmode, DUMMY_CONF_USB, NULL);

			break;
		case CMD_DUMMY1:
			/* We are happy */
			break;
		default:
			break;
		}
		break;
	case CMDT_RSP_NAK:
		switch (cmd) {
		case CMD_DUMMY1:
			/* Port back to USB mode */
			typec_altmode_notify(altmode, DUMMY_CONF_USB, NULL);

			/* Exit Mode? */

			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}

static int dummy_altmode_notify(struct typec_altmode *altmode,
				unsigned long conf, void *data)
{
}

static struct typec_altmode_ops dummy_altmode_ops = {
	.vdm = dummy_altmode_vdm,
	.notify = dummy_altmode_notify,
};

static int dummy_altmode_probe(struct typec_altmode *alt)
{
	typec_altmode_register_ops(alt, &dummy_altmode_ops);

	return 0;
}

static struct typec_altmode_driver dummy_altmode_driver = {
	.svid = DUMMY_SVID,
	.probe = dummy_altmode_probe,
	.driver = {
		.name = "dummy_altmode",
		.owner = THIS_MODULE,
	},
};

module_typec_altmode_driver(dummy_altmode_driver);

MODULE_AUTHOR("Heikki Krogerus <heikki.krogerus@linux.intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DisplayPort Alternate Mode");
