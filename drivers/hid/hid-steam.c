// SPDX-License-Identifier: GPL-2.0
/*
 * HID driver for Valve Steam Controller
 *
 * Supports both the wired and wireless interfaces.
 *
 * Copyright (c) 2018 Rodrigo Rivas Costa <rodrigorivascosta@gmail.com>
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/device.h>
#include <linux/input.h>
#include <linux/usb.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>
#include "hid-ids.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rodrigo Rivas Costa <rodrigorivascosta@gmail.com>");

#define STEAM_QUIRK_WIRELESS		BIT(0)

struct steam_device {
	spinlock_t lock;
	struct hid_device *hid_dev;
	struct input_dev *input_dev;
	unsigned long quirks;
	struct work_struct work_connect;
	bool connected;
	char serial_no[11];
	struct power_supply_desc battery_desc;
	struct power_supply *battery;
	u8 battery_charge;
	u16 voltage;
};

static int steam_register(struct steam_device *steam);
static void steam_unregister(struct steam_device *steam);
static void steam_do_connect_event(struct steam_device *steam, bool connected);
static void steam_do_input_event(struct steam_device *steam, u8 *data);
static int steam_send_report(struct steam_device *steam,
		u8 *cmd, int size);
static int steam_recv_report(struct steam_device *steam,
		u8 *data, int size);
static int steam_battery_register(struct steam_device *steam);
static void steam_do_battery_event(struct steam_device *steam, u8 *data);

static int steam_input_open(struct input_dev *dev)
{
	struct steam_device *steam = input_get_drvdata(dev);

	return hid_hw_open(steam->hid_dev);
}

static void steam_input_close(struct input_dev *dev)
{
	struct steam_device *steam = input_get_drvdata(dev);

	hid_hw_close(steam->hid_dev);
}

#define STEAM_FEATURE_REPORT_SIZE 65

static int steam_send_report(struct steam_device *steam,
		u8 *cmd, int size)
{
	int retry;
	int ret;
	u8 *buf = kzalloc(STEAM_FEATURE_REPORT_SIZE, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;

	/* The report ID is always 0 */
	memcpy(buf + 1, cmd, size);

	/* Sometimes the wireless controller fails with EPIPE
	 * when sending a feature report.
	 * Doing a HID_REQ_GET_REPORT and waiting for a while
	 * seems to fix that.
	 */
	for (retry = 0; retry < 10; ++retry) {
		ret = hid_hw_raw_request(steam->hid_dev, 0,
				buf, size + 1,
				HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
		if (ret != -EPIPE)
			break;
		dbg_hid("%s: failed, retrying (%d times)\n", __func__, retry+1);
		steam_recv_report(steam, NULL, 0);
		msleep(50);
	}
	kfree(buf);
	return ret;
}

static int steam_recv_report(struct steam_device *steam,
		u8 *data, int size)
{
	int ret;
	u8 *buf = kzalloc(STEAM_FEATURE_REPORT_SIZE, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;

	/* The report ID is always 0 */
	ret = hid_hw_raw_request(steam->hid_dev, 0x00,
			buf, STEAM_FEATURE_REPORT_SIZE,
			HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	memcpy(data, buf + 1, size);
	kfree(buf);
	return ret;
}

static int steam_get_serial(struct steam_device *steam)
{
	/* Send: 0xae 0x15 0x01
	 * Recv: 0xae 0x15 0x01 serialnumber (10 chars)
	 */
	int ret;
	u8 cmd[] = {0xae, 0x15, 0x01};
	u8 reply[14];

	ret = steam_send_report(steam, cmd, sizeof(cmd));
	if (ret < 0)
		return ret;
	ret = steam_recv_report(steam, reply, sizeof(reply));
	if (ret < 0)
		return ret;
	reply[13] = 0;
	strcpy(steam->serial_no, reply + 3);
	return 0;
}

static void steam_work_connect_cb(struct work_struct *work)
{
	struct steam_device *steam = container_of(work, struct steam_device,
							work_connect);
	unsigned long flags;
	bool connected;
	int ret;

	dbg_hid("%s\n", __func__);

	spin_lock_irqsave(&steam->lock, flags);
	connected = steam->connected;
	spin_unlock_irqrestore(&steam->lock, flags);

	if (connected) {
		if (steam->input_dev) {
			dbg_hid("%s: already connected\n", __func__);
			return;
		}
		ret = steam_register(steam);
		if (ret) {
			hid_err(steam->hid_dev,
				"%s:steam_register returned error %d\n",
				__func__, ret);
			return;
		}
	} else {
		steam_unregister(steam);
	}
}

static int steam_probe(struct hid_device *hdev,
			 const struct hid_device_id *id)
{
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
	struct steam_device *steam;
	int ret;

	dbg_hid("%s called for ifnum %d protocol %d\n", __func__,
		intf->cur_altsetting->desc.bInterfaceNumber,
		intf->cur_altsetting->desc.bInterfaceProtocol
		);

	/*
	 * The wired device creates 3 interfaces:
	 *  0: emulated mouse.
	 *  1: emulated keyboard.
	 *  2: the real game pad.
	 * The wireless device creates 5 interfaces:
	 *  0: emulated keyboard.
	 *  1-4: slots where up to 4 real game pads will be connected to.
	 * Instead of the interface index we use the protocol, it is 0
	 * for the real game pad.
	 * Since we have a real game pad now, we can ignore the virtual
	 * mouse and keyboard.
	 */
	if (intf->cur_altsetting->desc.bInterfaceProtocol != 0) {
		dbg_hid("%s: interface ignored\n", __func__);
		return -ENODEV;
	}

	steam = kzalloc(sizeof(struct steam_device), GFP_KERNEL);
	if (!steam)
		return -ENOMEM;

	spin_lock_init(&steam->lock);
	steam->hid_dev = hdev;
	hid_set_drvdata(hdev, steam);
	steam->quirks = id->driver_data;
	INIT_WORK(&steam->work_connect, steam_work_connect_cb);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev,
			"%s:parse of hid interface failed\n", __func__);
		goto hid_parse_fail;
	}
	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hdev,
			"%s:hid_hw_start returned error\n", __func__);
		goto hid_hw_start_fail;
	}

	if (steam->quirks & STEAM_QUIRK_WIRELESS) {
		steam->input_dev = NULL;
		ret = hid_hw_open(hdev);
		if (ret) {
			hid_err(hdev,
				"%s:hid_hw_open for wireless\n",
				__func__);
			goto hid_hw_open_fail;
		}
		hid_info(hdev, "Steam wireless receiver connected");
	} else {
		ret = steam_register(steam);
		if (ret) {
			hid_err(hdev,
				"%s:steam_register returned error\n",
				__func__);
			goto input_register_fail;
		}
	}

	return 0;

input_register_fail:
hid_hw_open_fail:
	hid_hw_stop(hdev);
hid_hw_start_fail:
hid_parse_fail:
	cancel_work_sync(&steam->work_connect);
	kfree(steam);
	hid_set_drvdata(hdev, NULL);
	return ret;
}

static void steam_remove(struct hid_device *hdev)
{
	struct steam_device *steam = hid_get_drvdata(hdev);

	dbg_hid("%s\n", __func__);

	if (steam->quirks & STEAM_QUIRK_WIRELESS) {
		hid_info(hdev, "Steam wireless receiver disconnected");
		hid_hw_close(hdev);
	}
	hid_hw_stop(hdev);
	steam_unregister(steam);
	cancel_work_sync(&steam->work_connect);
	kfree(steam);
	hid_set_drvdata(hdev, NULL);
}

static int steam_raw_event(struct hid_device *hdev,
			     struct hid_report *report, u8 *data,
			     int size)
{
	struct steam_device *steam = hid_get_drvdata(hdev);

	/*
	 * All messages are size=64, all values little-endian.
	 * The format is:
	 *  Offset| Meaning
	 * -------+--------------------------------------------
	 *  0-1   | always 0x01, 0x00, maybe protocol version?
	 *  2     | type of message
	 *  3     | length of the real payload (not checked)
	 *  4-n   | payload data, depends on the type
	 *
	 * There are these known types of message:
	 *  0x01: input data (60 bytes)
	 *  0x03: wireless connect/disconnect (1 byte)
	 *  0x04: battery status (11 bytes)
	 */

	if (size != 64 || data[0] != 1 || data[1] != 0)
		return 0;

	switch (data[2]) {
	case 0x01:
		steam_do_input_event(steam, data);
		break;
	case 0x03:
		/*
		 * The payload of this event is a single byte:
		 *  0x01: disconnected.
		 *  0x02: connected.
		 */
		switch (data[4]) {
		case 0x01:
			steam_do_connect_event(steam, false);
			break;
		case 0x02:
			steam_do_connect_event(steam, true);
			break;
		}
		break;
	case 0x04:
		if (steam->quirks & STEAM_QUIRK_WIRELESS)
			steam_do_battery_event(steam, data);
		break;
	}
	return 0;
}

static void steam_do_connect_event(struct steam_device *steam, bool connected)
{
	unsigned long flags;

	spin_lock_irqsave(&steam->lock, flags);
	steam->connected = connected;
	spin_unlock_irqrestore(&steam->lock, flags);

	if (schedule_work(&steam->work_connect) == 0)
		dbg_hid("%s: connected=%d event already queued\n",
				__func__, connected);
}

/* The size for this message payload is 60.
 * The known values are:
 *  (* values are not sent through wireless)
 *  (* accelerator/gyro is disabled by default)
 *  Offset| Type  | Mapped to |Meaning
 * -------+-------+-----------+--------------------------
 *  4-7   | u32   | --        | sequence number
 *  8-10  | 24bit | see below | buttons
 *  11    | u8    | ABS_Z     | left trigger
 *  12    | u8    | ABS_RZ    | right trigger
 *  13-15 | --    | --        | always 0
 *  16-17 | s16   | ABS_X     | X value
 *  18-19 | s16   | ABS_Y     | Y value
 *  20-21 | s16   | ABS_RX    | right-pad X value
 *  22-23 | s16   | ABS_RY    | right-pad Y value
 *  24-25 | s16   | --        | * left trigger
 *  26-27 | s16   | --        | * right trigger
 *  28-29 | s16   | --        | * accelerometer X value
 *  30-31 | s16   | --        | * accelerometer Y value
 *  32-33 | s16   | --        | * accelerometer Z value
 *  34-35 | s16   | --        | gyro X value
 *  36-36 | s16   | --        | gyro Y value
 *  38-39 | s16   | --        | gyro Z value
 *  40-41 | s16   | --        | quaternion W value
 *  42-43 | s16   | --        | quaternion X value
 *  44-45 | s16   | --        | quaternion Y value
 *  46-47 | s16   | --        | quaternion Z value
 *  48-49 | --    | --        | always 0
 *  50-51 | s16   | --        | * left trigger (uncalibrated)
 *  52-53 | s16   | --        | * right trigger (uncalibrated)
 *  54-55 | s16   | --        | * joystick X value (uncalibrated)
 *  56-57 | s16   | --        | * joystick Y value (uncalibrated)
 *  58-59 | s16   | --        | * left-pad X value
 *  60-61 | s16   | --        | * left-pad Y value
 *  62-63 | u16   | --        | * battery voltage
 *
 * The buttons are:
 *  Bit  | Mapped to  | Description
 * ------+------------+--------------------------------
 *  8.0  | BTN_TR2    | right trigger fully pressed
 *  8.1  | BTN_TL2    | left trigger fully pressed
 *  8.2  | BTN_TR     | right shoulder
 *  8.3  | BTN_TL     | left shoulder
 *  8.4  | BTN_Y      | button Y
 *  8.5  | BTN_B      | button B
 *  8.6  | BTN_X      | button X
 *  8.7  | BTN_A      | button A
 *  9.0  | -ABS_HAT0Y | lef-pad up
 *  9.1  | +ABS_HAT0X | lef-pad right
 *  9.2  | -ABS_HAT0X | lef-pad left
 *  9.3  | +ABS_HAT0Y | lef-pad down
 *  9.4  | BTN_SELECT | menu left
 *  9.5  | BTN_MODE   | steam logo
 *  9.6  | BTN_START  | menu right
 *  9.7  | BTN_GEAR_DOWN | left back lever
 * 10.0  | BTN_GEAR_UP   | right back lever
 * 10.1  | --         | left-pad clicked
 * 10.2  | BTN_THUMBR | right-pad clicked
 * 10.3  | --         | left-pad touched
 * 10.4  | --         | right-pad touched
 * 10.5  | --         | unknown
 * 10.6  | BTN_THUMBL | joystick clicked
 * 10.7  | --         | lpad_and_joy
 */

static void steam_do_input_event(struct steam_device *steam, u8 *data)
{
	struct input_dev *input = steam->input_dev;

	/* 24 bits of buttons */
	u8 b8, b9, b10;

	/*
	 * If we get input events from the wireless without a 'connected'
	 * event, just connect it now.
	 * This can happen, for example, if we bind the HID device with
	 * the controller already paired.
	 */
	if (unlikely(!input)) {
		dbg_hid("%s: input data without connect event\n", __func__);
		steam_do_connect_event(steam, true);
		return;
	}

	input_report_abs(input, ABS_Z, data[11]);
	input_report_abs(input, ABS_RZ, data[12]);

	input_report_abs(input, ABS_X,
			(s16) le16_to_cpup((__le16 *)(data + 16)));
	input_report_abs(input, ABS_Y,
			-(s16) le16_to_cpup((__le16 *)(data + 18)));
	input_report_abs(input, ABS_RX,
			(s16) le16_to_cpup((__le16 *)(data + 20)));
	input_report_abs(input, ABS_RY,
			-(s16) le16_to_cpup((__le16 *)(data + 22)));

	b8 = data[8];
	b9 = data[9];
	b10 = data[10];

	input_event(input, EV_KEY, BTN_TR2, !!(b8 & 0x01));
	input_event(input, EV_KEY, BTN_TL2, !!(b8 & 0x02));
	input_event(input, EV_KEY, BTN_TR, !!(b8 & 0x04));
	input_event(input, EV_KEY, BTN_TL, !!(b8 & 0x08));
	input_event(input, EV_KEY, BTN_Y, !!(b8 & 0x10));
	input_event(input, EV_KEY, BTN_B, !!(b8 & 0x20));
	input_event(input, EV_KEY, BTN_X, !!(b8 & 0x40));
	input_event(input, EV_KEY, BTN_A, !!(b8 & 0x80));
	input_event(input, EV_KEY, BTN_SELECT, !!(b9 & 0x10));
	input_event(input, EV_KEY, BTN_MODE, !!(b9 & 0x20));
	input_event(input, EV_KEY, BTN_START, !!(b9 & 0x40));
	input_event(input, EV_KEY, BTN_GEAR_DOWN, !!(b9 & 0x80));
	input_event(input, EV_KEY, BTN_GEAR_UP, !!(b10 & 0x01));
	input_event(input, EV_KEY, BTN_THUMBR, !!(b10 & 0x04));
	input_event(input, EV_KEY, BTN_THUMBL, !!(b10 & 0x40));

	input_report_abs(input, ABS_HAT0X,
		 !!(b9 & 0x02) - !!(b9 & 0x04));
	input_report_abs(input, ABS_HAT0Y,
		 !!(b9 & 0x08) - !!(b9 & 0x01));

	input_sync(input);
}

static int steam_register(struct steam_device *steam)
{
	struct hid_device *hdev = steam->hid_dev;
	struct input_dev *input;
	int ret;

	dbg_hid("%s\n", __func__);

	ret = steam_get_serial(steam);
	if (ret)
		return ret;

	hid_info(hdev, "Steam Controller SN: '%s' connected",
			steam->serial_no);

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;

	input_set_drvdata(input, steam);
	input->dev.parent = &hdev->dev;
	input->open = steam_input_open;
	input->close = steam_input_close;

	input->name = "Steam Controller";
	input->phys = hdev->phys;
	input->uniq = steam->serial_no;
	input->id.bustype = hdev->bus;
	input->id.vendor  = hdev->vendor;
	input->id.product = hdev->product;
	input->id.version = hdev->version;

	input_set_capability(input, EV_KEY, BTN_TR2);
	input_set_capability(input, EV_KEY, BTN_TL2);
	input_set_capability(input, EV_KEY, BTN_TR);
	input_set_capability(input, EV_KEY, BTN_TL);
	input_set_capability(input, EV_KEY, BTN_Y);
	input_set_capability(input, EV_KEY, BTN_B);
	input_set_capability(input, EV_KEY, BTN_X);
	input_set_capability(input, EV_KEY, BTN_A);
	input_set_capability(input, EV_KEY, BTN_SELECT);
	input_set_capability(input, EV_KEY, BTN_MODE);
	input_set_capability(input, EV_KEY, BTN_START);
	input_set_capability(input, EV_KEY, BTN_GEAR_DOWN);
	input_set_capability(input, EV_KEY, BTN_GEAR_UP);
	input_set_capability(input, EV_KEY, BTN_THUMBR);
	input_set_capability(input, EV_KEY, BTN_THUMBL);

	input_set_abs_params(input, ABS_Z, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_RZ, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_X, -32767, 32767, 0, 0);
	input_set_abs_params(input, ABS_Y, -32767, 32767, 0, 0);
	input_set_abs_params(input, ABS_RX, -32767, 32767, 0, 0);
	input_set_abs_params(input, ABS_RY, -32767, 32767, 0, 0);
	input_set_abs_params(input, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(input, ABS_HAT0Y, -1, 1, 0, 0);

	ret = input_register_device(input);
	if (ret)
		goto input_register_fail;

	steam->input_dev = input;

	/* ignore battery errors, we can live without it */
	if (steam->quirks & STEAM_QUIRK_WIRELESS)
		steam_battery_register(steam);

	return 0;

input_register_fail:
	input_free_device(input);
	return ret;
}

static void steam_unregister(struct steam_device *steam)
{
	dbg_hid("%s\n", __func__);

	if (steam->battery) {
		power_supply_unregister(steam->battery);
		steam->battery = NULL;
		kfree(steam->battery_desc.name);
		steam->battery_desc.name = NULL;
	}
	if (steam->input_dev) {
		hid_info(steam->hid_dev, "Steam Controller SN: '%s' disconnected",
			steam->serial_no);
		input_unregister_device(steam->input_dev);
		steam->input_dev = NULL;
	}
}

/* The size for this message payload is 11.
 * The known values are:
 *  Offset| Type  | Meaning
 * -------+-------+---------------------------
 *  4-7   | u32   | sequence number
 *  8-11  | --    | always 0
 *  12-13 | u16   | voltage (mV)
 *  14    | u8    | battery percent
 */
static void steam_do_battery_event(struct steam_device *steam, u8 *data)
{
	unsigned long flags;
	s16 volts = le16_to_cpup((__le16 *)(data + 12));
	u8 batt = data[14];

	dbg_hid("%s: %d %d\n", __func__, volts, batt);

	if (unlikely(!steam->battery)) {
		dbg_hid("%s: battery data without connect event\n", __func__);
		steam_do_connect_event(steam, true);
		return;
	}

	spin_lock_irqsave(&steam->lock, flags);
	steam->voltage = volts;
	steam->battery_charge = batt;
	spin_unlock_irqrestore(&steam->lock, flags);

	power_supply_changed(steam->battery);
}

static enum power_supply_property steam_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
};

static int steam_battery_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct steam_device *steam = power_supply_get_drvdata(psy);
	unsigned long flags;
	s16 volts;
	u8 batt;
	int ret = 0;

	spin_lock_irqsave(&steam->lock, flags);
	volts = steam->voltage;
	batt = steam->battery_charge;
	spin_unlock_irqrestore(&steam->lock, flags);

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = volts * 1000; /* mV -> uV */
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = batt;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int steam_battery_register(struct steam_device *steam)
{
	struct power_supply *battery;
	struct power_supply_config battery_cfg = { .drv_data = steam, };
	unsigned long flags;
	int ret;

	dbg_hid("%s\n", __func__);

	steam->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	steam->battery_desc.properties = steam_battery_props;
	steam->battery_desc.num_properties = ARRAY_SIZE(steam_battery_props);
	steam->battery_desc.get_property = steam_battery_get_property;
	steam->battery_desc.name = kasprintf(GFP_KERNEL,
			"steam-controller-%s-battery", steam->serial_no);
	if (!steam->battery_desc.name) {
		ret = -ENOMEM;
		goto print_name_fail;
	}

	/* avoid the warning of 0% battery while waiting for the first info */
	spin_lock_irqsave(&steam->lock, flags);
	steam->voltage = 3000;
	steam->battery_charge = 100;
	spin_unlock_irqrestore(&steam->lock, flags);

	battery = power_supply_register(&steam->hid_dev->dev,
			&steam->battery_desc, &battery_cfg);
	if (IS_ERR(battery)) {
		ret = PTR_ERR(battery);
		hid_err(steam->hid_dev,
				"%s:power_supply_register returned error %d\n",
				__func__, ret);
		goto power_supply_reg_fail;
	}
	steam->battery = battery;
	power_supply_powers(steam->battery, &steam->hid_dev->dev);
	return 0;

power_supply_reg_fail:
	kfree(steam->battery_desc.name);
	steam->battery_desc.name = NULL;
print_name_fail:
	return ret;
}

static const struct hid_device_id steam_controllers[] = {
	{ /* Wired Steam Controller */
	  HID_USB_DEVICE(USB_VENDOR_ID_VALVE,
		USB_DEVICE_ID_STEAM_CONTROLLER)
	},
	{ /* Wireless Steam Controller */
	  HID_USB_DEVICE(USB_VENDOR_ID_VALVE,
		USB_DEVICE_ID_STEAM_CONTROLLER_WIRELESS),
	  .driver_data = STEAM_QUIRK_WIRELESS
	},
	{}
};

MODULE_DEVICE_TABLE(hid, steam_controllers);

static struct hid_driver steam_controller_driver = {
	.name = "hid-steam",
	.id_table = steam_controllers,
	.probe = steam_probe,
	.remove = steam_remove,
	.raw_event = steam_raw_event,
};

module_hid_driver(steam_controller_driver);
/* vi: set softtabstop=8 shiftwidth=8 noexpandtab tabstop=8: */
