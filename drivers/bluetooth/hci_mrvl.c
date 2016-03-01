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

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/poll.h>
#include <linux/firmware.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/signal.h>
#include <linux/ioctl.h>
#include <linux/skbuff.h>
#include <asm/unaligned.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "hci_uart.h"
#include "btmrvl.h"

struct mrvl_data {
	struct sk_buff *rx_skb;
	struct sk_buff_head txq;
	struct fw_data *fwdata;
};

/* Initialize protocol */
static int mrvl_open(struct hci_uart *hu)
{
	struct mrvl_data *mrvl;

	BT_DBG("hu %p", hu);

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

	BT_DBG("hu %p", hu);

	skb_queue_purge(&mrvl->txq);

	return 0;
}

/* Close protocol */
static int mrvl_close(struct hci_uart *hu)
{
	struct mrvl_data *mrvl = hu->priv;

	BT_DBG("hu %p", hu);

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

	if (test_bit(HCI_UART_DNLD_FW, &hu->flags))
		return -EBUSY;

	BT_DBG("hu %p skb %p", hu, skb);

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

/* Receive data */
static int mrvl_recv(struct hci_uart *hu, const void *data, int count)
{
	struct mrvl_data *mrvl = hu->priv;

	if (test_bit(HCI_UART_DNLD_FW, &hu->flags)) {
		hci_uart_recv_data(hu, (u8 *)data, count);
		return 0;
	}

	if (!test_bit(HCI_UART_REGISTERED, &hu->flags))
		return -EUNATCH;

	mrvl->rx_skb = h4_recv_buf(hu->hdev, mrvl->rx_skb, data, count,
				   mrvl_recv_pkts, ARRAY_SIZE(mrvl_recv_pkts));
	if (IS_ERR(mrvl->rx_skb)) {
		int err = PTR_ERR(mrvl->rx_skb);

		BT_ERR("%s: Frame reassembly failed (%d)", hu->hdev->name, err);
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

	if (!fwdata) {
		BT_ERR("Can't allocate firmware data");
		return -ENOMEM;
	}

	mrvl->fwdata = fwdata;
	init_waitqueue_head(&fwdata->init_wait_q);

	return 0;
}

static int mrvl_setup(struct hci_uart *hu)
{
	mrvl_init_fw_data(hu);
	set_bit(HCI_UART_DNLD_FW, &hu->flags);

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
};

int __init mrvl_init(void)
{
	return hci_uart_register_proto(&mrvlp);
}

int __exit mrvl_deinit(void)
{
	return hci_uart_unregister_proto(&mrvlp);
}

static int get_cts(struct tty_struct *tty)
{
	u32 state;

	if (tty->ops->tiocmget) {
		state = tty->ops->tiocmget(tty);
		if (state & TIOCM_CTS) {
			BT_DBG("CTS is low");
			return 1;
		}
		BT_DBG("CTS is high");
		return 0;
	}
	return -1;
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
					((MRVL_WAIT_TIMEOUT) * HZ / 1000))) {
		BT_ERR("TIMEOUT, waiting for:0x%x", fw_data->expected_ack);
		return -1;
	}

	return 0;
}

/* Send bytes to device */
static int mrvl_send_data(struct hci_uart *hu, struct sk_buff *skb)
{
	struct tty_struct *tty = hu->tty;
	int retry = 0;
	int skb_len;

	skb_len = skb->len;
	while (retry < MRVL_MAX_RETRY_SEND) {
		tty->ops->write(tty, skb->data, skb->len);
		if (mrvl_wait_for_hdr(hu, MRVL_HDR_REQ_FW) == -1) {
			retry++;
			continue;
		} else {
			skb_pull(skb, skb_len);
			break;
		}
	}
	if (retry == MRVL_MAX_RETRY_SEND)
		return -1;

	return 0;
}

/* Download firmware to the device */
static int mrvl_dnld_fw(struct hci_uart *hu, const char *file_name)
{
	struct hci_dev *hdev = hu->hdev;
	const struct firmware *fw;
	struct sk_buff *skb = NULL;
	int offset = 0;
	int ret, tx_len;
	struct mrvl_data *mrvl = hu->priv;
	struct fw_data *fw_data = mrvl->fwdata;

	ret = request_firmware(&fw, file_name, &hdev->dev);
	if (ret < 0) {
		BT_ERR("request_firmware() failed");
		ret = -1;
		goto done;
	}
	if (fw) {
		BT_INFO("Downloading FW (%d bytes)", (u16)fw->size);
	} else {
		BT_ERR("No FW image found");
		ret = -1;
		goto done;
	}

	skb = bt_skb_alloc(MRVL_MAX_FW_BLOCK_SIZE, GFP_ATOMIC);
	if (!skb) {
		BT_ERR("cannot allocate memory for skb");
		ret = -1;
		goto done;
	}

	skb->dev = (void *)hdev;
	fw_data->last_ack = 0;

	do {
		if ((offset >= fw->size) || (fw_data->last_ack))
			break;
		tx_len = fw_data->next_len;
		if ((fw->size - offset) < tx_len)
			tx_len = fw->size - offset;

		memcpy(skb->data, &fw->data[offset], tx_len);
		skb_put(skb, tx_len);
		if (mrvl_send_data(hu, skb) != 0) {
			BT_ERR("fail to download firmware");
			ret = -1;
			goto done;
		}
		skb_push(skb, tx_len);
		skb_trim(skb, 0);
		offset += tx_len;
	} while (1);

	BT_INFO("downloaded %d byte firmware", offset);
done:
	if (fw)
		release_firmware(fw);

	kfree(skb);
	return ret;
}

/* Get standard baud rate, given the speed */
static unsigned int get_baud_rate(unsigned int speed)
{
	switch (speed) {
	case 9600:
		return B9600;
	case 19200:
		return B19200;
	case 38400:
		return B38400;
	case 57600:
		return B57600;
	case 115200:
		return B115200;
	case 230400:
		return B230400;
	case 460800:
		return B460800;
	case 921600:
		return B921600;
	case 2000000:
		return B2000000;
	case 3000000:
		return B3000000;
	default:
		return -1;
	}
}

/* Set terminal properties */
static int mrvl_set_term_baud(struct tty_struct *tty, unsigned int speed,
			      unsigned char flow_ctl)
{
	struct ktermios old_termios = tty->termios;
	int baud;

	tty->termios.c_cflag &= ~CBAUD;
	baud = get_baud_rate(speed);

	if (baud == -1) {
		BT_ERR("Baud rate not supported");
		return -1;
	}

	tty->termios.c_cflag |= baud;

	if (flow_ctl)
		tty->termios.c_cflag |= CRTSCTS;
	else
		tty->termios.c_cflag &= ~CRTSCTS;

	tty->ops->set_termios(tty, &old_termios);

	return 0;
}

/* Set the baud rate */
static int mrvl_set_dev_baud(struct hci_uart *hu)
{
	struct hci_dev *hdev = hu->hdev;
	struct sk_buff *skb;
	static const u8 baud_param[] = { 0xc0, 0xc6, 0x2d, 0x00};
	int err;

	skb = __hci_cmd_sync(hdev, MRVL_HCI_OP_SET_BAUD, sizeof(baud_param),
			     baud_param, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		err = PTR_ERR(skb);
		BT_ERR("%s: Set device baudrate failed (%d)", hdev->name, err);
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
		BT_ERR("%s: Reset device failed (%d)", hdev->name, err);
		return err;
	}
	kfree_skb(skb);

	return 0;
}

static int mrvl_set_baud(struct hci_uart *hu)
{
	struct tty_struct *tty = hu->tty;
	int ret;

	ret = mrvl_set_term_baud(tty, 115200, 1);
	if (ret)
		return -1;

	ret = mrvl_reset(hu);
	if (ret)
		goto set_term_baud;
	else
		goto set_dev_baud;

set_dev_baud:
	ret = mrvl_set_dev_baud(hu);
	if (ret)
		return -1;

set_term_baud:
	ret = mrvl_set_term_baud(tty, 3000000, 1);
	if (ret)
		return -1;

	return ret;
}

static int mrvl_get_fw_name(struct hci_uart *hu, char *fw_name)
{
	int ret;
	struct mrvl_data *mrvl = hu->priv;
	struct fw_data *fw_data = mrvl->fwdata;

	ret = mrvl_wait_for_hdr(hu, MRVL_HDR_CHIP_VER);
	if (ret) {
		ret = -1;
		BT_ERR("Could not read chip id and revision");
		goto fail;
	}

	BT_DBG("chip_id=0x%x, chip_rev=0x%x",
	       fw_data->chip_id, fw_data->chip_rev);

	if (fw_data->chip_id == 0x50) {
		memcpy(fw_name, MRVL_8997_FW_NAME, sizeof(MRVL_8997_FW_NAME));
	} else {
		ret = -1;
		BT_ERR("Invalid chip id");
	}
fail:
	return ret;
}

/* Download helper and firmare to device */
int hci_uart_dnld_fw(struct hci_uart *hu)
{
	struct tty_struct *tty = hu->tty;
	struct ktermios new_termios;
	struct ktermios old_termios;
	char fw_name[128];
	int ret;

	old_termios = tty->termios;

	if (!tty) {
		BT_ERR("tty is null");
		clear_bit(HCI_UART_DNLD_FW, &hu->flags);
		ret = -1;
		goto fail;
	}

	if (get_cts(hu->tty)) {
		BT_INFO("fw is running");
		clear_bit(HCI_UART_DNLD_FW, &hu->flags);
		goto set_baud;
	}

	ret = mrvl_set_term_baud(tty, 115200, 0);
	if (ret)
		goto fail;

	ret = mrvl_wait_for_hdr(hu, MRVL_HDR_REQ_FW);
	if (ret)
		goto fail;

	ret = mrvl_dnld_fw(hu, MRVL_HELPER_NAME);
	if (ret)
		goto fail;

	mdelay(MRVL_DNLD_DELAY);

	ret = mrvl_set_term_baud(tty, 3000000, 1);
	if (ret)
		goto fail;

	ret = mrvl_get_fw_name(hu, fw_name);
	if (ret)
		goto fail;

	ret = mrvl_wait_for_hdr(hu, MRVL_HDR_REQ_FW);
	if (ret)
		goto fail;

	ret = mrvl_dnld_fw(hu, fw_name);
	if (ret)
		goto fail;

	mdelay(MRVL_DNLD_DELAY);
	/* restore uart settings */
	new_termios = tty->termios;
	tty->termios.c_cflag = old_termios.c_cflag;
	tty->ops->set_termios(tty, &new_termios);
	clear_bit(HCI_UART_DNLD_FW, &hu->flags);

set_baud:
	ret = mrvl_set_baud(hu);
	if (ret)
		goto fail;

	mdelay(MRVL_DNLD_DELAY);

	return ret;
fail:
	/* restore uart settings */
	new_termios = tty->termios;
	tty->termios.c_cflag = old_termios.c_cflag;
	tty->ops->set_termios(tty, &new_termios);
	clear_bit(HCI_UART_DNLD_FW, &hu->flags);

	return ret;
}

/* Send ACK/NAK to the device */
static void mrvl_send_ack(struct hci_uart *hu, unsigned char ack)
{
	struct tty_struct *tty = hu->tty;

	tty->ops->write(tty, &ack, sizeof(ack));
}

/* Validate the feedback data from device */
static void mrvl_validate_hdr_and_len(struct hci_uart *hu, u8 *buf)
{
	struct mrvl_data *mrvl = hu->priv;
	struct fw_data *fw_data = mrvl->fwdata;
	u16 lhs, rhs;

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

void hci_uart_recv_data(struct hci_uart *hu, u8 *buf, int len)
{
	struct mrvl_data *mrvl = hu->priv;
	struct fw_data *fw_data = mrvl->fwdata;
	int i = 0;

	if (len < 5) {
		if ((!fw_data->next_index) &&
		    (buf[0] != fw_data->expected_ack)) {
			/*ex: XX XX XX*/
			return;
		}
	}

	if (len == 5) {
		if (buf[0] != fw_data->expected_ack) {
			/*ex: XX XX XX XX XX*/
			return;
		}
		/*ex: 5A LL LL LL LL*/
		fw_data->next_index = 0;
		mrvl_validate_hdr_and_len(hu, buf);
		return;
	}

	if (len > 5) {
		i = 0;

		while ((i < len) && (buf[i] != fw_data->expected_ack))
			i++;

		if (i == len) {
			/* Could not find a header */
			return;
		}

		if ((len - i) >= 5) {
			/*ex: 00 00 00 00 a5 LL LL LL LL*/
			/*ex: a5 LL LL LL LL 00 00 00 00*/
			/*ex: 00 00 a5 LL LL LL LL 00 00*/
			/*ex: a5 LL LL LL LL*/
			fw_data->next_index = 0;
			mrvl_validate_hdr_and_len(hu, buf + i);
			return;
		}

		/*ex: 00 00 00 00 a5 LL LL*/
		hci_uart_recv_data(hu, buf + i, len - i);
		return;
	}

	for (i = 0; i < len; i++) {
		fw_data->five_bytes[fw_data->next_index] = buf[i];
		if (++fw_data->next_index == 5) {
			fw_data->next_index = 0;
			mrvl_validate_hdr_and_len(hu, fw_data->five_bytes);
			return;
		}
	}
}
