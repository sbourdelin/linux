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

#include <sys/types.h>
#include <libudev.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AS_LIBRARY
#include <getopt.h>
#endif
#include <unistd.h>

#include "usbip_common.h"
#include "usbip_network.h"
#include "usbip.h"

#ifndef AS_LIBRARY
static const char usbip_list_usage_string[] =
	"usbip list <args>\n"
	"    -p, --parsable         Parsable list format\n"
	"    -r, --remote=<host>    List the importable USB devices on <host>\n"
	"    -l, --local            List the local USB devices\n";

void usbip_list_usage(void)
{
	printf("usage: %s", usbip_list_usage_string);
}
#endif

static int get_importable_devices(char *host, usbip_sock_t *sock)
{
	char product_name[100];
	char class_name[100];
	struct op_devlist_reply reply;
	uint16_t code = OP_REP_DEVLIST;
	struct usbip_usb_device udev;
	struct usbip_usb_interface uintf;
	unsigned int i;
	int rc, j;

	rc = usbip_net_send_op_common(sock, OP_REQ_DEVLIST, 0);
	if (rc < 0) {
		dbg("usbip_net_send_op_common failed");
		return -1;
	}

	rc = usbip_net_recv_op_common(sock, &code);
	if (rc < 0) {
		dbg("usbip_net_recv_op_common failed");
		return -1;
	}

	memset(&reply, 0, sizeof(reply));
	rc = usbip_net_recv(sock, &reply, sizeof(reply));
	if (rc < 0) {
		dbg("usbip_net_recv_op_devlist failed");
		return -1;
	}
	PACK_OP_DEVLIST_REPLY(0, &reply);
	dbg("importable devices: %d\n", reply.ndev);

	if (reply.ndev == 0) {
		info("no importable devices found on %s", host);
		return 0;
	}

	printf("Importable USB devices\n");
	printf("======================\n");
	printf(" - %s\n", host);

	for (i = 0; i < reply.ndev; i++) {
		memset(&udev, 0, sizeof(udev));
		rc = usbip_net_recv(sock, &udev, sizeof(udev));
		if (rc < 0) {
			dbg("usbip_net_recv failed: usbip_usb_device[%d]", i);
			return -1;
		}
		usbip_net_pack_usb_device(0, &udev);

		usbip_names_get_product(product_name, sizeof(product_name),
					udev.idVendor, udev.idProduct);
		usbip_names_get_class(class_name, sizeof(class_name),
				      udev.bDeviceClass, udev.bDeviceSubClass,
				      udev.bDeviceProtocol);
		printf("%11s: %s\n", udev.busid, product_name);
		printf("%11s: %s\n", "", udev.path);
		printf("%11s: %s\n", "", class_name);

		for (j = 0; j < udev.bNumInterfaces; j++) {
			rc = usbip_net_recv(sock, &uintf, sizeof(uintf));
			if (rc < 0) {
				err("usbip_net_recv failed: usbip_usb_intf[%d]",
						j);

				return -1;
			}
			usbip_net_pack_usb_interface(0, &uintf);

			usbip_names_get_class(class_name, sizeof(class_name),
					uintf.bInterfaceClass,
					uintf.bInterfaceSubClass,
					uintf.bInterfaceProtocol);
			printf("%11s: %2d - %s\n", "", j, class_name);
		}

		printf("\n");
	}

	return 0;
}

int usbip_list_importable_devices(char *host, char* port)
{
	int rc;
	usbip_sock_t *sock;

	if (usbip_names_init(USBIDS_FILE))
		err("failed to open %s", USBIDS_FILE);

	sock = usbip_conn_ops.open(host, port);
	if (!sock) {
		err("could not connect to %s:%s: %s", host,
		    port, usbip_net_gai_strerror(sock->fd));
		goto err_names_free;
	}
	dbg("connected to %s:%s", host, port);

	rc = get_importable_devices(host, sock);
	if (rc < 0) {
		err("failed to get device list from %s", host);
		goto err_conn_close;
	}

	usbip_conn_ops.close(sock);
	usbip_names_free();

	return 0;

err_conn_close:
	usbip_conn_ops.close(sock);
err_names_free:
	usbip_names_free();
	return -1;
}

static void print_device(const char *busid, const char *vendor,
			 const char *product, int parsable)
{
	if (parsable)
		printf("busid=%s#usbid=%.4s:%.4s#", busid, vendor, product);
	else
		printf(" - busid %s (%.4s:%.4s)\n", busid, vendor, product);
}

static void print_product_name(char *product_name, int parsable)
{
	if (!parsable)
		printf("   %s\n", product_name);
}

int usbip_list_devices(int parsable)
{
	struct udev *udev;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;
	struct udev_device *dev;
	const char *path;
	const char *idVendor;
	const char *idProduct;
	const char *bConfValue;
	const char *bNumIntfs;
	const char *busid;
	char product_name[128];
	int ret = -1;

	if (usbip_names_init(USBIDS_FILE))
		err("failed to open %s", USBIDS_FILE);

	/* Create libudev context. */
	udev = udev_new();

	/* Create libudev device enumeration. */
	enumerate = udev_enumerate_new(udev);

	/* Take only USB devices that are not hubs and do not have
	 * the bInterfaceNumber attribute, i.e. are not interfaces.
	 */
	udev_enumerate_add_match_subsystem(enumerate, "usb");
	udev_enumerate_add_nomatch_sysattr(enumerate, "bDeviceClass", "09");
	udev_enumerate_add_nomatch_sysattr(enumerate, "bInterfaceNumber", NULL);
	udev_enumerate_scan_devices(enumerate);

	devices = udev_enumerate_get_list_entry(enumerate);

	/* Show information about each device. */
	udev_list_entry_foreach(dev_list_entry, devices) {
		path = udev_list_entry_get_name(dev_list_entry);
		dev = udev_device_new_from_syspath(udev, path);

		/* Get device information. */
		idVendor = udev_device_get_sysattr_value(dev, "idVendor");
		idProduct = udev_device_get_sysattr_value(dev, "idProduct");
		bConfValue = udev_device_get_sysattr_value(dev, "bConfigurationValue");
		bNumIntfs = udev_device_get_sysattr_value(dev, "bNumInterfaces");
		busid = udev_device_get_sysname(dev);
		if (!idVendor || !idProduct || !bConfValue || !bNumIntfs) {
			err("problem getting device attributes: %s",
			    strerror(errno));
			goto err_out;
		}

		/* Get product name. */
		usbip_names_get_product(product_name, sizeof(product_name),
					strtol(idVendor, NULL, 16),
					strtol(idProduct, NULL, 16));

		/* Print information. */
		print_device(busid, idVendor, idProduct, parsable);
		print_product_name(product_name, parsable);

		printf("\n");

		udev_device_unref(dev);
	}

	ret = 0;

err_out:
	udev_enumerate_unref(enumerate);
	udev_unref(udev);
	usbip_names_free();

	return ret;
}

#ifndef AS_LIBRARY
int usbip_list(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "parsable", no_argument,       NULL, 'p' },
		{ "remote",   required_argument, NULL, 'r' },
		{ "local",    no_argument,       NULL, 'l' },
		{ NULL,       0,                 NULL,  0  }
	};

	int local = 0;
	int remote = 0;
	int parsable = 0;
	int opt;
	char *host = NULL;

	for (;;) {
		opt = getopt_long(argc, argv, "pr:l", opts, NULL);

		if (opt == -1)
			break;

		switch (opt) {
		case 'p':
			parsable = 1;
			break;
		case 'r':
			remote = 1;
			host = optarg;
			break;
		case 'l':
			local = 1;
			break;
		default:
			goto err_out;
		}
	}
	if (remote && host != NULL) {
		return usbip_list_importable_devices(host, usbip_port_string);
	} else if (local) {
		return usbip_list_devices(parsable);
	}

err_out:
	usbip_list_usage();
	return -1;
}
#endif
