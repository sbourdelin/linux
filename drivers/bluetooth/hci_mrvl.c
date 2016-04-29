/* Bluetooth HCI UART driver for Marvell devices
 *
 * Copyright (C) 2016, Marvell International Ltd.
 *
 *  Acknowledgements:
 *  This file is based on hci_h4.c, which was written
 *  by Maxim Krasnyansky and Marcel Holtmann.
 *
 * This software file (the "File") is distributed by Marvell International
 * Ltd. under the terms of the GNU General Public License Version 2, June 1991
 * (the "License").  You may use, redistribute and/or modify this File in
 * accordance with the terms and conditions of the License, a copy of which
 * is available on the worldwide web at
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt.
 *
 * THE FILE IS DISTRIBUTED AS-IS, WITHOUT WARRANTY OF ANY KIND, AND THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE
 * ARE EXPRESSLY DISCLAIMED.  The License provides additional details about
 * this warranty disclaimer.
 */

#include <linux/firmware.h>
#include <linux/tty.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#include "hci_uart.h"

struct fw_data {
	wait_queue_head_t init_wait_q;
	u8 wait_fw;
	int next_len;
	u8 five_bytes[5];
	u8 next_index;
	u8 last_ack;
	u8 expected_ack;
	struct ktermios old_termios;
	u8 chip_id;
	u8 chip_rev;
	struct sk_buff *skb;
};

#define HCI_UART_DNLD_FW	0
#define MRVL_HELPER_NAME	"mrvl/helper_uart_3000000.bin"
#define MRVL_8997_CHIP_ID	0x50
#define MRVL_8997_FW_NAME	"mrvl/uart8997_bt.bin"
#define MRVL_MAX_FW_BLOCK_SIZE	1024
#define MRVL_MAX_RETRY_SEND	12
#define MRVL_DNLD_DELAY		100
#define MRVL_ACK		0x5A
#define MRVL_NAK		0xBF
#define MRVL_HDR_REQ_FW		0xA5
#define MRVL_HDR_CHIP_VER	0xAA
#define MRVL_HCI_OP_SET_BAUD	0xFC09
#define MRVL_FW_HDR_LEN		5
#define MRVL_WAIT_TIMEOUT	msecs_to_jiffies(12000)

struct mrvl_data {
	struct sk_buff *rx_skb;
	struct sk_buff_head txq;
	struct fw_data *fwdata;
	unsigned long flags;
};

static int get_cts(struct hci_uart *hu)
{
	struct tty_struct *tty = hu->tty;
	u32 state =  tty->ops->tiocmget(tty);

	if (state & TIOCM_CTS) {
		bt_dev_dbg(hu->hdev, "CTS is low");
		return 1;
	}
	bt_dev_dbg(hu->hdev, "CTS is high");

	return 0;
}

/* Initialize protocol */
static int mrvl_open(struct hci_uart *hu)
{
	struct mrvl_data *mrvl;

	bt_dev_dbg(hu->hdev, "hu %p", hu);

	mrvl = kzalloc(sizeof(*mrvl), GFP_KERNEL);
	if (!mrvl)
		return -ENOMEM;

	skb_queue_head_init(&mrvl->txq);
	hu->priv = mrvl;

	return 0;
}

/* Flush protocol data */
static int mrvl_flush(struct hci_uart *hu)
{
	struct mrvl_data *mrvl = hu->priv;

	bt_dev_dbg(hu->hdev, "hu %p", hu);

	skb_queue_purge(&mrvl->txq);

	return 0;
}

/* Close protocol */
static int mrvl_close(struct hci_uart *hu)
{
	struct mrvl_data *mrvl = hu->priv;

	bt_dev_dbg(hu->hdev, "hu %p", hu);

	skb_queue_purge(&mrvl->txq);
	kfree_skb(mrvl->rx_skb);
	kfree(mrvl->fwdata);
	hu->priv = NULL;
	kfree(mrvl);

	return 0;
}

/* Enqueue frame for transmittion (padding, crc, etc) */
static int mrvl_enqueue(struct hci_uart *hu, struct sk_buff *skb)
{
	struct mrvl_data *mrvl = hu->priv;

	if (test_bit(HCI_UART_DNLD_FW, &mrvl->flags))
		return -EBUSY;

	bt_dev_dbg(hu->hdev, "hu %p skb %p", hu, skb);

	/* Prepend skb with frame type */
	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);
	skb_queue_tail(&mrvl->txq, skb);

	return 0;
}

static const struct h4_recv_pkt mrvl_recv_pkts[] = {
	{ H4_RECV_ACL,   .recv = hci_recv_frame },
	{ H4_RECV_SCO,   .recv = hci_recv_frame },
	{ H4_RECV_EVENT, .recv = hci_recv_frame },
};

/* Send ACK/NAK to the device */
static void mrvl_send_ack(struct hci_uart *hu, unsigned char ack)
{
	struct tty_struct *tty = hu->tty;

	tty->ops->write(tty, &ack, sizeof(ack));
}

/* Validate the feedback data from device */
static void mrvl_pkt_complete(struct hci_uart *hu, struct sk_buff *skb)
{
	struct mrvl_data *mrvl = hu->priv;
	struct fw_data *fw_data = mrvl->fwdata;
	u8 buf[MRVL_FW_HDR_LEN];
	u16 lhs, rhs;

	memcpy(buf, skb->data, skb->len);

	lhs = le16_to_cpu(*((__le16 *)(&buf[1])));
	rhs = le16_to_cpu(*((__le16 *)(&buf[3])));
	if ((lhs ^ rhs) == 0xffff) {
		mrvl_send_ack(hu, MRVL_ACK);
		fw_data->wait_fw = 1;
		fw_data->next_len = lhs;
		/*firmware download is done, send the last ack*/
		if (!lhs)
			fw_data->last_ack = 1;

		if (unlikely(fw_data->expected_ack == MRVL_HDR_CHIP_VER)) {
			fw_data->chip_id = *((u8 *)(&buf[1]));
			fw_data->chip_rev = *((u8 *)(&buf[2]));
		}
		wake_up_interruptible(&fw_data->init_wait_q);
	} else {
		mrvl_send_ack(hu, MRVL_NAK);
	}
}

/* This function receives data from the uart device during firmware download.
 * Driver expects 5 bytes of data as per the protocal in the below format:
 * <HEADER><BYTE_1><BYTE_2><BYTE_3><BYTE_4>
 * BYTE_3 and BYTE_4 are compliment of BYTE_1 an BYTE_2. Data can come in chunks
 * of any length. If length received is < 5, accumulate the data in an array,
 * until we have a sequence of 5 bytes, starting with the expected HEADER. If
 * the length received is > 5  bytes, then get the first 5 bytes, starting with
 * the HEADER and process the same, ignoring the rest of the bytes as per the
 * protocal.
 */
static struct sk_buff *mrvl_process_fw_data(struct hci_uart *hu,
					    struct sk_buff *skb,
					    u8 *buf, int count)
{
	struct mrvl_data *mrvl = hu->priv;
	struct fw_data *fw_data = mrvl->fwdata;
	int i = 0, len;

	if (!skb) {
		while (buf[i] != fw_data->expected_ack && i < count)
			i++;
		if (i == count)
			return ERR_PTR(-EILSEQ);

		skb = bt_skb_alloc(MRVL_FW_HDR_LEN, GFP_KERNEL);
	}

	if (!skb)
		return ERR_PTR(-ENOMEM);

	len = count - i;
	memcpy(skb_put(skb, len), &buf[i], len);

	if (skb->len == MRVL_FW_HDR_LEN) {
		mrvl_pkt_complete(hu, skb);
		kfree_skb(skb);
		skb = NULL;
	}

	return skb;
}

/* Receive data */
static int mrvl_recv(struct hci_uart *hu, const void *data, int count)
{
	struct mrvl_data *mrvl = hu->priv;

	if (test_bit(HCI_UART_DNLD_FW, &mrvl->flags)) {
		mrvl->fwdata->skb = mrvl_process_fw_data(hu, mrvl->fwdata->skb,
							 (u8 *)data, count);
		if (IS_ERR(mrvl->fwdata->skb)) {
			int err = PTR_ERR(mrvl->fwdata->skb);

			bt_dev_err(hu->hdev,
				   "Receive firmware data failed (%d)", err);
			mrvl->fwdata->skb = NULL;
			return err;
		}
		return 0;
	}

	if (!test_bit(HCI_UART_REGISTERED, &hu->flags))
		return -EUNATCH;

	mrvl->rx_skb = h4_recv_buf(hu->hdev, mrvl->rx_skb, data, count,
				   mrvl_recv_pkts, ARRAY_SIZE(mrvl_recv_pkts));
	if (IS_ERR(mrvl->rx_skb)) {
		int err = PTR_ERR(mrvl->rx_skb);

		bt_dev_err(hu->hdev, "Frame reassembly failed (%d)", err);
		mrvl->rx_skb = NULL;
		return err;
	}

	return count;
}

static struct sk_buff *mrvl_dequeue(struct hci_uart *hu)
{
	struct mrvl_data *mrvl = hu->priv;

	return skb_dequeue(&mrvl->txq);
}

static int mrvl_init_fw_data(struct hci_uart *hu)
{
	struct fw_data *fwdata;
	struct mrvl_data *mrvl = hu->priv;

	fwdata = kzalloc(sizeof(*fwdata), GFP_KERNEL);
	if (!fwdata)
		return -ENOMEM;

	mrvl->fwdata = fwdata;
	init_waitqueue_head(&fwdata->init_wait_q);

	return 0;
}

/* Wait for the header from device */
static int mrvl_wait_for_hdr(struct hci_uart *hu, u8 header)
{
	struct mrvl_data *mrvl = hu->priv;
	struct fw_data *fw_data = mrvl->fwdata;

	fw_data->expected_ack = header;
	fw_data->wait_fw = 0;

	if (!wait_event_interruptible_timeout(fw_data->init_wait_q,
					      fw_data->wait_fw,
					      MRVL_WAIT_TIMEOUT)) {
		BT_ERR("TIMEOUT, waiting for:0x%x", fw_data->expected_ack);
		return -1;
	}

	return 0;
}

/* Send bytes to device */
static int mrvl_send_data(struct hci_uart *hu, struct sk_buff *skb)
{
	struct mrvl_data *mrvl = hu->priv;

	skb_queue_head(&mrvl->txq, skb);
	hci_uart_tx_wakeup(hu);

	if (mrvl_wait_for_hdr(hu, MRVL_HDR_REQ_FW) == -1)
		return -1;

	return 0;
}

/* Download firmware to the device */
static int mrvl_dnld_fw(struct hci_uart *hu, const char *file_name)
{
	const struct firmware *fw = NULL;
	struct sk_buff *skb = NULL;
	int offset = 0;
	int ret, tx_len;
	struct mrvl_data *mrvl = hu->priv;
	struct fw_data *fw_data = mrvl->fwdata;

	ret = request_firmware(&fw, file_name, hu->tty->dev);
	if (ret < 0) {
		BT_ERR("request_firmware() failed");
		return -1;
	}

	BT_INFO("Downloading FW (%d bytes)", (u16)fw->size);

	fw_data->last_ack = 0;

	do {
		if ((offset >= fw->size) || (fw_data->last_ack))
			break;
		tx_len = fw_data->next_len;
		if ((fw->size - offset) < tx_len)
			tx_len = fw->size - offset;

		skb = bt_skb_alloc(MRVL_MAX_FW_BLOCK_SIZE, GFP_KERNEL);
		if (!skb) {
			ret = -1;
			goto done;
		}

		memcpy(skb->data, &fw->data[offset], tx_len);
		skb_put(skb, tx_len);
		if (mrvl_send_data(hu, skb) != 0) {
			BT_ERR("fail to download firmware");
			ret = -1;
			goto done;
		}
		offset += tx_len;
	} while (1);

	BT_INFO("downloaded %d byte firmware", offset);
done:
	release_firmware(fw);

	return ret;
}

/* Set the baud rate */
static int mrvl_set_dev_baud(struct hci_uart *hu)
{
	struct hci_dev *hdev = hu->hdev;
	struct sk_buff *skb;
	static const u8 baud_param[] = { 0xc0, 0xc6, 0x2d, 0x00 };
	int err;

	skb = __hci_cmd_sync(hdev, MRVL_HCI_OP_SET_BAUD, sizeof(baud_param),
			     baud_param, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		err = PTR_ERR(skb);
		bt_dev_err(hu->hdev, "Set device baudrate failed (%d)", err);
		return err;
	}
	kfree_skb(skb);

	return 0;
}

/* Reset device */
static int mrvl_reset(struct hci_uart *hu)
{
	struct hci_dev *hdev = hu->hdev;
	struct sk_buff *skb;
	int err;

	skb = __hci_cmd_sync(hdev, HCI_OP_RESET, 0, NULL, HCI_CMD_TIMEOUT);
	if (IS_ERR(skb)) {
		err = PTR_ERR(skb);
		bt_dev_err(hu->hdev, "Reset device failed (%d)", err);
		return err;
	}
	kfree_skb(skb);

	return 0;
}

static int mrvl_set_baud(struct hci_uart *hu)
{
	int err;

	hci_uart_set_baudrate(hu, 115200);
	hci_uart_set_flow_control(hu, false);

	err = mrvl_reset(hu);
	if (err)
		goto set_term_baud;
	else
		goto set_dev_baud;

set_dev_baud:
	err = mrvl_set_dev_baud(hu);
	if (err)
		return -1;

set_term_baud:
	hci_uart_set_baudrate(hu, 3000000);
	hci_uart_set_flow_control(hu, false);

	msleep(MRVL_DNLD_DELAY);

	return 0;
}

static int mrvl_get_fw_name(struct hci_uart *hu, char *fw_name)
{
	struct mrvl_data *mrvl = hu->priv;
	struct fw_data *fw_data = mrvl->fwdata;

	if (mrvl_wait_for_hdr(hu, MRVL_HDR_CHIP_VER) != 0) {
		BT_ERR("Could not read chip id and revision");
		return -1;
	}

	BT_DBG("chip_id=0x%x, chip_rev=0x%x",
	       fw_data->chip_id, fw_data->chip_rev);

	switch (fw_data->chip_id) {
	case MRVL_8997_CHIP_ID:
		memcpy(fw_name, MRVL_8997_FW_NAME, sizeof(MRVL_8997_FW_NAME));
		return 0;
	default:
		BT_ERR("Invalid chip id");
		return -1;
	}
}

/* Download helper and firmare to device */
static int hci_uart_dnld_fw(struct hci_uart *hu)
{
	struct tty_struct *tty = hu->tty;
	struct ktermios new_termios;
	struct ktermios old_termios;
	struct mrvl_data *mrvl = hu->priv;
	char fw_name[128];
	int ret;

	old_termios = tty->termios;

	if (get_cts(hu)) {
		BT_INFO("fw is running");
		clear_bit(HCI_UART_DNLD_FW, &mrvl->flags);
		return 0;
	}

	hci_uart_set_baudrate(hu, 115200);
	hci_uart_set_flow_control(hu, true);

	ret = mrvl_wait_for_hdr(hu, MRVL_HDR_REQ_FW);
	if (ret)
		goto fail;

	ret = mrvl_dnld_fw(hu, MRVL_HELPER_NAME);
	if (ret)
		goto fail;

	msleep(MRVL_DNLD_DELAY);

	hci_uart_set_baudrate(hu, 3000000);
	hci_uart_set_flow_control(hu, false);

	ret = mrvl_get_fw_name(hu, fw_name);
	if (ret)
		goto fail;

	ret = mrvl_wait_for_hdr(hu, MRVL_HDR_REQ_FW);
	if (ret)
		goto fail;

	ret = mrvl_dnld_fw(hu, fw_name);
	if (ret)
		goto fail;

	msleep(MRVL_DNLD_DELAY);
fail:
	/* restore uart settings */
	new_termios = tty->termios;
	tty->termios.c_cflag = old_termios.c_cflag;
	tty_set_termios(tty, &new_termios);
	clear_bit(HCI_UART_DNLD_FW, &mrvl->flags);

	return ret;
}

static int mrvl_setup(struct hci_uart *hu)
{
	return mrvl_set_baud(hu);
}

static int mrvl_prepare(struct hci_uart *hu)
{
	struct mrvl_data *mrvl = hu->priv;

	mrvl_init_fw_data(hu);
	set_bit(HCI_UART_DNLD_FW, &mrvl->flags);

	return hci_uart_dnld_fw(hu);
}

static const struct hci_uart_proto mrvlp = {
	.id		= HCI_UART_MRVL,
	.name		= "MRVL",
	.open		= mrvl_open,
	.close		= mrvl_close,
	.recv		= mrvl_recv,
	.enqueue	= mrvl_enqueue,
	.dequeue	= mrvl_dequeue,
	.flush		= mrvl_flush,
	.setup		= mrvl_setup,
	.prepare	= mrvl_prepare,
};

int __init mrvl_init(void)
{
	return hci_uart_register_proto(&mrvlp);
}

int __exit mrvl_deinit(void)
{
	return hci_uart_unregister_proto(&mrvlp);
}
