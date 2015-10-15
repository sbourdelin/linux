/**
 * 	mpc5xxx_get_bus_frequency - Find the bus frequency for a device
 * 	@node:	device node
 *
 * 	Returns bus frequency (IPS on MPC512x, IPB on MPC52xx),
 * 	or 0 if the bus frequency cannot be found.
 */

#include <linux/kernel.h>
#include <linux/of_platform.h>
#include <linux/export.h>
#include <asm/mpc5xxx.h>

unsigned long mpc5xxx_get_bus_frequency(struct device_node *node)
{
	u32 bus_freq = 0;

	of_node_get(node);
	while (node) {
		if (!of_property_read_u32(node, "bus-frequency", &bus_freq))
			break;

		node = of_get_next_parent(node);
	}
	of_node_put(node);

	return bus_freq;
}
EXPORT_SYMBOL(mpc5xxx_get_bus_frequency);
