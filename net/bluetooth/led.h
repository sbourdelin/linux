/*
 * Copyright 2015, Heiner Kallweit <hkallweit1@gmail.com>
 * This code is based on the MAC80211 LED code.
 * Original author: Johannes Berg <johannes.berg@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifdef CONFIG_BT_LEDS
void hci_led_assoc(struct hci_dev *hdev, bool associated);
void hci_led_radio(struct hci_dev *hdev, bool enabled);
void hci_led_init(struct hci_dev *hdev);
void hci_led_exit(struct hci_dev *hdev);
#else
static inline void hci_led_assoc(struct hci_dev *hdev, bool associated)
{
}

static inline void hci_led_radio(struct hci_dev *hdev, bool enabled)
{
}

static inline void hci_led_init(struct hci_dev *hdev)
{
}

static inline void hci_led_exit(struct hci_dev *hdev)
{
}
#endif
