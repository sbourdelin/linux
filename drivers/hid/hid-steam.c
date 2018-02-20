// SPDX-License-Identifier: GPL-2.0
/*
 * HID driver for Valve Steam Controller
 *
 * Supports both the wired and wireless interfaces.
 *
 * Copyright (c) 2018 Rodrigo Rivas Costa <rodrigorivascosta@gmail.com>
 */

#include <linux/device.h>
#include <linux/input.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/power_supply.h>
#include "hid-ids.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rodrigo Rivas Costa <rodrigorivascosta@gmail.com>");

#define STEAM_QUIRK_WIRELESS		BIT(0)

/* Touch pads are 40 mm in diameter and 65535 units */
#define STEAM_PAD_RESOLUTION 1638
/* Trigger runs are about 5 mm and 256 units */
#define STEAM_TRIGGER_RESOLUTION 51

struct steam_device {
	spinlock_t lock;
	struct hid_device *hdev;
	struct input_dev *input;
	unsigned long quirks;
	struct work_struct work_connect;
	bool connected;
	char serial_no[11];
};

static int steam_recv_report(struct steam_device *steam,
		u8 *data, int size)
{
	struct hid_report *r;
	u8 *buf;
	int ret;

	r = steam->hdev->report_enum[HID_FEATURE_REPORT].report_id_hash[0];
	if (hid_report_len(r) < 64)
		return -EINVAL;
	buf = hid_alloc_report_buf(r, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/*
	 * The report ID is always 0, so strip the first byte from the output.
	 * hid_report_len() is not counting the report ID, so +1 to the length
	 * or else we get a EOVERFLOW. We are safe from a buffer overflow
	 * because hid_alloc_report_buf() allocates +7 bytes.
	 */
	ret = hid_hw_raw_request(steam->hdev, 0x00,
			buf, hid_report_len(r) + 1,
			HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret > 0)
		memcpy(data, buf + 1, min(size, ret - 1));
	kfree(buf);
	return ret;
}

static int steam_send_report(struct steam_device *steam,
		u8 *cmd, int size)
{
	struct hid_report *r;
	u8 *buf;
	int retry;
	int ret;

	r = steam->hdev->report_enum[HID_FEATURE_REPORT].report_id_hash[0];
	if (hid_report_len(r) < 64)
		return -EINVAL;
	buf = hid_alloc_report_buf(r, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* The report ID is always 0 */
	memcpy(buf + 1, cmd, size);

	/*
	 * Sometimes the wireless controller fails with EPIPE
	 * when sending a feature report.
	 * Doing a HID_REQ_GET_REPORT and waiting for a while
	 * seems to fix that.
	 */
	for (retry = 0; retry < 10; ++retry) {
		ret = hid_hw_raw_request(steam->hdev, 0,
				buf, size + 1,
				HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
		if (ret != -EPIPE)
			break;
		steam_recv_report(steam, NULL, 0);
		msleep(50);
	}
	kfree(buf);
	if (ret < 0)
		hid_err(steam->hdev, "%s: error %d (%*ph)\n", __func__,
				ret, size, cmd);
	return ret;
}

static int steam_get_serial(struct steam_device *steam)
{
	/*
	 * Send: 0xae 0x15 0x01
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

static int steam_input_open(struct input_dev *dev)
{
	struct steam_device *steam = input_get_drvdata(dev);

	return hid_hw_open(steam->hdev);
}

static void steam_input_close(struct input_dev *dev)
{
	struct steam_device *steam = input_get_drvdata(dev);

	hid_hw_close(steam->hdev);
}

static int steam_register(struct steam_device *steam)
{
	struct hid_device *hdev = steam->hdev;
	struct input_dev *input;
	int ret;

	ret = steam_get_serial(steam);
	if (ret)
		return ret;

	hid_info(hdev, "Steam Controller '%s' connected",
			steam->serial_no);

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;

	input_set_drvdata(input, steam);
	input->dev.parent = &hdev->dev;
	input->open = steam_input_open;
	input->close = steam_input_close;

	input->name = (steam->quirks & STEAM_QUIRK_WIRELESS) ?
		"Wireless Steam Controller" :
		"Steam Controller";
	input->phys = hdev->phys;
	input->uniq = steam->serial_no;
	input->id.bustype = hdev->bus;
	input->id.vendor = hdev->vendor;
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
	input_abs_set_res(input, ABS_X, STEAM_PAD_RESOLUTION);
	input_abs_set_res(input, ABS_Y, STEAM_PAD_RESOLUTION);
	input_abs_set_res(input, ABS_RX, STEAM_PAD_RESOLUTION);
	input_abs_set_res(input, ABS_RY, STEAM_PAD_RESOLUTION);
	input_abs_set_res(input, ABS_Z, STEAM_TRIGGER_RESOLUTION);
	input_abs_set_res(input, ABS_RZ, STEAM_TRIGGER_RESOLUTION);

	ret = input_register_device(input);
	if (ret)
		goto input_register_fail;

	steam->input = input;

	return 0;

input_register_fail:
	input_free_device(input);
	return ret;
}

static void steam_unregister(struct steam_device *steam)
{
	if (steam->input) {
		hid_info(steam->hdev, "Steam Controller '%s' disconnected",
				steam->serial_no);
		input_unregister_device(steam->input);
		steam->input = NULL;
	}
}

static void steam_work_connect_cb(struct work_struct *work)
{
	struct steam_device *steam = container_of(work, struct steam_device,
							work_connect);
	unsigned long flags;
	bool connected;
	int ret;

	spin_lock_irqsave(&steam->lock, flags);
	connected = steam->connected;
	spin_unlock_irqrestore(&steam->lock, flags);

	if (connected) {
		if (steam->input) {
			dbg_hid("%s: already connected\n", __func__);
			return;
		}
		ret = steam_register(steam);
		if (ret) {
			hid_err(steam->hdev,
				"%s:steam_register failed with error %d\n",
				__func__, ret);
			return;
		}
	} else {
		steam_unregister(steam);
	}
}

static bool steam_is_valve_interface(struct hid_device *hdev)
{
	struct hid_report_enum *rep_enum;
	struct hid_report *hreport;

	/*
	 * The wired device creates 3 interfaces:
	 *  0: emulated mouse.
	 *  1: emulated keyboard.
	 *  2: the real game pad.
	 * The wireless device creates 5 interfaces:
	 *  0: emulated keyboard.
	 *  1-4: slots where up to 4 real game pads will be connected to.
	 * We know which one is the real gamepad interface because they are the
	 * only ones with a feature report.
	 */
	rep_enum = &hdev->report_enum[HID_FEATURE_REPORT];
	list_for_each_entry(hreport, &rep_enum->report_list, list) {
		/* should we check hreport->id == 0? */
		return true;
	}
	return false;
}

static int steam_probe(struct hid_device *hdev,
				const struct hid_device_id *id)
{
	struct steam_device *steam;
	int ret;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev,
			"%s:parse of hid interface failed\n", __func__);
		return ret;
	}

	/*
	 * Since we have a proper gamepad now, we can ignore the virtual
	 * mouse and keyboard.
	 */
	if (!steam_is_valve_interface(hdev))
		return -ENODEV;

	steam = devm_kzalloc(&hdev->dev,
			sizeof(struct steam_device), GFP_KERNEL);
	if (!steam)
		return -ENOMEM;

	spin_lock_init(&steam->lock);
	steam->hdev = hdev;
	hid_set_drvdata(hdev, steam);
	steam->quirks = id->driver_data;
	INIT_WORK(&steam->work_connect, steam_work_connect_cb);

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hdev,
			"%s:hid_hw_start failed with error %d\n",
			__func__, ret);
		goto hid_hw_start_fail;
	}

	if (steam->quirks & STEAM_QUIRK_WIRELESS) {
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
				"%s:steam_register failed with error %d\n",
				__func__, ret);
			goto input_register_fail;
		}
	}

	return 0;

input_register_fail:
hid_hw_open_fail:
	hid_hw_stop(hdev);
hid_hw_start_fail:
	cancel_work_sync(&steam->work_connect);
	hid_set_drvdata(hdev, NULL);
	return ret;
}

static void steam_remove(struct hid_device *hdev)
{
	struct steam_device *steam = hid_get_drvdata(hdev);

	if (steam->quirks & STEAM_QUIRK_WIRELESS) {
		hid_info(hdev, "Steam wireless receiver disconnected");
		hid_hw_close(hdev);
	}
	hid_hw_stop(hdev);
	cancel_work_sync(&steam->work_connect);
	steam_unregister(steam);
	hid_set_drvdata(hdev, NULL);
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

/*
 * The size for this message payload is 60.
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
	struct input_dev *input = steam->input;

	/* 24 bits of buttons */
	u8 b8, b9, b10;

	/*
	 * If we get input events from the wireless without a 'connected'
	 * event, just connect it now.
	 * This can happen if we bind the HID device with the controller
	 * already paired.
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
		/* TODO battery status */
		break;
	}
	return 0;
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
