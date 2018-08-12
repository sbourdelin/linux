// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018 MediaTek Inc.

/*
 * Common operations for MediaTek Bluetooth devices
 * with the UART, USB and SDIO transport
 *
 * Author: Sean Wang <sean.wang at mediatek.com>
 *
 */
#include <asm/unaligned.h>
#include <linux/firmware.h>
#include <linux/iopoll.h>
#include <linux/module.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "btmtk.h"

#define VERSION "0.1"

int
btmtk_hci_wmt_sync(struct hci_dev *hdev, struct btmtk_hci_wmt_params *params)
{
	struct btmtk_hci_wmt_evt_funcc *evt_funcc;
	u32 hlen, status = BTMTK_WMT_INVALID;
	struct btmtk_wmt_hdr *hdr, *ehdr;
	struct btmtk_hci_wmt_cmd wc;
	struct sk_buff *skb;
	int err = 0;

	hlen = sizeof(*hdr) + params->dlen;
	if (hlen > 255)
		return -EINVAL;

	hdr = (struct btmtk_wmt_hdr *)&wc;
	hdr->dir = 1;
	hdr->op = params->op;
	hdr->dlen = cpu_to_le16(params->dlen + 1);
	hdr->flag = params->flag;
	memcpy(wc.data, params->data, params->dlen);

	/* TODO: Add a fixup with __hci_raw_sync_ev that uses the hdev->raw_q
	 * instead of the hack with __hci_cmd_sync_ev + atomic_inc on cmd_cnt.
	 */
	atomic_inc(&hdev->cmd_cnt);

	skb =  __hci_cmd_sync_ev(hdev, 0xfc6f, hlen, &wc, HCI_VENDOR_PKT,
				 HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		err = PTR_ERR(skb);

		bt_dev_err(hdev, "Failed to send wmt cmd (%d)\n", err);

		print_hex_dump(KERN_ERR, "failed cmd: ",
			       DUMP_PREFIX_ADDRESS, 16, 1, &wc,
			       hlen > 16 ? 16 : hlen, true);
		return err;
	}

	ehdr = (struct btmtk_wmt_hdr *)skb->data;
	if (ehdr->op != hdr->op) {
		bt_dev_err(hdev, "Wrong op received %d expected %d",
			   ehdr->op, hdr->op);
		err = -EIO;
		goto err_free_skb;
	}

	switch (ehdr->op) {
	case BTMTK_WMT_SEMAPHORE:
		if (ehdr->flag == 2)
			status = BTMTK_WMT_PATCH_UNDONE;
		else
			status = BTMTK_WMT_PATCH_DONE;
		break;
	case BTMTK_WMT_FUNC_CTRL:
		evt_funcc = (struct btmtk_hci_wmt_evt_funcc *)ehdr;
		if (be16_to_cpu(evt_funcc->status) == 4)
			status = BTMTK_WMT_ON_DONE;
		else if (be16_to_cpu(evt_funcc->status) == 32)
			status = BTMTK_WMT_ON_PROGRESS;
		else
			status = BTMTK_WMT_ON_UNDONE;
		break;
	};

	if (params->status)
		*params->status = status;

err_free_skb:
	kfree_skb(skb);

	return err;
}
EXPORT_SYMBOL_GPL(btmtk_hci_wmt_sync);

static int
btmtk_setup_firmware(struct hci_dev *hdev, const char *fwname,
		     int (*cmd_sync)(struct hci_dev *,
				     struct btmtk_hci_wmt_params *))
{
	struct btmtk_hci_wmt_params wmt_params;
	const struct firmware *fw;
	const u8 *fw_ptr;
	size_t fw_size;
	int err, dlen;
	u8 flag;

	if (!cmd_sync)
		return -EINVAL;

	err = request_firmware(&fw, fwname, &hdev->dev);
	if (err < 0) {
		bt_dev_err(hdev, "Failed to load firmware file (%d)", err);
		return err;
	}

	fw_ptr = fw->data;
	fw_size = fw->size;

	/* The size of patch header is 30 bytes, should be skip */
	if (fw_size < 30)
		return -EINVAL;

	fw_size -= 30;
	fw_ptr += 30;
	flag = 1;

	wmt_params.op = BTMTK_WMT_PATCH_DWNLD;
	wmt_params.status = NULL;

	while (fw_size > 0) {
		dlen = min_t(int, 250, fw_size);

		/* Tell deivice the position in sequence */
		if (fw_size - dlen <= 0)
			flag = 3;
		else if (fw_size < fw->size - 30)
			flag = 2;

		wmt_params.flag = flag;
		wmt_params.dlen = dlen;
		wmt_params.data = fw_ptr;

		err = cmd_sync(hdev, &wmt_params);
		if (err < 0) {
			bt_dev_err(hdev, "Failed to send wmt patch dwnld (%d)",
				   err);
			goto err_release_fw;
		}

		fw_size -= dlen;
		fw_ptr += dlen;
	}

	wmt_params.op = BTMTK_WMT_RST;
	wmt_params.flag = 4;
	wmt_params.dlen = 0;
	wmt_params.data = NULL;
	wmt_params.status = NULL;

	/* Activate funciton the firmware providing to */
	err = cmd_sync(hdev, &wmt_params);
	if (err < 0) {
		bt_dev_err(hdev, "Failed to send wmt rst (%d)", err);
		return err;
	}

err_release_fw:
	release_firmware(fw);

	return err;
}

static int
btmtk_func_query(struct btmtk_func_query *fq)
{
	struct btmtk_hci_wmt_params wmt_params;
	int status, err;
	u8 param = 0;

	if (!fq || !fq->hdev || !fq->cmd_sync)
		return -EINVAL;

	/* Query whether the function is enabled */
	wmt_params.op = BTMTK_WMT_FUNC_CTRL;
	wmt_params.flag = 4;
	wmt_params.dlen = sizeof(param);
	wmt_params.data = &param;
	wmt_params.status = &status;

	err = fq->cmd_sync(fq->hdev, &wmt_params);
	if (err < 0) {
		bt_dev_err(fq->hdev, "Failed to query function status (%d)",
			   err);
		return err;
	}

	return status;
}

int btmtk_enable(struct hci_dev *hdev, const char *fwname,
		 int (*cmd_sync)(struct hci_dev *hdev,
				 struct btmtk_hci_wmt_params *))
{
	struct btmtk_hci_wmt_params wmt_params;
	struct btmtk_func_query func_query;
	int status, err;
	u8 param;

	if (!cmd_sync)
		return -EINVAL;

	/* Query whether the firmware is already download */
	wmt_params.op = BTMTK_WMT_SEMAPHORE;
	wmt_params.flag = 1;
	wmt_params.dlen = 0;
	wmt_params.data = NULL;
	wmt_params.status = &status;

	err = cmd_sync(hdev, &wmt_params);
	if (err < 0) {
		bt_dev_err(hdev, "Failed to query firmware status (%d)", err);
		return err;
	}

	if (status == BTMTK_WMT_PATCH_DONE) {
		bt_dev_info(hdev, "firmware already downloaded");
		goto ignore_setup_fw;
	}

	/* Setup a firmware which the device definitely requires */
	err = btmtk_setup_firmware(hdev, fwname, cmd_sync);
	if (err < 0)
		return err;

ignore_setup_fw:
	func_query.hdev = hdev;
	func_query.cmd_sync = cmd_sync;
	err = readx_poll_timeout(btmtk_func_query, &func_query, status,
				 status < 0 || status != BTMTK_WMT_ON_PROGRESS,
				 2000, 5000000);
	/* -ETIMEDOUT happens */
	if (err < 0)
		return err;

	/* The other errors happen internally inside btmtk_func_query */
	if (status < 0)
		return status;

	if (status == BTMTK_WMT_ON_DONE) {
		bt_dev_info(hdev, "function already on");
		goto ignore_func_on;
	}

	/* Enable Bluetooth protocol */
	param = 1;
	wmt_params.op = BTMTK_WMT_FUNC_CTRL;
	wmt_params.flag = 0;
	wmt_params.dlen = sizeof(param);
	wmt_params.data = &param;
	wmt_params.status = NULL;

	err = cmd_sync(hdev, &wmt_params);
	if (err < 0) {
		bt_dev_err(hdev, "Failed to send wmt func ctrl (%d)", err);
		return err;
	}

ignore_func_on:
	return 0;
}
EXPORT_SYMBOL_GPL(btmtk_enable);

int btmtk_disable(struct hci_dev *hdev,
		  int (*cmd_sync)(struct hci_dev *hdev,
				  struct btmtk_hci_wmt_params *))
{
	struct btmtk_hci_wmt_params wmt_params;
	u8 param = 0;
	int err;

	if (!cmd_sync)
		return -EINVAL;

	/* Disable the device */
	wmt_params.op = BTMTK_WMT_FUNC_CTRL;
	wmt_params.flag = 0;
	wmt_params.dlen = sizeof(param);
	wmt_params.data = &param;
	wmt_params.status = NULL;

	err = cmd_sync(hdev, &wmt_params);
	if (err < 0) {
		bt_dev_err(hdev, "Failed to send wmt func ctrl (%d)", err);
		return err;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(btmtk_disable);

MODULE_AUTHOR("Sean Wang <sean.wang@mediatek.com>");
MODULE_DESCRIPTION("MediaTek Bluetooth device driver ver " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(FIRMWARE_MT7663);
MODULE_FIRMWARE(FIRMWARE_MT7668);
