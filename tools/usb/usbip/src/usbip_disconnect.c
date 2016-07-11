/*
 * Copyright (C) 2015 Nobuo Iwata
 *               2011 matt mooney <mfm@muteddisk.com>
 *               2005-2007 Takahiro Hirofuchi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/stat.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <getopt.h>
#include <unistd.h>

#include "usbip_host_driver.h"
#include "usbip_host_common.h"
#include "usbip_device_driver.h"
#include "usbip_common.h"
#include "usbip_network.h"
#include "usbip.h"

static struct usbip_host_driver *driver = &host_driver;

static const char usbip_disconnect_usage_string[] =
	"usbip disconnect <args>\n"
	"    -r, --remote=<host>    Address of a remote computer\n"
	"    -b, --busid=<busid>    Bus ID of a device to be disconnected\n"
	"    -d, --device           Run with an alternate driver, e.g. vUDC\n";

void usbip_disconnect_usage(void)
{
	printf("usage: %s", usbip_disconnect_usage_string);
}

static int send_unexport_device(int sockfd, struct usbip_usb_device *udev)
{
	int rc;
	struct op_unexport_request request;
	struct op_unexport_reply   reply;
	uint16_t code = OP_REP_UNEXPORT;

	memset(&request, 0, sizeof(request));
	memset(&reply, 0, sizeof(reply));

	/* send a request */
	rc = usbip_net_send_op_common(sockfd, OP_REQ_UNEXPORT, 0);
	if (rc < 0) {
		err("send op_common");
		return -1;
	}

	memcpy(&request.udev, udev, sizeof(struct usbip_usb_device));

	PACK_OP_UNEXPORT_REQUEST(0, &request);

	rc = usbip_net_send(sockfd, (void *) &request, sizeof(request));
	if (rc < 0) {
		err("send op_export_request");
		return -1;
	}

	/* receive a reply */
	rc = usbip_net_recv_op_common(sockfd, &code);
	if (rc < 0) {
		err("recv op_common");
		return -1;
	}

	rc = usbip_net_recv(sockfd, (void *) &reply, sizeof(reply));
	if (rc < 0) {
		err("recv op_unexport_reply");
		return -1;
	}

	PACK_OP_EXPORT_REPLY(0, &reply);

	/* check the reply */
	if (reply.returncode) {
		err("recv error return %d", reply.returncode);
		return -1;
	}

	return 0;
}

static int unexport_device(char *busid, int sockfd)
{
	int rc;
	struct usbip_exported_device *edev;

	rc = usbip_driver_open(driver);
	if (rc < 0) {
		err("open driver");
		return -1;
	}

	rc = usbip_refresh_device_list(driver);
	if (rc < 0) {
		err("could not refresh device list");
		usbip_driver_close(driver);
		return -1;
	}

	edev = usbip_get_device(driver, busid);
	if (edev == NULL) {
		err("find device");
		usbip_driver_close(driver);
		return -1;
	}

	rc = send_unexport_device(sockfd, &edev->udev);
	if (rc < 0) {
		err("send unexport");
		usbip_driver_close(driver);
		return -1;
	}

	usbip_driver_close(driver);

	return 0;
}

static int disconnect_device(char *host, char *busid, int unbind)
{
	int sockfd;
	int rc;

	sockfd = usbip_net_tcp_connect(host, usbip_port_string);
	if (sockfd < 0) {
		err("tcp connect");
		return -1;
	}

	rc = unexport_device(busid, sockfd);
	if (rc < 0) {
		err("unexport");
		close(sockfd);
		return -1;
	}

	close(sockfd);

	if (unbind) {
		rc = usbip_unbind_device(busid);
		if (rc) {
			err("unbind");
			return -1;
		}
	}

	return 0;
}

int usbip_disconnect(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "remote", required_argument, NULL, 'r' },
		{ "busid",  required_argument, NULL, 'b' },
		{ "device", no_argument,       NULL, 'd' },
		{ NULL, 0,  NULL, 0 }
	};
	char *host = NULL;
	char *busid = NULL;
	int opt;
	int unbind = 1;
	int ret = -1;

	for (;;) {
		opt = getopt_long(argc, argv, "r:b:d", opts, NULL);

		if (opt == -1)
			break;

		switch (opt) {
		case 'r':
			host = optarg;
			break;
		case 'b':
			busid = optarg;
			break;
		case 'd':
			driver = &device_driver;
			unbind = 0;
			break;
		default:
			goto err_out;
		}
	}

	if (!host || !busid)
		goto err_out;

	ret = disconnect_device(host, busid, unbind);
	goto out;

err_out:
	usbip_disconnect_usage();
out:
	return ret;
}
