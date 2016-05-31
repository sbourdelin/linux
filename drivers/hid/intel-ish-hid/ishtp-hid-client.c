/*
 * ISHTP client driver for HID (ISH)
 *
 * Copyright (c) 2014-2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */

#include <linux/module.h>
#include <linux/hid.h>
#include "ishtp/ishtp-dev.h"
#include "ishtp/client.h"
#include "ishtp-hid.h"

/* Rx ring buffer pool size */
#define HID_CL_RX_RING_SIZE	32
#define HID_CL_TX_RING_SIZE	16

/**
 * report_bad_packets() - Report bad packets
 * @hid_ishtp_cl:	Client instance to get stats
 * @recv_buf:		Raw received host interface message
 * @cur_pos:		Current position index in payload
 * @payload_len:	Length of payload expected
 *
 * Dumps error in case bad packet is received
 */
static void report_bad_packet(struct ishtp_cl *hid_ishtp_cl, void *recv_buf,
			      size_t cur_pos,  size_t payload_len)
{
	struct hostif_msg *recv_msg = recv_buf;
	struct ishtp_cl_data *client_data = hid_ishtp_cl->client_data;

	dev_err(&hid_ishtp_cl->device->dev, "[hid-ish]: BAD packet %02X\n"
		"total_bad=%u cur_pos=%u\n"
		"[%02X %02X %02X %02X]\n"
		"payload_len=%u\n"
		"multi_packet_cnt=%u\n"
		"is_response=%02X\n",
		recv_msg->hdr.command, client_data->bad_recv_cnt,
		(unsigned int)cur_pos,
		((unsigned char *)recv_msg)[0], ((unsigned char *)recv_msg)[1],
		((unsigned char *)recv_msg)[2], ((unsigned char *)recv_msg)[3],
		(unsigned int)payload_len, client_data->multi_packet_cnt,
		recv_msg->hdr.command & ~CMD_MASK);
}

/**
 * process_recv() - Received and parse incoming packet
 * @hid_ishtp_cl:	Client instance to get stats
 * @recv_buf:		Raw received host interface message
 * @data_len:		length of the message
 *
 * Parse the incoming packet. If it is a response packet then it will update
 * per instance flags and wake up the caller waiting to for the response.
 */
static void process_recv(struct ishtp_cl *hid_ishtp_cl, void *recv_buf,
			 size_t data_len)
{
	struct hostif_msg *recv_msg;
	unsigned char *payload;
	struct device_info *dev_info;
	int i, j;
	size_t	payload_len, total_len, cur_pos;
	int report_type;
	struct report_list *reports_list;
	char *reports;
	size_t report_len;
	struct ishtp_cl_data *client_data = hid_ishtp_cl->client_data;

	if (data_len < sizeof(struct hostif_msg_hdr)) {
		dev_err(&hid_ishtp_cl->device->dev,
			"[hid-ish]: error, received %u which is "
			"less than data header %u\n",
			(unsigned int)data_len,
			(unsigned int)sizeof(struct hostif_msg_hdr));
		++client_data->bad_recv_cnt;
		ish_hw_reset(hid_ishtp_cl->dev);
		return;
	}

	payload = recv_buf + sizeof(struct hostif_msg_hdr);
	total_len = data_len;
	cur_pos = 0;

	do {
		recv_msg = (struct hostif_msg *)(recv_buf + cur_pos);
		payload_len = recv_msg->hdr.size;

		/* Sanity checks */
		if (cur_pos + payload_len + sizeof(struct hostif_msg) >
				total_len) {
			++client_data->bad_recv_cnt;
			report_bad_packet(hid_ishtp_cl, recv_msg, cur_pos,
					  payload_len);
			ish_hw_reset(hid_ishtp_cl->dev);
			break;
		}


		switch (recv_msg->hdr.command & CMD_MASK) {
		case HOSTIF_DM_ENUM_DEVICES:
			if ((!(recv_msg->hdr.command & ~CMD_MASK) ||
					client_data->init_done)) {
				++client_data->bad_recv_cnt;
				report_bad_packet(hid_ishtp_cl, recv_msg,
						  cur_pos,
						  payload_len);
				ish_hw_reset(hid_ishtp_cl->dev);
				break;
			}
			client_data->hid_dev_count = (unsigned int)*payload;
			client_data->hid_devices = devm_kzalloc(
						&hid_ishtp_cl->device->dev,
						client_data->hid_dev_count *
						sizeof(struct device_info),
						GFP_KERNEL);
			if (!client_data->hid_devices) {
				dev_err(&hid_ishtp_cl->device->dev,
				"Mem alloc failed for hid device info\n");
				wake_up(&client_data->init_wait);
				break;
			}
			for (i = 0; i < client_data->hid_dev_count; ++i) {
				if (1 + sizeof(struct device_info) * i >=
						payload_len) {
					dev_err(&hid_ishtp_cl->device->dev,
						"[hid-ish]: [ENUM_DEVICES]:"
						" content size %lu "
						"is bigger than "
						"payload_len %u\n",
						1 + sizeof(struct device_info)
						* i,
						(unsigned int)payload_len);
				}

				if (1 + sizeof(struct device_info) * i >=
						data_len)
					break;

				dev_info = (struct device_info *)(payload + 1 +
					sizeof(struct device_info) * i);
				if (client_data->hid_devices)
					memcpy(client_data->hid_devices + i,
					       dev_info,
					       sizeof(struct device_info));
			}

			client_data->enum_devices_done = 1;
			wake_up(&client_data->init_wait);

			break;

		case HOSTIF_GET_HID_DESCRIPTOR:
			if ((!(recv_msg->hdr.command & ~CMD_MASK) ||
					client_data->init_done)) {
				++client_data->bad_recv_cnt;
				report_bad_packet(hid_ishtp_cl, recv_msg,
						  cur_pos,
						  payload_len);
				ish_hw_reset(hid_ishtp_cl->dev);
				break;
			}
			client_data->hid_descr[client_data->cur_hid_dev] =
				devm_kmalloc(&hid_ishtp_cl->device->dev,
					     payload_len, GFP_KERNEL);
			if (client_data->hid_descr[client_data->cur_hid_dev]) {
				memcpy(client_data->hid_descr[
						client_data->cur_hid_dev],
				       payload, payload_len);
				client_data->hid_descr_size[
				client_data->cur_hid_dev] = payload_len;
				client_data->hid_descr_done = 1;
			}
			wake_up(&client_data->init_wait);

			break;

		case HOSTIF_GET_REPORT_DESCRIPTOR:
			if ((!(recv_msg->hdr.command & ~CMD_MASK) ||
					client_data->init_done)) {
				++client_data->bad_recv_cnt;
				report_bad_packet(hid_ishtp_cl, recv_msg,
						  cur_pos,
						  payload_len);
				ish_hw_reset(hid_ishtp_cl->dev);
				break;
			}
			client_data->report_descr[client_data->cur_hid_dev] =
				devm_kmalloc(&hid_ishtp_cl->device->dev,
					     payload_len, GFP_KERNEL);
			if (client_data->report_descr[
						client_data->cur_hid_dev]) {
				memcpy(client_data->report_descr[
					client_data->cur_hid_dev], payload,
				       payload_len);
				client_data->report_descr_size[
				client_data->cur_hid_dev] = payload_len;
				client_data->report_descr_done = 1;
			}
			wake_up(&client_data->init_wait);

			break;

		case HOSTIF_GET_FEATURE_REPORT:
			report_type = HID_FEATURE_REPORT;
			goto	do_get_report;

		case HOSTIF_GET_INPUT_REPORT:
			report_type = HID_INPUT_REPORT;
do_get_report:
			/* Get index of device that matches this id */
			for (i = 0; i < client_data->num_hid_devices; ++i) {
				if (recv_msg->hdr.device_id ==
					client_data->hid_devices[i].dev_id)
					if (client_data->hid_sensor_hubs[i]) {
						hid_input_report(
						client_data->hid_sensor_hubs[
									i],
						report_type, payload,
						payload_len, 0);
						break;
					}
			}
			client_data->get_report_done = 1;
			wake_up(&client_data->ishtp_hid_wait);
			break;

		case HOSTIF_SET_FEATURE_REPORT:
			client_data->get_report_done = 1;
			wake_up(&client_data->ishtp_hid_wait);
			break;

		case HOSTIF_PUBLISH_INPUT_REPORT:
			report_type = HID_INPUT_REPORT;
			for (i = 0; i < client_data->num_hid_devices; ++i)
				if (recv_msg->hdr.device_id ==
					client_data->hid_devices[i].dev_id)
					if (client_data->hid_sensor_hubs[i])
						hid_input_report(
						client_data->hid_sensor_hubs[
									i],
						report_type, payload,
						payload_len, 0);
			break;

		case HOSTIF_PUBLISH_INPUT_REPORT_LIST:
			report_type = HID_INPUT_REPORT;
			reports_list = (struct report_list *)payload;
			reports = (char *)reports_list->reports;

			for (j = 0; j < reports_list->num_of_reports; j++) {
				recv_msg = (struct hostif_msg *)(reports +
					sizeof(uint16_t));
				report_len = *(uint16_t *)reports;
				payload = reports + sizeof(uint16_t) +
					sizeof(struct hostif_msg_hdr);
				payload_len = report_len -
					sizeof(struct hostif_msg_hdr);

				for (i = 0; i < client_data->num_hid_devices;
				     ++i)
					if (recv_msg->hdr.device_id ==
					client_data->hid_devices[i].dev_id &&
					client_data->hid_sensor_hubs[i]) {
						hid_input_report(
						client_data->hid_sensor_hubs[
									i],
						report_type,
						payload, payload_len,
						0);
					}

				reports += sizeof(uint16_t) + report_len;
			}
			break;
		default:
			++client_data->bad_recv_cnt;
			report_bad_packet(hid_ishtp_cl, recv_msg, cur_pos,
					  payload_len);
			ish_hw_reset(hid_ishtp_cl->dev);
			break;

		}

		if (!cur_pos && cur_pos + payload_len +
				sizeof(struct hostif_msg) < total_len)
			++client_data->multi_packet_cnt;

		cur_pos += payload_len + sizeof(struct hostif_msg);
		payload += payload_len + sizeof(struct hostif_msg);

	} while (cur_pos < total_len);
}

/**
 * ish_cl_event_cb() - bus driver callback for incoming message/packet
 * @device:	Pointer to the the ishtp client device for which this message
 *		is targeted
 *
 * Remove the packet from the list and process the message by calling
 * process_recv
 */
static void ish_cl_event_cb(struct ishtp_cl_device *device)
{
	struct ishtp_cl	*hid_ishtp_cl = device->driver_data;
	struct ishtp_cl_rb *rb_in_proc;
	size_t r_length;
	unsigned long flags;

	if (!hid_ishtp_cl)
		return;

	spin_lock_irqsave(&hid_ishtp_cl->in_process_spinlock, flags);
	while (!list_empty(&hid_ishtp_cl->in_process_list.list)) {
		rb_in_proc = list_entry(
			hid_ishtp_cl->in_process_list.list.next,
			struct ishtp_cl_rb, list);
		list_del_init(&rb_in_proc->list);
		spin_unlock_irqrestore(&hid_ishtp_cl->in_process_spinlock,
			flags);

		if (!rb_in_proc->buffer.data)
			return;

		r_length = rb_in_proc->buf_idx;

		/* decide what to do with received data */
		process_recv(hid_ishtp_cl, rb_in_proc->buffer.data, r_length);

		ishtp_io_rb_recycle(rb_in_proc);
		spin_lock_irqsave(&hid_ishtp_cl->in_process_spinlock, flags);
	}
	spin_unlock_irqrestore(&hid_ishtp_cl->in_process_spinlock, flags);
}

/**
 * hid_ishtp_set_feature() - send request to ISH FW to set a feature request
 * @hid:	hid device instance for this request
 * @buf:	feature buffer
 * @len:	Length of feature buffer
 * @report_id:	Report id for the feature set request
 *
 * This is called from hid core .request() callback. This function doesn't wait
 * for response.
 */
void hid_ishtp_set_feature(struct hid_device *hid, char *buf, unsigned int len,
			   int report_id)
{
	struct ishtp_cl *hid_ishtp_cl = hid->driver_data;
	struct ishtp_cl_data *client_data = hid_ishtp_cl->client_data;
	struct hostif_msg *msg = (struct hostif_msg *)buf;
	int	rv;
	int	i;

	memset(msg, 0, sizeof(struct hostif_msg));
	msg->hdr.command = HOSTIF_SET_FEATURE_REPORT;
	for (i = 0; i < client_data->num_hid_devices; ++i) {
		if (hid == client_data->hid_sensor_hubs[i]) {
			msg->hdr.device_id =
				client_data->hid_devices[i].dev_id;
			break;
		}
	}

	if (i == client_data->num_hid_devices)
		return;

	rv = ishtp_cl_send(hid_ishtp_cl, buf, len);
}

/**
 * hid_ishtp_get_report() - request to get feature/input report
 * @hid:	hid device instance for this request
 * @report_id:	Report id for the get request
 * @report_type:	Report type for the this request
 *
 * This is called from hid core .request() callback. This function will send
 * request to FW and return without waiting for response.
 */
void hid_ishtp_get_report(struct hid_device *hid, int report_id,
			  int report_type)
{
	struct ishtp_cl *hid_ishtp_cl = hid->driver_data;
	struct ishtp_cl_data *client_data = hid_ishtp_cl->client_data;
	static unsigned char	buf[10];
	unsigned int	len;
	struct hostif_msg_to_sensor *msg = (struct hostif_msg_to_sensor *)buf;
	int	rv;
	int	i;

	len = sizeof(struct hostif_msg_to_sensor);

	memset(msg, 0, sizeof(struct hostif_msg_to_sensor));
	msg->hdr.command = (report_type == HID_FEATURE_REPORT) ?
		HOSTIF_GET_FEATURE_REPORT : HOSTIF_GET_INPUT_REPORT;
	for (i = 0; i < client_data->num_hid_devices; ++i) {
		if (hid == client_data->hid_sensor_hubs[i]) {
			msg->hdr.device_id =
				client_data->hid_devices[i].dev_id;
			break;
		}
	}

	if (i == client_data->num_hid_devices)
		return;

	msg->report_id = report_id;
	rv = ishtp_cl_send(hid_ishtp_cl, buf, len);
}

/**
 * hid_ishtp_cl_init() - Init function for ISHTP client
 * @hid_istp_cl:	ISHTP client instance
 *
 * This function complete the initializtion of the client. The summary of
 * processing:
 * - Send request to enumerate the hid clients
 *	Get the HID descriptor for each enumearated device
 *	Get report description of each device
 *	Register each device wik hid core by calling ishtp_hid_probe
 */
static int hid_ishtp_cl_init(struct ishtp_cl *hid_ishtp_cl)
{
	static unsigned char buf[512];
	unsigned int len;
	struct hostif_msg *msg = (struct hostif_msg *)buf;
	struct ishtp_device *dev;
	int retry_count;
	unsigned long flags;
	struct ishtp_cl_data *client_data = hid_ishtp_cl->client_data;
	int i;
	int rv;

	init_waitqueue_head(&client_data->init_wait);
	init_waitqueue_head(&client_data->ishtp_hid_wait);

	rv = ishtp_cl_link(hid_ishtp_cl, ISHTP_HOST_CLIENT_ID_ANY);
	if (rv)
		return	-ENOMEM;

	client_data->init_done = 0;

	dev = hid_ishtp_cl->dev;

	/* Connect to FW client */
	hid_ishtp_cl->rx_ring_size = HID_CL_RX_RING_SIZE;
	hid_ishtp_cl->tx_ring_size = HID_CL_TX_RING_SIZE;

	spin_lock_irqsave(&dev->fw_clients_lock, flags);
	i = ishtp_fw_cl_by_uuid(dev, &hid_ishtp_guid);
	if (i < 0) {
		spin_unlock_irqrestore(&dev->fw_clients_lock, flags);
		return i;
	}
	hid_ishtp_cl->fw_client_id = dev->fw_clients[i].client_id;
	spin_unlock_irqrestore(&dev->fw_clients_lock, flags);
	hid_ishtp_cl->state = ISHTP_CL_CONNECTING;

	rv = ishtp_cl_connect(hid_ishtp_cl);
	if (rv)
		goto ret;

	/* Register read callback */
	ishtp_register_event_cb(hid_ishtp_cl->device, ish_cl_event_cb);

	/* Send HOSTIF_DM_ENUM_DEVICES */
	memset(msg, 0, sizeof(struct hostif_msg));
	msg->hdr.command = HOSTIF_DM_ENUM_DEVICES;
	len = sizeof(struct hostif_msg);
	rv = ishtp_cl_send(hid_ishtp_cl, buf, len);
	if (rv)
		goto ret;

	rv = 0;

	retry_count = 0;
	while (!client_data->enum_devices_done &&
	       retry_count < 10) {
		wait_event_timeout(client_data->init_wait,
				   client_data->enum_devices_done,
				   3 * HZ);
		++retry_count;
		if (!client_data->enum_devices_done)
			/* Send HOSTIF_DM_ENUM_DEVICES */
			rv = ishtp_cl_send(hid_ishtp_cl, buf, len);
	}
	if (!client_data->enum_devices_done) {
		dev_err(&hid_ishtp_cl->device->dev,
			"[hid-ish]: timed out waiting for enum_devices\n");
		rv = -ETIMEDOUT;
		goto ret;
	}
	if (!client_data->hid_devices) {
		dev_err(&hid_ishtp_cl->device->dev,
			"[hid-ish]: failed to allocate HID dev structures\n");
		rv = -ENOMEM;
		goto ret;
	}

	client_data->num_hid_devices = client_data->hid_dev_count;
	dev_warn(&hid_ishtp_cl->device->dev,
		"[hid-ish]: enum_devices_done OK, num_hid_devices=%d\n",
		client_data->num_hid_devices);

	for (i = 0; i < client_data->num_hid_devices; ++i) {
		client_data->cur_hid_dev = i;

		/* Get HID descriptor */
		client_data->hid_descr_done = 0;
		memset(msg, 0, sizeof(struct hostif_msg));
		msg->hdr.command = HOSTIF_GET_HID_DESCRIPTOR;
		msg->hdr.device_id = client_data->hid_devices[i].dev_id;
		len = sizeof(struct hostif_msg);
		rv = ishtp_cl_send(hid_ishtp_cl, buf, len);
		rv = 0;

		if (!client_data->hid_descr_done)
			wait_event_timeout(client_data->init_wait,
					   client_data->hid_descr_done,
					   30 * HZ);
		if (!client_data->hid_descr_done) {
			dev_err(&hid_ishtp_cl->device->dev,
				"[hid-ish]: timed out for hid_descr_done\n");
			continue;
		}

		if (!client_data->hid_descr[i]) {
			dev_err(&hid_ishtp_cl->device->dev,
				"[hid-ish]: allocation HID desc fail\n");
			continue;
		}

		/* Get report descriptor */
		client_data->report_descr_done = 0;
		memset(msg, 0, sizeof(struct hostif_msg));
		msg->hdr.command = HOSTIF_GET_REPORT_DESCRIPTOR;
		msg->hdr.device_id = client_data->hid_devices[i].dev_id;
		len = sizeof(struct hostif_msg);
		rv = ishtp_cl_send(hid_ishtp_cl, buf, len);

		rv = 0;

		if (!client_data->report_descr_done)
			wait_event_timeout(client_data->init_wait,
					   client_data->report_descr_done,
					   30 * HZ);
		if (!client_data->report_descr_done) {
			dev_err(&hid_ishtp_cl->device->dev,
				"[hid-ish]: timed out for report descr\n");
			continue;
		}

		if (!client_data->report_descr[i]) {
			dev_err(&hid_ishtp_cl->device->dev,
				"[hid-ish]: failed to alloc report descr\n");
			continue;
		}

		rv = ishtp_hid_probe(i, hid_ishtp_cl);
		if (rv) {
			dev_err(&hid_ishtp_cl->device->dev,
				"[hid-ish]: HID probe for #%u failed: %d\n",
				i, rv);
			continue;
		}
	} /* for() on all hid devices */

ret:
	client_data->init_done = 1;

	return rv;
}

/**
 * hid_ishtp_cl_probe() - ISHTP client driver probe
 * @cl_device:		ISHTP client device instance
 *
 * This function calls on device create on ISHTP bus
 */
static int hid_ishtp_cl_probe(struct ishtp_cl_device *cl_device)
{
	struct ishtp_cl *hid_ishtp_cl;
	struct ishtp_cl_data *client_data;
	int rv;

	if (!cl_device)
		return	-ENODEV;

	if (uuid_le_cmp(hid_ishtp_guid,
			cl_device->fw_client->props.protocol_name) != 0)
		return	-ENODEV;

	client_data = devm_kzalloc(&cl_device->dev, sizeof(*client_data),
				   GFP_KERNEL);
	if (!client_data)
		return -ENOMEM;

	hid_ishtp_cl = ishtp_cl_allocate(cl_device->ishtp_dev);
	if (!hid_ishtp_cl)
		return -ENOMEM;

	cl_device->driver_data = hid_ishtp_cl;
	hid_ishtp_cl->client_data = client_data;

	rv = hid_ishtp_cl_init(hid_ishtp_cl);
	if (rv) {
		ishtp_cl_free(hid_ishtp_cl);
		return rv;
	}

	return 0;
}

/**
 * hid_ishtp_cl_remove() - ISHTP client driver remove
 * @cl_device:		ISHTP client device instance
 *
 * This function calls on device create on ISHTP bus
 */
static int hid_ishtp_cl_remove(struct ishtp_cl_device *cl_device)
{
	struct ishtp_cl *hid_ishtp_cl = cl_device->driver_data;
	struct ishtp_cl_data *client_data = hid_ishtp_cl->client_data;

	ishtp_hid_remove(hid_ishtp_cl);

	ishtp_cl_unlink(hid_ishtp_cl);
	ishtp_cl_flush_queues(hid_ishtp_cl);

	/* disband and free all Tx and Rx client-level rings */
	ishtp_cl_free(hid_ishtp_cl);
	hid_ishtp_cl = NULL;

	client_data->num_hid_devices = 0;

	return 0;
}


struct ishtp_cl_driver	hid_ishtp_cl_driver = {
	.name = "ish",
	.probe = hid_ishtp_cl_probe,
	.remove = hid_ishtp_cl_remove,
};

static int __init ish_hid_init(void)
{
	int	rv;

	/* Register ISHTP client device driver with ISHTP Bus */
	rv = ishtp_cl_driver_register(&hid_ishtp_cl_driver);

	return rv;

}
late_initcall(ish_hid_init);

MODULE_DESCRIPTION("ISH ISHTP HID client driver");
/* Primary author */
MODULE_AUTHOR("Daniel Drubin <daniel.drubin@intel.com>");
/* Modification for multi instance support and clean up */
MODULE_AUTHOR("Srinivas Pandruvada <srinivas.pandruvada@linux.intel.com>");

MODULE_LICENSE("GPL");
