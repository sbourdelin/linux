/*
 * ISHTP-HID glue driver.
 *
 * Copyright (c) 2012-2016, Intel Corporation.
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

#include <linux/hid.h>
#include "ishtp/client.h"
#include "ishtp-hid.h"

/**
 * ishtp_hid_parse() - hid-core .parse() callback
 * @hid:	hid device instance
 *
 * This function gets called during call to hid_add_device
 */
static int ishtp_hid_parse(struct hid_device *hid)
{
	struct ishtp_cl *hid_ishtp_cl = hid->driver_data;
	struct ishtp_cl_data *client_data = hid_ishtp_cl->client_data;
	int	rv;

	rv = hid_parse_report(hid,
		client_data->report_descr[client_data->cur_hid_dev],
		client_data->report_descr_size[client_data->cur_hid_dev]);
	if (rv)
		return	rv;

	return 0;
}

/* Empty callbacks with success return code */
static int ishtp_hid_start(struct hid_device *hid)
{
	return 0;
}

static void ishtp_hid_stop(struct hid_device *hid)
{
}

static int ishtp_hid_open(struct hid_device *hid)
{
	return 0;
}

static void ishtp_hid_close(struct hid_device *hid)
{
}

static int ishtp_raw_request(struct hid_device *hdev, unsigned char reportnum,
	__u8 *buf, size_t len, unsigned char rtype, int reqtype)
{
	return 0;
}

/**
 * ishtp_hid_request() - hid-core .request() callback
 * @hid:	hid device instance
 * @rep:	pointer to hid_report
 * @reqtype:	type of req. [GET|SET]_REPORT
 *
 * This function is used to set/get feaure/input report.
 */
static void ishtp_hid_request(struct hid_device *hid, struct hid_report *rep,
	int reqtype)
{
	/* the specific report length, just HID part of it */
	unsigned int len = ((rep->size - 1) >> 3) + 1 + (rep->id > 0);
	char *buf;
	unsigned int header_size = sizeof(struct hostif_msg);

	len += header_size;

	switch (reqtype) {
	case HID_REQ_GET_REPORT:
		hid_ishtp_get_report(hid, rep->id, rep->type);
		break;
	case HID_REQ_SET_REPORT:
		/*
		 * Spare 7 bytes for 64b accesses through
		 * get/put_unaligned_le64()
		 */
		buf = kzalloc(len + 7, GFP_KERNEL);
		if (!buf)
			return;

		hid_output_report(rep, buf + header_size);
		hid_ishtp_set_feature(hid, buf, len, rep->id);
		kfree(buf);
		break;
	}
}

/**
 * ishtp_wait_for_response() - hid-core .wait() callback
 * @hid:	hid device instance
 *
 * This function is used to wait after get feaure/input report.
 */
static int ishtp_wait_for_response(struct hid_device *hid)
{
	struct ishtp_cl *hid_ishtp_cl = hid->driver_data;
	struct ishtp_cl_data *client_data = hid_ishtp_cl->client_data;

	if (!client_data->get_report_done)
		wait_event_timeout(client_data->ishtp_hid_wait,
				   client_data->get_report_done, 3 * HZ);

	if (!client_data->get_report_done) {
		hid_err(hid,
			"timeout waiting for response from ISHTP device\n");
		return -1;
	}

	client_data->get_report_done = 0;

	return 0;
}

static struct hid_ll_driver ishtp_hid_ll_driver = {
	.parse = ishtp_hid_parse,
	.start = ishtp_hid_start,
	.stop = ishtp_hid_stop,
	.open = ishtp_hid_open,
	.close = ishtp_hid_close,
	.request = ishtp_hid_request,
	.wait = ishtp_wait_for_response,
	.raw_request = ishtp_raw_request
};

/**
 * ishtp_hid_probe() - hid register ll driver
 * @curr_hid_dev:	Index of hid device calling to register
 * @hid_ishtp_cl:	ISHTP Client instance
 *
 * This function is used to allocate and add HID device.
 */
int ishtp_hid_probe(unsigned int cur_hid_dev, struct ishtp_cl *hid_ishtp_cl)
{
	int rv;
	struct hid_device	*hid;
	struct ishtp_cl_data *client_data = hid_ishtp_cl->client_data;

	hid = hid_allocate_device();
	if (IS_ERR(hid)) {
		rv = PTR_ERR(hid);
		return	-ENOMEM;
	}

	hid->driver_data = hid_ishtp_cl;

	client_data->hid_sensor_hubs[cur_hid_dev] = hid;

	hid->ll_driver = &ishtp_hid_ll_driver;
	hid->bus = BUS_ISHTP;
	hid->version = le16_to_cpu(ISH_HID_VERSION);
	hid->vendor = le16_to_cpu(ISH_HID_VENDOR);
	hid->product = le16_to_cpu(ISH_HID_PRODUCT);

	snprintf(hid->name, sizeof(hid->name), "%s %04hX:%04hX", "hid-ishtp",
		hid->vendor, hid->product);

	rv = hid_add_device(hid);
	if (rv) {
		if (rv != -ENODEV)
			hid_err(hid, "[hid-ishtp]: can't add HID device: %d\n",
				rv);
		kfree(hid);
		return	rv;
	}

	return 0;
}

/**
 * ishtp_hid_probe() - Remove registered hid device
 * @hid_ishtp_cl:	ISHTP Client instance
 *
 * This function is used to destroy allocatd HID device.
 */
void ishtp_hid_remove(struct ishtp_cl *hid_ishtp_cl)
{
	struct ishtp_cl_data *client_data = hid_ishtp_cl->client_data;
	int	i;

	for (i = 0; i < client_data->num_hid_devices; ++i)
		if (client_data->hid_sensor_hubs[i]) {
			hid_destroy_device(client_data->hid_sensor_hubs[i]);
			client_data->hid_sensor_hubs[i] = NULL;
		}
}
