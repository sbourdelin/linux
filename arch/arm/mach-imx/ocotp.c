/*
 * Copyright (C) 2016 Freescale Semiconductor, Inc.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_net.h>
#include <linux/slab.h>

#include "hardware.h"

static void __iomem *ocotp_base;

void __init imx_ocotp_init(const char *compat)
{
	struct device_node *ocotp_np;

	ocotp_np = of_find_compatible_node(NULL, NULL, compat);
	if (!ocotp_np) {
		pr_warn("failed to find ocotp node\n");
		return;
	}

	ocotp_base = of_iomap(ocotp_np, 0);
	if (!ocotp_base)
		pr_warn("failed to map ocotp\n");

	of_node_put(ocotp_np);
}

u32 imx_ocotp_read(u32 offset)
{
	if (WARN_ON(!ocotp_base))
		return 0;

	return readl_relaxed(ocotp_base + offset);
}

#define OCOTP_MAC_OFF	(cpu_is_imx7d() ? 0x640 : 0x620)
#define OCOTP_MACn(n)	(OCOTP_MAC_OFF + (n) * 0x10)

void __init ocotp_enet_mac_init(const char *enet_compat)
{
	struct device_node *enet_np, *from = NULL;
	struct property *newmac;
	u32 macaddr_low;
	u32 macaddr_high = 0;
	u32 macaddr1_high = 0;
	u8 *macaddr;
	int i, id;

	for (i = 0; i < 2; i++) {
		enet_np = of_find_compatible_node(from, NULL, enet_compat);
		if (!enet_np)
			return;

		from = enet_np;

		if (of_get_mac_address(enet_np))
			goto put_enet_node;

		id = of_alias_get_id(enet_np, "ethernet");
		if (id < 0)
			id = i;

		macaddr_low = imx_ocotp_read(OCOTP_MACn(1));
		if (id)
			macaddr1_high = imx_ocotp_read(OCOTP_MACn(2));
		else
			macaddr_high = imx_ocotp_read(OCOTP_MACn(0));

		newmac = kzalloc(sizeof(*newmac) + 6, GFP_KERNEL);
		if (!newmac)
			goto put_enet_node;

		newmac->value = newmac + 1;
		newmac->length = 6;
		newmac->name = kstrdup("local-mac-address", GFP_KERNEL);
		if (!newmac->name) {
			kfree(newmac);
			goto put_enet_node;
		}

		macaddr = newmac->value;
		if (id) {
			macaddr[5] = (macaddr_low >> 16) & 0xff;
			macaddr[4] = (macaddr_low >> 24) & 0xff;
			macaddr[3] = macaddr1_high & 0xff;
			macaddr[2] = (macaddr1_high >> 8) & 0xff;
			macaddr[1] = (macaddr1_high >> 16) & 0xff;
			macaddr[0] = (macaddr1_high >> 24) & 0xff;
		} else {
			macaddr[5] = macaddr_high & 0xff;
			macaddr[4] = (macaddr_high >> 8) & 0xff;
			macaddr[3] = (macaddr_high >> 16) & 0xff;
			macaddr[2] = (macaddr_high >> 24) & 0xff;
			macaddr[1] = macaddr_low & 0xff;
			macaddr[0] = (macaddr_low >> 8) & 0xff;
		}

		of_update_property(enet_np, newmac);

put_enet_node:
		of_node_put(enet_np);
	}
}
