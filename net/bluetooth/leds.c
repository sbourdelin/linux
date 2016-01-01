/*
 * Copyright 2015, Heiner Kallweit <hkallweit1@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "leds.h"

struct hci_basic_led_trigger {
	struct led_trigger	led_trigger;
	atomic_t		cnt;
};

#define to_hci_basic_led_trigger(arg) container_of(arg, \
					struct hci_basic_led_trigger, \
					led_trigger)

static void hci_basic_led(struct led_trigger *trig, bool inc)
{
	struct hci_basic_led_trigger *htrig;
	int cnt;

	if (!trig)
		return;

	htrig = to_hci_basic_led_trigger(trig);

	if (inc)
		cnt = atomic_inc_return(&htrig->cnt);
	else
		cnt = atomic_dec_return(&htrig->cnt);

	WARN_ON(cnt < 0);

	led_trigger_event(trig, cnt ? LED_FULL : LED_OFF);
}

void hci_led_radio(struct hci_dev *hdev, bool enabled)
{
	hci_basic_led(hdev->radio_led, enabled);
}

static void hci_basic_led_activate(struct led_classdev *led_cdev)
{
	struct hci_basic_led_trigger *htrig;
	int cnt;

	htrig = to_hci_basic_led_trigger(led_cdev->trigger);
	cnt = atomic_read(&htrig->cnt);

	led_trigger_event(led_cdev->trigger, cnt ? LED_FULL : LED_OFF);
}

static struct led_trigger *hci_basic_led_allocate(struct hci_dev *hdev,
						  const char *name)
{
	struct hci_basic_led_trigger *htrig;

	htrig =	devm_kzalloc(&hdev->dev, sizeof(*htrig), GFP_KERNEL);
	if (!htrig)
		return NULL;

	atomic_set(&htrig->cnt, 0);
	htrig->led_trigger.activate = hci_basic_led_activate;
	htrig->led_trigger.name = devm_kasprintf(&hdev->dev, GFP_KERNEL,
						 "%s-%s", hdev->name,
						 name);
	if (!htrig->led_trigger.name)
		goto err_alloc;

	if (led_trigger_register(&htrig->led_trigger))
		goto err_register;

	return &htrig->led_trigger;

err_register:
	devm_kfree(&hdev->dev, (void *)htrig->led_trigger.name);
err_alloc:
	devm_kfree(&hdev->dev, htrig);
	return NULL;
}

void hci_led_init(struct hci_dev *hdev)
{
	/* initialize radio_led */
	hdev->radio_led = hci_basic_led_allocate(hdev, "radio");
}

void hci_led_exit(struct hci_dev *hdev)
{
	if (hdev->radio_led)
		led_trigger_unregister(hdev->radio_led);
}

