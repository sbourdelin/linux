/* SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause) */
/*
 * Copyright © 2017-2018 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 *
 * Authors:
 *	Ramalingam C <ramalingam.c@intel.com>
 */

#ifndef _LINUX_MEI_HDCP_H
#define _LINUX_MEI_HDCP_H

#include <linux/mei_cl_bus.h>
#include <drm/drm_hdcp.h>

enum mei_cldev_state {
	MEI_CLDEV_DISABLED,
	MEI_CLDEV_ENABLED
};

/*
 * Enumeration of the physical DDI available on the platform
 */
enum hdcp_physical_port {
	INVALID_PORT = 0x00,	/* Not a valid port */

	DDI_RANGE_BEGIN = 0x01,	/* Beginning of the valid DDI port range */
	DDI_B		= 0x01,		/* Port DDI B */
	DDI_C		= 0x02,		/* Port DDI C */
	DDI_D		= 0x03,		/* Port DDI D */
	DDI_E		= 0x04,		/* Port DDI E */
	DDI_F		= 0x05,		/* Port DDI F */
	DDI_A		= 0x07,		/* Port DDI A */
	DDI_RANGE_END	= DDI_A,/* End of the valid DDI port range */
};

/* The types of HDCP 2.2 ports supported */
enum hdcp_integrated_port_type {
	HDCP_INVALID_TYPE	= 0x00,

	/* HDCP 2.x ports that are integrated into Intel HW */
	INTEGRATED		= 0x01,

	/* HDCP2.2 discrete wired Tx port with LSPCON (HDMI 2.0) solution */
	LSPCON			= 0x02,

	/* HDCP2.2 discrete wired Tx port using the CPDP (DP 1.3) solution */
	CPDP			= 0x03,
};

/*
 * wired_protocol: Supported integrated wired HDCP protocol.
 * Based on this value, Minor difference needed between wired specifications
 * are handled.
 */
enum hdcp_protocol {
	HDCP_PROTOCOL_INVALID,
	HDCP_PROTOCOL_HDMI,
	HDCP_PROTOCOL_DP
};

/*
 * mei_hdcp_data: Input data to the mei_hdcp APIs.
 */
struct mei_hdcp_data {
	enum hdcp_physical_port port;
	enum hdcp_integrated_port_type port_type;
	enum hdcp_protocol protocol;

	/*
	 * No of streams transmitted on a port.
	 * In case of HDMI & DP SST, single stream will be
	 * transmitted on a port.
	 */
	uint16_t k;

	/*
	 * Count of RepeaterAuth_Stream_Manage msg propagated.
	 * Initialized to 0 on AKE_INIT. Incremented after every successful
	 * transmission of RepeaterAuth_Stream_Manage message. When it rolls
	 * over re-Auth has to be triggered.
	 */
	uint32_t seq_num_m;

	/* k(No of Streams per port) x structure of wired_streamid_type */
	struct hdcp2_streamid_type *streams;
};

#if IS_ENABLED(CONFIG_INTEL_MEI_HDCP)
int mei_cldev_register_notify(struct notifier_block *nb);
int mei_cldev_unregister_notify(struct notifier_block *nb);
int mei_cldev_poll_notification(void);
int mei_initiate_hdcp2_session(struct mei_cl_device *cldev,
			       struct mei_hdcp_data *data,
			       struct hdcp2_ake_init *ake_data);
int
mei_verify_receiver_cert_prepare_km(struct mei_cl_device *cldev,
				    struct mei_hdcp_data *data,
				    struct hdcp2_ake_send_cert *rx_cert,
				    bool *km_stored,
				    struct hdcp2_ake_no_stored_km *ek_pub_km,
				    size_t *msg_sz);
#else
static inline int mei_cldev_register_notify(struct notifier_block *nb)
{
	return -ENODEV;
}
static inline int mei_cldev_unregister_notify(struct notifier_block *nb)
{
	return -ENODEV;
}
static inline int mei_cldev_poll_notification(void)
{
	return -ENODEV;
}
static inline
int mei_initiate_hdcp2_session(struct mei_cl_device *cldev,
			       struct mei_hdcp_data *data,
			       struct hdcp2_ake_init *ake_data)
{
	return -ENODEV;
}
static inline int
mei_verify_receiver_cert_prepare_km(struct mei_cl_device *cldev,
				    struct mei_hdcp_data *data,
				    struct hdcp2_ake_send_cert *rx_cert,
				    bool *km_stored,
				    struct hdcp2_ake_no_stored_km *ek_pub_km,
				    size_t *msg_sz)
{
	return -ENODEV;
}
#endif /* IS_ENABLED(CONFIG_INTEL_MEI_HDCP) */
#endif /* defined (_LINUX_MEI_HDCP_H) */
