/*
 * Copyright 2015, Heiner Kallweit <hkallweit1@gmail.com>
 * This code is based on the MAC80211 LED code.
 * Original author: Johannes Berg <johannes.berg@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "led.h"

#define cdev_to_hdev(cdev, name) container_of(cdev->trigger, \
					      struct hci_dev, \
					      name)

void hci_led_assoc(struct hci_dev *hdev, bool associated)
{
	int cnt;

	if (associated)
		cnt = atomic_inc_return(&hdev->assoc_led_cnt);
	else
		cnt = atomic_dec_return(&hdev->assoc_led_cnt);

	WARN_ON(cnt < 0);

	if (!atomic_read(&hdev->assoc_led_active))
		return;

	led_trigger_event(&hdev->assoc_led,
			  cnt ? LED_FULL : LED_OFF);
}

void hci_led_radio(struct hci_dev *hdev, bool enabled)
{
	int cnt;

	if (enabled)
		cnt = atomic_inc_return(&hdev->radio_led_cnt);
	else
		cnt = atomic_dec_return(&hdev->radio_led_cnt);

	WARN_ON(cnt < 0);

	if (!atomic_read(&hdev->radio_led_active))
		return;

	led_trigger_event(&hdev->radio_led,
			  cnt ? LED_FULL : LED_OFF);
}

static void hci_assoc_led_activate(struct led_classdev *led_cdev)
{
	struct hci_dev *hdev = cdev_to_hdev(led_cdev, assoc_led);
	int cnt = atomic_read(&hdev->assoc_led_cnt);

	atomic_inc(&hdev->assoc_led_active);

	led_trigger_event(&hdev->assoc_led, cnt ? LED_FULL : LED_OFF);
}

static void hci_assoc_led_deactivate(struct led_classdev *led_cdev)
{
	struct hci_dev *hdev = cdev_to_hdev(led_cdev, assoc_led);

	atomic_dec(&hdev->assoc_led_active);
}

static void hci_radio_led_activate(struct led_classdev *led_cdev)
{
	struct hci_dev *hdev = cdev_to_hdev(led_cdev, radio_led);
	int cnt = atomic_read(&hdev->radio_led_cnt);

	atomic_inc(&hdev->radio_led_active);

	led_trigger_event(&hdev->radio_led, cnt ? LED_FULL : LED_OFF);
}

static void hci_radio_led_deactivate(struct led_classdev *led_cdev)
{
	struct hci_dev *hdev = cdev_to_hdev(led_cdev, radio_led);

	atomic_dec(&hdev->radio_led_active);
}

void hci_led_init(struct hci_dev *hdev)
{
	/* initialize assoc_led */
	atomic_set(&hdev->assoc_led_cnt, 0);
	atomic_set(&hdev->assoc_led_active, 0);
	hdev->assoc_led.activate = hci_assoc_led_activate;
	hdev->assoc_led.deactivate = hci_assoc_led_deactivate;
	hdev->assoc_led.name = devm_kasprintf(&hdev->dev, GFP_KERNEL,
					      "%s-assoc", hdev->name);
	if (hdev->assoc_led.name &&
	    led_trigger_register(&hdev->assoc_led)) {
		devm_kfree(&hdev->dev, (void *)hdev->assoc_led.name);
		hdev->assoc_led.name = NULL;
	}

	/* initialize radio_led */
	atomic_set(&hdev->radio_led_cnt, 0);
	atomic_set(&hdev->radio_led_active, 0);
	hdev->radio_led.activate = hci_radio_led_activate;
	hdev->radio_led.deactivate = hci_radio_led_deactivate;
	hdev->radio_led.name = devm_kasprintf(&hdev->dev, GFP_KERNEL,
					      "%s-radio", hdev->name);
	if (hdev->radio_led.name &&
	    led_trigger_register(&hdev->radio_led)) {
		devm_kfree(&hdev->dev, (void *)hdev->radio_led.name);
		hdev->radio_led.name = NULL;
	}
}

void hci_led_exit(struct hci_dev *hdev)
{
	if (hdev->assoc_led.name)
		led_trigger_unregister(&hdev->assoc_led);
	if (hdev->radio_led.name)
		led_trigger_unregister(&hdev->radio_led);
}

