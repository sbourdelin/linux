// SPDX-License-Identifier: GPL-2.0+
/*
 *  Driver for the XAP-EA-00x series of the Xaptum Edge Access Card, a
 *  TPM 2.0-based hardware module for authenticating IoT devices and
 *  gateways.
 *
 *  Copyright (c) 2017 Xaptum, Inc.
 */

#ifndef _XAPEA00X_H
#define _XAPEA00X_H

#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#define USB_VENDOR_ID_SILABS           0x10c4
#define USB_VENDOR_ID_XAPTUM           0x2FE0

#define USB_PRODUCT_ID_XAPEA001        0x8BDE
#define USB_PRODUCT_ID_XAPEA002        0x8BDE
#define USB_PRODUCT_ID_XAPEA003        0x8BEE

struct xapea00x_device {
	struct kref kref;

	struct usb_device *udev;
	/*
	 * The interface pointer will be set NULL when the device
	 * disconnects.  Accessing it safe only while holding the
	 * usb_mutex.
	 */
	struct usb_interface *interface;
	/*
	 * Th usb_mutex must be held while synchronous USB requests are
	 * in progress. It is acquired during disconnect to be sure
	 * that there is not an outstanding request.
	 */
	struct mutex usb_mutex;

	struct usb_endpoint_descriptor *bulk_in;
	struct usb_endpoint_descriptor *bulk_out;

	u16 pid;
	u16 vid;

	struct spi_master *spi_master;
	struct spi_device *tpm;
};

/* Public bridge functions */
int xapea00x_br_disable_cs(struct xapea00x_device *dev, u8 channel);
int xapea00x_br_assert_cs(struct xapea00x_device *dev, u8 channel);
int xapea00x_br_deassert_cs(struct xapea00x_device *dev, u8 channel);

int xapea00x_br_spi_read(struct xapea00x_device *dev, void *rx_buf, int len);
int xapea00x_br_spi_write(struct xapea00x_device *dev, const void *tx_buf,
			  int len);
int xapea00x_br_spi_write_read(struct xapea00x_device *dev, const void *tx_buf,
			       void *rx_buf, int len);

/* Shared SPI function */
int xapea00x_spi_transfer(struct xapea00x_device *dev,
			  const void *tx_buf, void *rx_buf, u32 len,
			  int cs_hold, u16 delay_usecs);

/* Shared TPM functions */
int xapea00x_tpm_platform_initialize(struct xapea00x_device *dev);

#endif /* _XAPEA00X_H */
