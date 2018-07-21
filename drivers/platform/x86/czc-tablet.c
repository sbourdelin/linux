// SPDX-License-Identifier: GPL-2.0+
/*
 * CZC Tablet Support
 *
 * Copyright (C) 2018 Lubomir Rintel <lkundrak@v3.sk>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/dmi.h>

static bool force;
module_param(force, bool, 0);
MODULE_PARM_DESC(force, "Disable the DMI check and force the driver to be loaded");

/*
 * The device boots up in "Windows 7" mode, when the home button sends a
 * Windows specific key sequence (Left Meta + D) and the second button
 * sends an unknown one while also toggling the Radio Kill Switch.
 * This is a surprising behavior when the second button is labeled "Back".
 *
 * The vendor-supplied Android-x86 build switches the device to a "Android"
 * mode by writing value 0x63 to the I/O port 0x68. This just seems to just
 * set bit 6 on address 0x96 in the EC region; switching the bit directly
 * seems to achieve the same result. It uses a "p10t_switcher" to do the
 * job. It doesn't seem to be able to do anything else, and no other use
 * of the port 0x68 is known.
 *
 * In the Android mode, the home button sends just a single scancode,
 * which can be handled in Linux userspace more reasonably and the back
 * button only sends a scancode without toggling the kill switch.
 * The scancode can then be mapped either to Back or RF Kill functionality
 * in userspace, depending on how the button is labeled on that particular
 * model.
 */

#define CZC_EC_EXTRA_PORT   0x68

#define CZC_EC_ANDROID_KEYS 0x63

static const struct dmi_system_id czc_tablet_table[] __initconst = {
	{
		.ident = "CZC ODEON TPC-10 (\"P10T\")",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "CZC"),
			DMI_MATCH(DMI_PRODUCT_NAME, "ODEON*TPC-10"),
		},
	},
	{
		.ident = "ViewSonic ViewPad 10",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ViewSonic"),
			DMI_MATCH(DMI_PRODUCT_NAME, "VPAD10"),
		},
	},
	{ }
};

static int __init czc_tablet_init(void)
{
	if (!force && !dmi_check_system(czc_tablet_table))
		return -ENODEV;

	outb(CZC_EC_ANDROID_KEYS, CZC_EC_EXTRA_PORT);

	return 0;
}

static void __exit czc_tablet_exit(void)
{
}

module_init(czc_tablet_init);
module_exit(czc_tablet_exit);

MODULE_DEVICE_TABLE(dmi, czc_tablet_table);

MODULE_AUTHOR("Lubomir Rintel <lkundrak@v3.sk>");
MODULE_DESCRIPTION("CZC Tablet Support");
MODULE_LICENSE("GPL");
