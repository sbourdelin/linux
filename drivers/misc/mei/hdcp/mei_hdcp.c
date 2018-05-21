/* SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause */
/*
 * Copyright © 2017-2018 Intel Corporation
 *
 * Mei_hdcp.c: HDCP client driver for mei bus
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *	Ramalingam C <ramalingam.c@intel.com>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uuid.h>
#include <linux/mei_cl_bus.h>
#include <linux/notifier.h>
#include <linux/mei_hdcp.h>
#include <drm/drm_connector.h>

#include "mei_hdcp.h"

static struct mei_cl_device *mei_cldev;
static BLOCKING_NOTIFIER_HEAD(mei_cldev_notifier_list);

/*
 * mei_initiate_hdcp2_session:
 *	Function to start a Wired HDCP2.2 Tx Session with ME FW
 *
 * cldev		: Pointer for mei client device
 * data			: Intel HW specific Data
 * ake_data		: ptr to store AKE_Init
 *
 * Returns 0 on Success, <0 on Failure.
 */
int mei_initiate_hdcp2_session(struct mei_cl_device *cldev,
			       struct mei_hdcp_data *data,
			       struct hdcp2_ake_init *ake_data)
{
	struct wired_cmd_initiate_hdcp2_session_in session_init_in = { { 0 } };
	struct wired_cmd_initiate_hdcp2_session_out
						session_init_out = { { 0 } };
	struct device *dev;
	ssize_t byte;

	if (!data || !ake_data)
		return -EINVAL;

	dev = &cldev->dev;

	session_init_in.header.api_version = HDCP_API_VERSION;
	session_init_in.header.command_id = WIRED_INITIATE_HDCP2_SESSION;
	session_init_in.header.status = ME_HDCP_STATUS_SUCCESS;
	session_init_in.header.buffer_len =
				WIRED_CMD_BUF_LEN_INITIATE_HDCP2_SESSION_IN;

	session_init_in.port.integrated_port_type = data->port_type;
	session_init_in.port.physical_port = data->port;
	session_init_in.protocol = (uint8_t)data->protocol;

	byte = mei_cldev_send(cldev, (u8 *)&session_init_in,
			      sizeof(session_init_in));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_send failed. %zd\n", byte);
		return byte;
	}

	byte = mei_cldev_recv(cldev, (u8 *)&session_init_out,
			      sizeof(session_init_out));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_recv failed. %zd\n", byte);
		return byte;
	}

	if (session_init_out.header.status != ME_HDCP_STATUS_SUCCESS) {
		dev_dbg(dev, "ME cmd 0x%08X Failed. Status: 0x%X\n",
			WIRED_INITIATE_HDCP2_SESSION,
			session_init_out.header.status);
		return -EIO;
	}

	ake_data->msg_id = HDCP_2_2_AKE_INIT;
	ake_data->tx_caps = session_init_out.tx_caps;
	memcpy(ake_data->r_tx, session_init_out.r_tx,
	       sizeof(session_init_out.r_tx));

	return 0;
}
EXPORT_SYMBOL(mei_initiate_hdcp2_session);

/*
 * mei_verify_receiver_cert_prepare_km:
 *	Function to verify the Receiver Certificate AKE_Send_Cert
 *	and prepare AKE_Stored_Km or AKE_No_Stored_Km
 *
 * cldev		: Pointer for mei client device
 * data			: Intel HW specific Data
 * rx_cert		: Pointer for AKE_Send_Cert
 * km_stored		: Pointer for pairing status flag
 * ek_pub_km		: Pointer for output msg
 * msg_sz		: Pointer for size of AKE_XXXXX_Km
 *
 * Returns 0 on Success, <0 on Failure
 */
int
mei_verify_receiver_cert_prepare_km(struct mei_cl_device *cldev,
				    struct mei_hdcp_data *data,
				    struct hdcp2_ake_send_cert *rx_cert,
				    bool *km_stored,
				    struct hdcp2_ake_no_stored_km *ek_pub_km,
				    size_t *msg_sz)
{
	struct wired_cmd_verify_receiver_cert_in verify_rxcert_in = { { 0 } };
	struct wired_cmd_verify_receiver_cert_out verify_rxcert_out = { { 0 } };
	struct device *dev;
	ssize_t byte;

	if (!data || !rx_cert || !km_stored || !ek_pub_km || !msg_sz)
		return -EINVAL;

	dev = &cldev->dev;

	verify_rxcert_in.header.api_version = HDCP_API_VERSION;
	verify_rxcert_in.header.command_id = WIRED_VERIFY_RECEIVER_CERT;
	verify_rxcert_in.header.status = ME_HDCP_STATUS_SUCCESS;
	verify_rxcert_in.header.buffer_len =
				WIRED_CMD_BUF_LEN_VERIFY_RECEIVER_CERT_IN;

	verify_rxcert_in.port.integrated_port_type = data->port_type;
	verify_rxcert_in.port.physical_port = data->port;

	memcpy(&verify_rxcert_in.cert_rx, &rx_cert->cert_rx,
	       sizeof(rx_cert->cert_rx));
	memcpy(verify_rxcert_in.r_rx, &rx_cert->r_rx, sizeof(rx_cert->r_rx));
	memcpy(verify_rxcert_in.rx_caps, rx_cert->rx_caps, HDCP_2_2_RXCAPS_LEN);

	byte = mei_cldev_send(cldev, (u8 *)&verify_rxcert_in,
			      sizeof(verify_rxcert_in));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_send failed: %zd\n", byte);
		return byte;
	}

	byte = mei_cldev_recv(cldev, (u8 *)&verify_rxcert_out,
			      sizeof(verify_rxcert_out));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_recv failed: %zd\n", byte);
		return byte;
	}

	if (verify_rxcert_out.header.status != ME_HDCP_STATUS_SUCCESS) {
		dev_dbg(dev, "ME cmd 0x%08X Failed. Status: 0x%X\n",
			WIRED_VERIFY_RECEIVER_CERT,
			verify_rxcert_out.header.status);
		return -EIO;
	}

	*km_stored = verify_rxcert_out.km_stored;
	if (verify_rxcert_out.km_stored) {
		ek_pub_km->msg_id = HDCP_2_2_AKE_STORED_KM;
		*msg_sz = sizeof(struct hdcp2_ake_stored_km);
	} else {
		ek_pub_km->msg_id = HDCP_2_2_AKE_NO_STORED_KM;
		*msg_sz = sizeof(struct hdcp2_ake_no_stored_km);
	}

	memcpy(ek_pub_km->e_kpub_km, &verify_rxcert_out.ekm_buff,
	       sizeof(verify_rxcert_out.ekm_buff));

	return 0;
}
EXPORT_SYMBOL(mei_verify_receiver_cert_prepare_km);

/*
 * mei_verify_hprime:
 *	Function to verify AKE_Send_H_prime received, through ME FW.
 *
 * cldev		: Pointer for mei client device
 * data			: Intel HW specific Data
 * rx_hprime		: Pointer for AKE_Send_H_prime
 * hprime_sz		: Input buffer size
 *
 * Returns 0 on Success, <0 on Failure
 */
int mei_verify_hprime(struct mei_cl_device *cldev, struct mei_hdcp_data *data,
		      struct hdcp2_ake_send_hprime *rx_hprime)
{
	struct wired_cmd_ake_send_hprime_in send_hprime_in = { { 0 } };
	struct wired_cmd_ake_send_hprime_out send_hprime_out = { { 0 } };
	struct device *dev;
	ssize_t byte;

	if (!data || !rx_hprime)
		return -EINVAL;

	dev = &cldev->dev;

	send_hprime_in.header.api_version = HDCP_API_VERSION;
	send_hprime_in.header.command_id = WIRED_AKE_SEND_HPRIME;
	send_hprime_in.header.status = ME_HDCP_STATUS_SUCCESS;
	send_hprime_in.header.buffer_len = WIRED_CMD_BUF_LEN_AKE_SEND_HPRIME_IN;

	send_hprime_in.port.integrated_port_type = data->port_type;
	send_hprime_in.port.physical_port = data->port;

	memcpy(send_hprime_in.h_prime, rx_hprime->h_prime,
	       sizeof(rx_hprime->h_prime));

	byte = mei_cldev_send(cldev, (u8 *)&send_hprime_in,
			      sizeof(send_hprime_in));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_send failed. %zd\n", byte);
		return byte;
	}

	byte = mei_cldev_recv(cldev, (u8 *)&send_hprime_out,
			      sizeof(send_hprime_out));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_recv failed. %zd\n", byte);
		return byte;
	}

	if (send_hprime_out.header.status != ME_HDCP_STATUS_SUCCESS) {
		dev_dbg(dev, "ME cmd 0x%08X Failed. Status: 0x%X\n",
			WIRED_AKE_SEND_HPRIME, send_hprime_out.header.status);
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(mei_verify_hprime);

/*
 * mei_store_pairing_info:
 *	Function to store pairing info received from panel
 *
 * cldev		: Pointer for mei client device
 * data			: Intel HW specific Data
 * pairing_info		: Pointer for AKE_Send_Pairing_Info
 *
 * Returns 0 on Success, <0 on Failure
 */
int mei_store_pairing_info(struct mei_cl_device *cldev,
			   struct mei_hdcp_data *data,
			   struct hdcp2_ake_send_pairing_info *pairing_info)
{
	struct wired_cmd_ake_send_pairing_info_in pairing_info_in = { { 0 } };
	struct wired_cmd_ake_send_pairing_info_out pairing_info_out = { { 0 } };
	struct device *dev;
	ssize_t byte;

	if (!data || !pairing_info)
		return -EINVAL;

	dev = &cldev->dev;

	pairing_info_in.header.api_version = HDCP_API_VERSION;
	pairing_info_in.header.command_id = WIRED_AKE_SEND_PAIRING_INFO;
	pairing_info_in.header.status = ME_HDCP_STATUS_SUCCESS;
	pairing_info_in.header.buffer_len =
					WIRED_CMD_BUF_LEN_SEND_PAIRING_INFO_IN;

	pairing_info_in.port.integrated_port_type = data->port_type;
	pairing_info_in.port.physical_port = data->port;

	memcpy(pairing_info_in.e_kh_km, pairing_info->e_kh_km,
	       sizeof(pairing_info_in.e_kh_km));

	byte = mei_cldev_send(cldev, (u8 *)&pairing_info_in,
			      sizeof(pairing_info_in));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_send failed. %zd\n", byte);
		return byte;
	}

	byte = mei_cldev_recv(cldev, (u8 *)&pairing_info_out,
			      sizeof(pairing_info_out));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_recv failed. %zd\n", byte);
		return byte;
	}

	if (pairing_info_out.header.status != ME_HDCP_STATUS_SUCCESS) {
		dev_dbg(dev, "ME cmd 0x%08X failed. Status: 0x%X\n",
			WIRED_AKE_SEND_PAIRING_INFO,
			pairing_info_out.header.status);
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(mei_store_pairing_info);

/*
 * mei_initiate_locality_check:
 *	Function to prepare LC_Init.
 *
 * cldev		: Pointer for mei client device
 * data			: Intel HW specific Data
 * hdcp2_lc_init	: Pointer for storing LC_Init
 *
 * Returns 0 on Success, <0 on Failure
 */
int mei_initiate_locality_check(struct mei_cl_device *cldev,
				struct mei_hdcp_data *data,
				struct hdcp2_lc_init *lc_init_data)
{
	struct wired_cmd_init_locality_check_in lc_init_in = { { 0 } };
	struct wired_cmd_init_locality_check_out lc_init_out = { { 0 } };
	struct device *dev;
	ssize_t byte;

	if (!data || !lc_init_data)
		return -EINVAL;

	dev = &cldev->dev;

	lc_init_in.header.api_version = HDCP_API_VERSION;
	lc_init_in.header.command_id = WIRED_INIT_LOCALITY_CHECK;
	lc_init_in.header.status = ME_HDCP_STATUS_SUCCESS;
	lc_init_in.header.buffer_len = WIRED_CMD_BUF_LEN_INIT_LOCALITY_CHECK_IN;

	lc_init_in.port.integrated_port_type = data->port_type;
	lc_init_in.port.physical_port = data->port;

	byte = mei_cldev_send(cldev, (u8 *)&lc_init_in, sizeof(lc_init_in));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_send failed. %zd\n", byte);
		return byte;
	}

	byte = mei_cldev_recv(cldev, (u8 *)&lc_init_out, sizeof(lc_init_out));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_recv failed. %zd\n", byte);
		return byte;
	}

	if (lc_init_out.header.status != ME_HDCP_STATUS_SUCCESS) {
		dev_dbg(dev, "ME cmd 0x%08X Failed. status: 0x%X\n",
			WIRED_INIT_LOCALITY_CHECK, lc_init_out.header.status);
		return -EIO;
	}

	lc_init_data->msg_id = HDCP_2_2_LC_INIT;
	memcpy(lc_init_data->r_n, lc_init_out.r_n, HDCP_2_2_RN_LEN);

	return 0;
}
EXPORT_SYMBOL(mei_initiate_locality_check);

/*
 * mei_verify_lprime:
 *	Function to verify lprime.
 *
 * cldev		: Pointer for mei client device
 * data			: Intel HW specific Data
 * rx_lprime		: Pointer for LC_Send_L_prime
 *
 * Returns 0 on Success, <0 on Failure
 */
int mei_verify_lprime(struct mei_cl_device *cldev, struct mei_hdcp_data *data,
		      struct hdcp2_lc_send_lprime *rx_lprime)
{
	struct wired_cmd_validate_locality_in verify_lprime_in = { { 0 } };
	struct wired_cmd_validate_locality_out verify_lprime_out = { { 0 } };
	struct device *dev;
	ssize_t byte;

	if (!data || !rx_lprime)
		return -EINVAL;

	dev = &cldev->dev;

	verify_lprime_in.header.api_version = HDCP_API_VERSION;
	verify_lprime_in.header.command_id = WIRED_VALIDATE_LOCALITY;
	verify_lprime_in.header.status = ME_HDCP_STATUS_SUCCESS;
	verify_lprime_in.header.buffer_len =
					WIRED_CMD_BUF_LEN_VALIDATE_LOCALITY_IN;

	verify_lprime_in.port.integrated_port_type = data->port_type;
	verify_lprime_in.port.physical_port = data->port;

	memcpy(verify_lprime_in.l_prime, rx_lprime->l_prime,
	       sizeof(rx_lprime->l_prime));

	byte = mei_cldev_send(cldev, (u8 *)&verify_lprime_in,
			      sizeof(verify_lprime_in));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_send failed. %zd\n", byte);
		return byte;
	}

	byte = mei_cldev_recv(cldev, (u8 *)&verify_lprime_out,
			      sizeof(verify_lprime_out));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_recv failed. %zd\n", byte);
		return byte;
	}

	if (verify_lprime_out.header.status != ME_HDCP_STATUS_SUCCESS) {
		dev_dbg(dev, "ME cmd 0x%08X failed. status: 0x%X\n",
			WIRED_VALIDATE_LOCALITY,
			verify_lprime_out.header.status);
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(mei_verify_lprime);

/*
 * mei_get_session_key:
 *	Function to prepare SKE_Send_Eks.
 *
 * cldev		: Pointer for mei client device
 * data			: Intel HW specific Data
 * ske_data		: Pointer for SKE_Send_Eks.
 *
 * Returns 0 on Success, <0 on Failure
 */
int mei_get_session_key(struct mei_cl_device *cldev, struct mei_hdcp_data *data,
			struct hdcp2_ske_send_eks *ske_data)
{
	struct wired_cmd_get_session_key_in get_skey_in = { { 0 } };
	struct wired_cmd_get_session_key_out get_skey_out = { { 0 } };
	struct device *dev;
	ssize_t byte;

	if (!data || !ske_data)
		return -EINVAL;

	dev = &cldev->dev;

	get_skey_in.header.api_version = HDCP_API_VERSION;
	get_skey_in.header.command_id = WIRED_GET_SESSION_KEY;
	get_skey_in.header.status = ME_HDCP_STATUS_SUCCESS;
	get_skey_in.header.buffer_len = WIRED_CMD_BUF_LEN_GET_SESSION_KEY_IN;

	get_skey_in.port.integrated_port_type = data->port_type;
	get_skey_in.port.physical_port = data->port;

	byte = mei_cldev_send(cldev, (u8 *)&get_skey_in, sizeof(get_skey_in));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_send failed. %zd\n", byte);
		return byte;
	}

	byte = mei_cldev_recv(cldev, (u8 *)&get_skey_out, sizeof(get_skey_out));

	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_recv failed. %zd\n", byte);
		return byte;
	}

	if (get_skey_out.header.status != ME_HDCP_STATUS_SUCCESS) {
		dev_dbg(dev, "ME cmd 0x%08X failed. status: 0x%X\n",
			WIRED_GET_SESSION_KEY, get_skey_out.header.status);
		return -EIO;
	}

	ske_data->msg_id = HDCP_2_2_SKE_SEND_EKS;
	memcpy(ske_data->e_dkey_ks, get_skey_out.e_dkey_ks,
	       HDCP_2_2_E_DKEY_KS_LEN);
	memcpy(ske_data->riv, get_skey_out.r_iv, HDCP_2_2_RIV_LEN);

	return 0;
}
EXPORT_SYMBOL(mei_get_session_key);

/*
 * mei_repeater_check_flow_prepare_ack:
 *	Function to validate the Downstream topology and prepare rep_ack.
 *
 * cldev		: Pointer for mei client device
 * data			: Intel HW specific Data
 * rep_topology		: Pointer for Receiver Id List to be validated.
 * rep_send_ack		: Pointer for repeater ack
 *
 * Returns 0 on Success, <0 on Failure
 */
int
mei_repeater_check_flow_prepare_ack(struct mei_cl_device *cldev,
				    struct mei_hdcp_data *data,
				    struct hdcp2_rep_send_receiverid_list
							*rep_topology,
				    struct hdcp2_rep_send_ack *rep_send_ack)
{
	struct wired_cmd_verify_repeater_in verify_repeater_in = { { 0 } };
	struct wired_cmd_verify_repeater_out verify_repeater_out = { { 0 } };
	struct device *dev;
	ssize_t byte;

	if (!rep_topology || !rep_send_ack || !data)
		return -EINVAL;

	dev = &cldev->dev;

	verify_repeater_in.header.api_version = HDCP_API_VERSION;
	verify_repeater_in.header.command_id = WIRED_VERIFY_REPEATER;
	verify_repeater_in.header.status = ME_HDCP_STATUS_SUCCESS;
	verify_repeater_in.header.buffer_len =
					WIRED_CMD_BUF_LEN_VERIFY_REPEATER_IN;

	verify_repeater_in.port.integrated_port_type = data->port_type;
	verify_repeater_in.port.physical_port = data->port;

	memcpy(verify_repeater_in.rx_info, rep_topology->rx_info,
	       HDCP_2_2_RXINFO_LEN);
	memcpy(verify_repeater_in.seq_num_v, rep_topology->seq_num_v,
	       HDCP_2_2_SEQ_NUM_LEN);
	memcpy(verify_repeater_in.v_prime, rep_topology->v_prime,
	       HDCP_2_2_LPRIME_HALF_LEN);
	memcpy(verify_repeater_in.receiver_ids, rep_topology->receiver_ids,
	       HDCP_2_2_RECEIVER_IDS_MAX_LEN);

	byte = mei_cldev_send(cldev, (u8 *)&verify_repeater_in,
			      sizeof(verify_repeater_in));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_send failed. %zd\n", byte);
		return byte;
	}

	byte = mei_cldev_recv(cldev, (u8 *)&verify_repeater_out,
			      sizeof(verify_repeater_out));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_recv failed. %zd\n", byte);
		return byte;
	}

	if (verify_repeater_out.header.status != ME_HDCP_STATUS_SUCCESS) {
		dev_dbg(dev, "ME cmd 0x%08X failed. status: 0x%X\n",
			WIRED_VERIFY_REPEATER,
			verify_repeater_out.header.status);
		return -EIO;
	}

	memcpy(rep_send_ack->v, verify_repeater_out.v,
	       HDCP_2_2_LPRIME_HALF_LEN);
	rep_send_ack->msg_id = HDCP_2_2_REP_SEND_ACK;

	return 0;
}
EXPORT_SYMBOL(mei_repeater_check_flow_prepare_ack);

/*
 * mei_verify_mprime:
 *	Function to verify mprime.
 *
 * cldev		: Pointer for mei client device
 * data			: Intel HW specific Data
 * stream_ready		: pointer for RepeaterAuth_Stream_Ready message.
 *
 * Returns 0 on Success, <0 on Failure
 */
int mei_verify_mprime(struct mei_cl_device *cldev, struct mei_hdcp_data *data,
		      struct hdcp2_rep_stream_ready *stream_ready)
{
	struct wired_cmd_repeater_auth_stream_req_in
					verify_mprime_in = { { 0 } };
	struct wired_cmd_repeater_auth_stream_req_out
					verify_mprime_out = { { 0 } };
	struct device *dev;
	ssize_t byte;

	if (!stream_ready || !data)
		return -EINVAL;

	dev = &cldev->dev;

	verify_mprime_in.header.api_version = HDCP_API_VERSION;
	verify_mprime_in.header.command_id = WIRED_REPEATER_AUTH_STREAM_REQ;
	verify_mprime_in.header.status = ME_HDCP_STATUS_SUCCESS;
	verify_mprime_in.header.buffer_len =
			WIRED_CMD_BUF_LEN_REPEATER_AUTH_STREAM_REQ_MIN_IN;

	verify_mprime_in.port.integrated_port_type = data->port_type;
	verify_mprime_in.port.physical_port = data->port;

	memcpy(verify_mprime_in.m_prime, stream_ready->m_prime,
	       HDCP_2_2_MPRIME_LEN);
	reverse_endianness((u8 *)&verify_mprime_in.seq_num_m,
			   HDCP_2_2_SEQ_NUM_LEN, (u8 *)&data->seq_num_m);
	memcpy(verify_mprime_in.streams, data->streams,
	       (data->k * sizeof(struct hdcp2_streamid_type)));

	verify_mprime_in.k = __swab16(data->k);

	byte = mei_cldev_send(cldev, (u8 *)&verify_mprime_in,
			      sizeof(verify_mprime_in));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_send failed. %zd\n", byte);
		return byte;
	}

	byte = mei_cldev_recv(cldev, (u8 *)&verify_mprime_out,
			      sizeof(verify_mprime_out));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_recv failed. %zd\n", byte);
		return byte;
	}

	if (verify_mprime_out.header.status != ME_HDCP_STATUS_SUCCESS) {
		dev_dbg(dev, "ME cmd 0x%08X failed. status: 0x%X\n",
			WIRED_REPEATER_AUTH_STREAM_REQ,
			verify_mprime_out.header.status);
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(mei_verify_mprime);

/*
 * mei_enable_hdcp_authentication:
 *	Function to request ME FW to mark a port as authenticated.
 *
 * cldev		: Pointer for mei client device
 * data		: Intel HW specific Data
 *
 * Returns 0 on Success, <0 on Failure
 */
int mei_enable_hdcp_authentication(struct mei_cl_device *cldev,
				   struct mei_hdcp_data *data)
{
	struct wired_cmd_enable_auth_in enable_auth_in = { { 0 } };
	struct wired_cmd_enable_auth_out enable_auth_out = { { 0 } };
	struct device *dev;
	ssize_t byte;

	if (!data)
		return -EINVAL;

	dev = &cldev->dev;

	enable_auth_in.header.api_version = HDCP_API_VERSION;
	enable_auth_in.header.command_id = WIRED_ENABLE_AUTH;
	enable_auth_in.header.status = ME_HDCP_STATUS_SUCCESS;
	enable_auth_in.header.buffer_len = WIRED_CMD_BUF_LEN_ENABLE_AUTH_IN;

	enable_auth_in.port.integrated_port_type = data->port_type;
	enable_auth_in.port.physical_port = data->port;
	enable_auth_in.stream_type = data->streams[0].stream_type;

	byte = mei_cldev_send(cldev, (u8 *)&enable_auth_in,
			      sizeof(enable_auth_in));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_send failed. %zd\n", byte);
		return byte;
	}

	byte = mei_cldev_recv(cldev, (u8 *)&enable_auth_out,
			      sizeof(enable_auth_out));
	if (byte < 0) {
		dev_dbg(dev, "mei_cldev_recv failed. %zd\n", byte);
		return byte;
	}

	if (enable_auth_out.header.status != ME_HDCP_STATUS_SUCCESS) {
		dev_dbg(dev, "ME cmd 0x%08X failed. status: 0x%X\n",
			WIRED_ENABLE_AUTH, enable_auth_out.header.status);
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(mei_enable_hdcp_authentication);

static void
mei_cldev_state_notify_clients(struct mei_cl_device *cldev, bool enabled)
{
	if (enabled)
		blocking_notifier_call_chain(&mei_cldev_notifier_list,
					     MEI_CLDEV_ENABLED, cldev);
	else
		blocking_notifier_call_chain(&mei_cldev_notifier_list,
					     MEI_CLDEV_DISABLED, NULL);
}

int mei_cldev_register_notify(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&mei_cldev_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(mei_cldev_register_notify);

int mei_cldev_unregister_notify(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&mei_cldev_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(mei_cldev_unregister_notify);

int mei_cldev_poll_notification(void)
{
	if (mei_cldev)
		mei_cldev_state_notify_clients(mei_cldev, true);
	return 0;
}
EXPORT_SYMBOL_GPL(mei_cldev_poll_notification);

static int mei_hdcp_probe(struct mei_cl_device *cldev,
			  const struct mei_cl_device_id *id)
{
	int ret;

	ret = mei_cldev_enable(cldev);
	if (ret < 0) {
		dev_err(&cldev->dev, "mei_cldev_enable Failed. %d\n", ret);
		return ret;
	}

	mei_cldev = cldev;
	mei_cldev_state_notify_clients(cldev, true);
	return 0;
}

static int mei_hdcp_remove(struct mei_cl_device *cldev)
{
	mei_cldev = NULL;
	mei_cldev_state_notify_clients(cldev, false);
	mei_cldev_set_drvdata(cldev, NULL);
	return mei_cldev_disable(cldev);
}

#define MEI_UUID_HDCP		UUID_LE(0xB638AB7E, 0x94E2, 0x4EA2, 0xA5, \
					0x52, 0xD1, 0xC5, 0x4B, \
					0x62, 0x7F, 0x04)

static struct mei_cl_device_id mei_hdcp_tbl[] = {
	{ .uuid = MEI_UUID_HDCP, .version = MEI_CL_VERSION_ANY },
	{ }
};
MODULE_DEVICE_TABLE(mei, mei_hdcp_tbl);

static struct mei_cl_driver mei_hdcp_driver = {
	.id_table	= mei_hdcp_tbl,
	.name		= KBUILD_MODNAME,
	.probe		= mei_hdcp_probe,
	.remove		= mei_hdcp_remove,
};

module_mei_cl_driver(mei_hdcp_driver);

MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("MEI HDCP");
