// SPDX-License-Identifier: GPL-2.0
/*
 * wilco_ec_event - Event handling for Wilco Embedded Controller
 *
 * Copyright 2018 Google LLC
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include "wilco_ec.h"

/* ACPI Notify event code indicating event data is available */
#define EC_ACPI_NOTIFY_EVENT		0x90

/* ACPI Method to execute to retrieve event data buffer from the EC */
#define EC_ACPI_GET_EVENT		"^QSET"

/* Maximum number of words in event data returned by the EC */
#define EC_ACPI_MAX_EVENT_DATA		6

/* Keep at most 100 events in the queue */
#define EC_EVENT_QUEUE_MAX		100

/**
 * enum ec_event_type - EC event categories.
 * @EC_EVENT_TYPE_HOTKEY: Hotkey event for handling special keys.
 * @EC_EVENT_TYPE_NOTIFY: EC feature state changes.
 * @EC_EVENT_TYPE_SYSTEM: EC system messages.
 */
enum ec_event_type {
	EC_EVENT_TYPE_HOTKEY = 0x10,
	EC_EVENT_TYPE_NOTIFY = 0x11,
	EC_EVENT_TYPE_SYSTEM = 0x12,
};

/**
 * struct ec_event - Extended event returned by the EC.
 * @size: Number of words in structure after the size word.
 * @type: Extended event type from &enum ec_event_type.
 * @event: Event data words.  Max count is %EC_ACPI_MAX_EVENT_DATA.
 */
struct ec_event {
	u16 size;
	u16 type;
	u16 event[0];
} __packed;

/**
 * struct ec_event_entry - Event queue entry.
 * @list: List node.
 * @size: Number of bytes in event structure.
 * @event: Extended event returned by the EC.  This should be the last
 *         element because &struct ec_event includes a zero length array.
 */
struct ec_event_entry {
	struct list_head list;
	size_t size;
	struct ec_event event;
};

static const struct key_entry wilco_ec_keymap[] = {
	{ KE_KEY, 0x0057, { KEY_BRIGHTNESSDOWN } },
	{ KE_KEY, 0x0058, { KEY_BRIGHTNESSUP } },
	{ KE_END, 0 }
};

/**
 * wilco_ec_handle_events() - Handle Embedded Controller events.
 * @ec: EC device.
 * @buf: Buffer of event data.
 * @length: Length of event data buffer.
 *
 * Return: Number of events in queue or negative error code on failure.
 *
 * This function will handle EC extended events, sending hotkey events
 * to the input subsystem and queueing others to be read by userspace.
 */
static int wilco_ec_handle_events(struct wilco_ec_device *ec,
				  u8 *buf, u32 length)
{
	struct wilco_ec_event *queue = &ec->event;
	struct ec_event *event;
	struct ec_event_entry *entry;
	size_t entry_size, num_words;
	u32 offset = 0;

	while (offset < length) {
		event = (struct ec_event *)(buf + offset);
		if (!event)
			return -EINVAL;

		dev_dbg(ec->dev, "EC event type 0x%02x size %d\n", event->type,
			event->size);

		/* Number of 16bit event data words is size - 1 */
		num_words = event->size - 1;
		entry_size = sizeof(*event) + (num_words * sizeof(u16));

		if (num_words > EC_ACPI_MAX_EVENT_DATA) {
			dev_err(ec->dev, "Invalid event word count: %d > %d\n",
				num_words, EC_ACPI_MAX_EVENT_DATA);
			return -EOVERFLOW;
		};

		/* Ensure event does not overflow the available buffer */
		if ((offset + entry_size) > length) {
			dev_err(ec->dev, "Event exceeds buffer: %d > %d\n",
				offset + entry_size, length);
			return -EOVERFLOW;
		}

		/* Point to the next event in the buffer */
		offset += entry_size;

		/* Hotkeys are sent to the input subsystem */
		if (event->type == EC_EVENT_TYPE_HOTKEY) {
			if (sparse_keymap_report_event(queue->input,
						       event->event[0],
						       1, true))
				continue;

			/* Unknown hotkeys are put into the event queue */
			dev_dbg(ec->dev, "Unknown hotkey 0x%04x\n",
				event->event[0]);
		}

		/* Prepare event for the queue */
		entry = kzalloc(entry_size, GFP_KERNEL);
		if (!entry)
			return -ENOMEM;

		/* Copy event data */
		entry->size = entry_size;
		memcpy(&entry->event, event, entry->size);

		/* Add this event to the queue */
		mutex_lock(&queue->lock);
		list_add_tail(&entry->list, &queue->list);
		queue->count++;
		mutex_unlock(&queue->lock);
	}

	return queue->count;
}

/**
 * wilco_ec_acpi_notify() - Handler called by ACPI subsystem for Notify.
 * @device: EC ACPI device.
 * @value: Value passed to Notify() in ACPI.
 * @data: Private data, pointer to EC device.
 */
static void wilco_ec_acpi_notify(acpi_handle device, u32 value, void *data)
{
	struct wilco_ec_device *ec = data;
	struct acpi_buffer event_buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	int count;

	/* Currently only handle event notifications */
	if (value != EC_ACPI_NOTIFY_EVENT) {
		dev_err(ec->dev, "Invalid event: 0x%08x\n", value);
		return;
	}

	/* Execute ACPI method to get event data buffer */
	status = acpi_evaluate_object(device, EC_ACPI_GET_EVENT,
				      NULL, &event_buffer);
	if (ACPI_FAILURE(status)) {
		dev_err(ec->dev, "Error executing ACPI method %s()\n",
			 EC_ACPI_GET_EVENT);
		return;
	}

	obj = (union acpi_object *)event_buffer.pointer;
	if (!obj) {
		dev_err(ec->dev, "Nothing returned from %s()\n",
			EC_ACPI_GET_EVENT);
		return;
	}
	if (obj->type != ACPI_TYPE_BUFFER) {
		dev_err(ec->dev, "Invalid object returned from %s()\n",
			EC_ACPI_GET_EVENT);
		kfree(obj);
		return;
	}
	if (obj->buffer.length < sizeof(struct ec_event)) {
		dev_err(ec->dev, "Invalid buffer length %d from %s()\n",
			obj->buffer.length, EC_ACPI_GET_EVENT);
		kfree(obj);
		return;
	}

	/* Handle events and notify sysfs if any queued for userspace */
	count = wilco_ec_handle_events(ec, obj->buffer.pointer,
				       obj->buffer.length);

	if (count > 0) {
		dev_dbg(ec->dev, "EC event queue has %d entries\n", count);
		sysfs_notify(&ec->dev->kobj, NULL, "event");
	}

	kfree(obj);
}

static ssize_t wilco_ec_event_read(struct file *filp, struct kobject *kobj,
				   struct bin_attribute *attr,
				   char *buf, loff_t off, size_t count)
{
	struct wilco_ec_device *ec = attr->private;
	struct ec_event_entry *entry;

	/* Only supports reading full events */
	if (off != 0)
		return -EINVAL;

	/* Remove the first event and provide it to userspace */
	mutex_lock(&ec->event.lock);
	entry = list_first_entry_or_null(&ec->event.list,
					 struct ec_event_entry, list);
	if (entry) {
		if (entry->size < count)
			count = entry->size;
		memcpy(buf, &entry->event, count);
		list_del(&entry->list);
		kfree(entry);
		ec->event.count--;
	} else {
		count = 0;
	}
	mutex_unlock(&ec->event.lock);

	return count;
}

static void wilco_ec_event_clear(struct wilco_ec_device *ec)
{
	struct ec_event_entry *entry, *next;

	mutex_lock(&ec->event.lock);

	/* Clear the event queue */
	list_for_each_entry_safe(entry, next, &ec->event.list, list) {
		list_del(&entry->list);
		kfree(entry);
		ec->event.count--;
	}

	mutex_unlock(&ec->event.lock);
}

int wilco_ec_event_init(struct wilco_ec_device *ec)
{
	struct wilco_ec_event *event = &ec->event;
	struct device *dev = ec->dev;
	struct acpi_device *adev = ACPI_COMPANION(dev);
	acpi_status status;
	int ret;

	if (!adev) {
		dev_err(dev, "Unable to find Wilco ACPI Device\n");
		return -ENODEV;
	}

	INIT_LIST_HEAD(&event->list);
	mutex_init(&event->lock);

	/* Allocate input device for hotkeys */
	event->input = input_allocate_device();
	if (!event->input)
		return -ENOMEM;
	event->input->name = "Wilco EC hotkeys";
	event->input->phys = "ec/input0";
	event->input->id.bustype = BUS_HOST;
	ret = sparse_keymap_setup(event->input, wilco_ec_keymap, NULL);
	if (ret) {
		dev_err(dev, "Unable to setup input device keymap\n");
		input_free_device(event->input);
		return ret;
	}
	ret = input_register_device(event->input);
	if (ret) {
		dev_err(dev, "Unable to register input device\n");
		input_free_device(event->input);
		return ret;
	}

	/* Create sysfs attribute for userspace event handling */
	sysfs_bin_attr_init(&event->attr);
	event->attr.attr.name = "event";
	event->attr.attr.mode = 0400;
	event->attr.read = wilco_ec_event_read;
	event->attr.private = ec;
	ret = device_create_bin_file(dev, &event->attr);
	if (ret) {
		dev_err(dev, "Failed to create event attribute in sysfs\n");
		input_unregister_device(event->input);
		return ret;
	}

	/* Install ACPI handler for Notify events */
	status = acpi_install_notify_handler(adev->handle, ACPI_ALL_NOTIFY,
					     wilco_ec_acpi_notify, ec);

	if (ACPI_FAILURE(status)) {
		dev_err(dev, "Failed to register notifier %08x\n", status);
		device_remove_bin_file(dev, &event->attr);
		input_unregister_device(event->input);
		return -ENODEV;
	}

	return 0;
}

void wilco_ec_event_remove(struct wilco_ec_device *ec)
{
	struct acpi_device *adev = ACPI_COMPANION(ec->dev);

	/* Stop new events */
	if (adev)
		acpi_remove_notify_handler(adev->handle, ACPI_ALL_NOTIFY,
					   wilco_ec_acpi_notify);

	/* Remove event interfaces */
	device_remove_bin_file(ec->dev, &ec->event.attr);
	input_unregister_device(ec->event.input);

	/* Clear the event queue */
	wilco_ec_event_clear(ec);
}
