/* Copyright (C) 2016 National Instruments Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/leds.h>
#include <linux/phy.h>
#include <linux/netdevice.h>

static struct phy_led_trigger *phy_speed_to_led_trigger(struct phy_device *phy,
							unsigned int speed)
{
	switch (speed) {
	case SPEED_10:
		return &phy->phy_led_trigger[0];
	case SPEED_100:
		return &phy->phy_led_trigger[1];
	case SPEED_1000:
		return &phy->phy_led_trigger[2];
	case SPEED_2500:
		return &phy->phy_led_trigger[3];
	case SPEED_10000:
		return &phy->phy_led_trigger[4];
	default:
		return NULL;
	}
}

void phy_led_trigger_change_speed(struct phy_device *phy)
{
	struct phy_led_trigger *plt;

	if (!phy->link)
		goto out_change_speed;

	if (phy->speed == 0)
		return;

	plt = phy_speed_to_led_trigger(phy, phy->speed);
	if (!plt) {
		netdev_alert(phy->attached_dev,
			     "Unsupported trigger speed %u (update phy_led_trigger.c)\n",
			     phy->speed);
		goto out_change_speed;
	}

	if (plt != phy->last_triggered) {
		led_trigger_event(&phy->last_triggered->trigger, LED_OFF);
		led_trigger_event(&plt->trigger, LED_FULL);
		phy->last_triggered = plt;
	}
	return;

out_change_speed:
	if (phy->last_triggered) {
		led_trigger_event(&phy->last_triggered->trigger,
				  LED_OFF);
		phy->last_triggered = NULL;
	}
}
EXPORT_SYMBOL_GPL(phy_led_trigger_change_speed);

static int phy_led_trigger_register(struct phy_device *phy,
				    struct phy_led_trigger *plt, int i)
{
	static const char * const name_suffix[] = {
		"10Mbps",
		"100Mbps",
		"1Gbps",
		"2.5Gbps",
		"10Gbps",
	};
	snprintf(plt->name, sizeof(plt->name), PHY_ID_FMT ":%s",
		 phy->mdio.bus->id, phy->mdio.addr, name_suffix[i]);
	plt->trigger.name = plt->name;
	return led_trigger_register(&plt->trigger);
}

static void phy_led_trigger_unregister(struct phy_led_trigger *plt)
{
	led_trigger_unregister(&plt->trigger);
}

int phy_led_triggers_register(struct phy_device *phy)
{
	int i, err;

	for (i = 0; i < ARRAY_SIZE(phy->phy_led_trigger); i++) {
		err = phy_led_trigger_register(phy, &phy->phy_led_trigger[i],
					       i);
		if (err)
			goto out_unreg;
	}

	phy->last_triggered = NULL;
	phy_led_trigger_change_speed(phy);

	return 0;

out_unreg:
	while (i--)
		phy_led_trigger_unregister(&phy->phy_led_trigger[i]);
	return err;
}
EXPORT_SYMBOL_GPL(phy_led_triggers_register);

void phy_led_triggers_unregister(struct phy_device *phy)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(phy->phy_led_trigger); i++)
		phy_led_trigger_unregister(&phy->phy_led_trigger[i]);
}
EXPORT_SYMBOL_GPL(phy_led_triggers_unregister);
