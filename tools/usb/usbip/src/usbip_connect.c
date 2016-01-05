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

#ifndef AS_LIBRARY
#include <getopt.h>
#endif
#include <unistd.h>

#include "usbip_host_driver.h"
#include "usbip_common.h"
#include "usbip_network.h"
#include "usbip_ux.h"
#include "usbip.h"

#ifndef AS_LIBRARY
static const char usbip_connect_usage_string[] =
	"usbip connect <args>\n"
	"    -r, --remote=<host>    Address of a remote computer\n"
	"    -b, --busid=<busid>    Bus ID of a device to be connected\n";

void usbip_connect_usage(void)
{
	printf("usage: %s", usbip_connect_usage_string);
}
#endif

static int send_export_device(usbip_sock_t *sock, struct usbip_usb_device *udev)
{
	int rc;
	struct op_export_request request;
	struct op_export_reply   reply;
	uint16_t code = OP_REP_EXPORT;

	memset(&request, 0, sizeof(request));
	memset(&reply, 0, sizeof(reply));

	/* send a request */
	rc = usbip_net_send_op_common(sock, OP_REQ_EXPORT, 0);
	if (rc < 0) {
		err("send op_common");
		return -1;
	}

	memcpy(&request.udev, udev, sizeof(struct usbip_usb_device));

	PACK_OP_EXPORT_REQUEST(0, &request);

	rc = usbip_net_send(sock, (void *) &request, sizeof(request));
	if (rc < 0) {
		err("send op_export_request");
		return -1;
	}

	/* receive a reply */
	rc = usbip_net_recv_op_common(sock, &code);
	if (rc < 0) {
		err("recv op_common");
		return -1;
	}

	rc = usbip_net_recv(sock, (void *) &reply, sizeof(reply));
	if (rc < 0) {
		err("recv op_export_reply");
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

static int export_device(char *busid, usbip_sock_t *sock)
{
	int rc;
	struct usbip_exported_device *edev;

	rc = usbip_host_driver_open();
	if (rc < 0) {
		err("open host_driver");
		return -1;
	}

	rc = usbip_host_refresh_device_list();
	if (rc < 0) {
		err("could not refresh device list");
		usbip_host_driver_close();
		return -1;
	}

	edev = usbip_host_find_device(busid);
	if (edev == NULL) {
		err("find device");
		usbip_host_driver_close();
		return -1;
	}

	rc = send_export_device(sock, &edev->udev);
	if (rc < 0) {
		err("send export");
		usbip_host_driver_close();
		return -1;
	}

	rc = usbip_host_export_device(edev, sock->fd);
	if (rc < 0) {
		err("export device");
		usbip_host_driver_close();
		return -1;
	}

	usbip_host_driver_close();

	return 0;
}

int usbip_connect_device(char *host, char *port, char *busid)
{
	usbip_sock_t *sock;
	usbip_ux_t *ux;
	int rc;

	rc = usbip_bind_device(busid);
	if (rc) {
		err("bind");
		goto err_out;
	}

	sock = usbip_conn_ops.open(host, port);
	if (!sock) {
		err("tcp connect");
		goto err_unbind_device;
	}

	rc = usbip_ux_setup(sock, &ux);
	if (rc) {
		err("ux setup");
		goto err_close_conn;
	}

	rc = export_device(busid, sock);
	if (rc < 0) {
		err("export");
		goto err_cleanup_ux;
	}

	if (ux != NULL) {
		usbip_ux_start(ux);
		usbip_ux_join(ux);
		usbip_unbind_device(busid);
	}
	usbip_ux_cleanup(&ux);
	usbip_conn_ops.close(sock);

	return 0;
err_cleanup_ux:
	usbip_ux_cleanup(&ux);
err_close_conn:
	usbip_conn_ops.close(sock);
err_unbind_device:
	usbip_unbind_device(busid);
err_out:
	return -1;
}

#ifndef AS_LIBRARY
int usbip_connect(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "remote", required_argument, NULL, 'r' },
		{ "busid",  required_argument, NULL, 'b' },
		{ NULL, 0,  NULL, 0 }
	};
	char *host = NULL;
	char *busid = NULL;
	int opt;
	int ret = -1;

	for (;;) {
		opt = getopt_long(argc, argv, "r:b:", opts, NULL);

		if (opt == -1)
			break;

		switch (opt) {
		case 'r':
			host = optarg;
			break;
		case 'b':
			busid = optarg;
			break;
		default:
			goto err_out;
		}
	}

	if (!host || !busid)
		goto err_out;

	ret = usbip_connect_device(host, usbip_port_string, busid);
	goto out;

err_out:
	usbip_connect_usage();
out:
	return ret;
}
#endif
