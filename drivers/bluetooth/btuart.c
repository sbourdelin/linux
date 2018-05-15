/*
 *
 *  Generic Bluetooth HCI UART driver
 *
 *  Copyright (C) 2015-2018  Intel Corporation
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/serdev.h>
#include <linux/of.h>
#include <linux/firmware.h>
#include <asm/unaligned.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "h4_recv.h"
#include "btuart.h"
#include "btbcm.h"

#define VERSION "1.0"

static void btuart_tx_work(struct work_struct *work)
{
	struct btuart_dev *bdev = container_of(work, struct btuart_dev,
					       tx_work);
	struct serdev_device *serdev = bdev->serdev;
	struct hci_dev *hdev = bdev->hdev;

	while (1) {
		clear_bit(BTUART_TX_STATE_WAKEUP, &bdev->tx_state);

		while (1) {
			struct sk_buff *skb = skb_dequeue(&bdev->txq);
			int len;

			if (!skb)
				break;

			len = serdev_device_write_buf(serdev, skb->data,
						      skb->len);
			hdev->stat.byte_tx += len;

			skb_pull(skb, len);
			if (skb->len > 0) {
				skb_queue_head(&bdev->txq, skb);
				break;
			}

			switch (hci_skb_pkt_type(skb)) {
			case HCI_COMMAND_PKT:
				hdev->stat.cmd_tx++;
				break;
			case HCI_ACLDATA_PKT:
				hdev->stat.acl_tx++;
				break;
			case HCI_SCODATA_PKT:
				hdev->stat.sco_tx++;
				break;
			}

			kfree_skb(skb);
		}

		if (!test_bit(BTUART_TX_STATE_WAKEUP, &bdev->tx_state))
			break;
	}

	clear_bit(BTUART_TX_STATE_ACTIVE, &bdev->tx_state);
}

static int btuart_tx_wakeup(struct btuart_dev *bdev)
{
	if (test_and_set_bit(BTUART_TX_STATE_ACTIVE, &bdev->tx_state)) {
		set_bit(BTUART_TX_STATE_WAKEUP, &bdev->tx_state);
		return 0;
	}

	schedule_work(&bdev->tx_work);
	return 0;
}

static int btuart_open(struct hci_dev *hdev)
{
	struct btuart_dev *bdev = hci_get_drvdata(hdev);
	int err;

	err = serdev_device_open(bdev->serdev);
	if (err) {
		bt_dev_err(hdev, "Unable to open UART device %s",
			   dev_name(&bdev->serdev->dev));
		return err;
	}

	if (bdev->vnd->open) {
		err = bdev->vnd->open(hdev);
		if (err) {
			serdev_device_close(bdev->serdev);
			return err;
		}
	}

	return 0;
}

static int btuart_close(struct hci_dev *hdev)
{
	struct btuart_dev *bdev = hci_get_drvdata(hdev);
	int err;

	if (bdev->vnd->close) {
		err = bdev->vnd->close(hdev);
		if (err)
			return err;
	}

	serdev_device_close(bdev->serdev);

	return 0;
}

static int btuart_flush(struct hci_dev *hdev)
{
	struct btuart_dev *bdev = hci_get_drvdata(hdev);

	/* Flush any pending characters */
	serdev_device_write_flush(bdev->serdev);
	skb_queue_purge(&bdev->txq);

	cancel_work_sync(&bdev->tx_work);

	kfree_skb(bdev->rx_skb);
	bdev->rx_skb = NULL;

	return 0;
}

static int btuart_setup(struct hci_dev *hdev)
{
	struct btuart_dev *bdev = hci_get_drvdata(hdev);

	if (bdev->vnd->setup)
		return bdev->vnd->setup(hdev);

	return 0;
}

static int btuart_shutdown(struct hci_dev *hdev)
{
	struct btuart_dev *bdev = hci_get_drvdata(hdev);

	if (bdev->vnd->shutdown)
		return bdev->vnd->shutdown(hdev);

	return 0;
}

static int btuart_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btuart_dev *bdev = hci_get_drvdata(hdev);

	if (bdev->vnd->send) {
		bdev->vnd->send(hdev, skb);
	} else {
		/* Prepend skb with frame type */
		memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);
		skb_queue_tail(&bdev->txq, skb);
	}

	btuart_tx_wakeup(bdev);
	return 0;
}

static int btuart_receive_buf(struct serdev_device *serdev, const u8 *data,
			      size_t count)
{
	struct btuart_dev *bdev = serdev_device_get_drvdata(serdev);
	const struct btuart_vnd *vnd = bdev->vnd;
	int err;

	if (bdev->vnd->recv) {
		err = bdev->vnd->recv(bdev->hdev, data, count);
		if (err < 0)
			return err;
	} else {
		bdev->rx_skb = h4_recv_buf(bdev->hdev, bdev->rx_skb,
					   data, count, vnd->recv_pkts,
					   vnd->recv_pkts_cnt);
		if (IS_ERR(bdev->rx_skb)) {
			err = PTR_ERR(bdev->rx_skb);
			bt_dev_err(bdev->hdev,
				   "Frame reassembly failed (%d)", err);
			bdev->rx_skb = NULL;
			return err;
		}
	}

	bdev->hdev->stat.byte_rx += count;

	return count;
}

static void btuart_write_wakeup(struct serdev_device *serdev)
{
	struct btuart_dev *bdev = serdev_device_get_drvdata(serdev);

	btuart_tx_wakeup(bdev);
}

static const struct serdev_device_ops btuart_client_ops = {
	.receive_buf = btuart_receive_buf,
	.write_wakeup = btuart_write_wakeup,
};

#define BCM_NULL_PKT 0x00
#define BCM_NULL_SIZE 0

#define BCM_LM_DIAG_PKT 0x07
#define BCM_LM_DIAG_SIZE 63

#define BCM_RECV_LM_DIAG \
	.type = BCM_LM_DIAG_PKT, \
	.hlen = BCM_LM_DIAG_SIZE, \
	.loff = 0, \
	.lsize = 0, \
	.maxlen = BCM_LM_DIAG_SIZE

#define BCM_RECV_NULL \
	.type = BCM_NULL_PKT, \
	.hlen = BCM_NULL_SIZE, \
	.loff = 0, \
	.lsize = 0, \
	.maxlen = BCM_NULL_SIZE

static int bcm_set_diag(struct hci_dev *hdev, bool enable)
{
	struct btuart_dev *bdev = hci_get_drvdata(hdev);
	struct sk_buff *skb;

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return -ENETDOWN;

	skb = bt_skb_alloc(3, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	skb_put_u8(skb, BCM_LM_DIAG_PKT);
	skb_put_u8(skb, 0xf0);
	skb_put_u8(skb, enable);

	skb_queue_tail(&bdev->txq, skb);
	btuart_tx_wakeup(bdev);

	return 0;
}

static int bcm_set_baudrate(struct btuart_dev *bdev, unsigned int speed)
{
	struct hci_dev *hdev = bdev->hdev;
	struct sk_buff *skb;
	struct bcm_update_uart_baud_rate param;

	if (speed > 3000000) {
		struct bcm_write_uart_clock_setting clock;

		clock.type = BCM_UART_CLOCK_48MHZ;

		bt_dev_dbg(hdev, "Set Controller clock (%d)", clock.type);

		/* This Broadcom specific command changes the UART's controller
		 * clock for baud rate > 3000000.
		 */
		skb = __hci_cmd_sync(hdev, 0xfc45, 1, &clock, HCI_INIT_TIMEOUT);
		if (IS_ERR(skb)) {
			int err = PTR_ERR(skb);
			bt_dev_err(hdev, "Failed to write clock (%d)", err);
			return err;
		}

		kfree_skb(skb);
	}

	bt_dev_dbg(hdev, "Set Controller UART speed to %d bit/s", speed);

	param.zero = cpu_to_le16(0);
	param.baud_rate = cpu_to_le32(speed);

	/* This Broadcom specific command changes the UART's controller baud
	 * rate.
	 */
	skb = __hci_cmd_sync(hdev, 0xfc18, sizeof(param), &param,
			     HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		int err = PTR_ERR(skb);
		bt_dev_err(hdev, "Failed to write update baudrate (%d)", err);
		return err;
	}

	kfree_skb(skb);

	return 0;
}

static int bcm_setup(struct hci_dev *hdev)
{
	struct btuart_dev *bdev = hci_get_drvdata(hdev);
	char fw_name[64];
	const struct firmware *fw;
	unsigned int speed;
	int err;

	hdev->set_diag = bcm_set_diag;
	hdev->set_bdaddr = btbcm_set_bdaddr;

	/* Init speed if any */
	speed = 115200;

	if (speed)
		serdev_device_set_baudrate(bdev->serdev, speed);

	/* Operational speed if any */
	speed = 115200;

	if (speed) {
		err = bcm_set_baudrate(bdev, speed);
		if (err)
			bt_dev_err(hdev, "Failed to set baudrate");
		else
			serdev_device_set_baudrate(bdev->serdev, speed);
	}

	err = btbcm_initialize(hdev, fw_name, sizeof(fw_name));
	if (err)
		return err;

	err = request_firmware(&fw, fw_name, &hdev->dev);
	if (err < 0) {
		bt_dev_warn(hdev, "Patch %s not found", fw_name);
		return 0;
	}

	err = btbcm_patchram(bdev->hdev, fw);
	if (err) {
		bt_dev_err(hdev, "Patching failed (%d)", err);
		goto finalize;
	}

	/* Init speed if any */
	speed = 115200;

	if (speed)
		serdev_device_set_baudrate(bdev->serdev, speed);

	/* Operational speed if any */
	speed = 115200;

	if (speed) {
		err = bcm_set_baudrate(bdev, speed);
		if (!err)
			serdev_device_set_baudrate(bdev->serdev, speed);
	}

finalize:
	release_firmware(fw);

	err = btbcm_finalize(hdev);
	if (err)
		return err;

	return err;
}

static const struct h4_recv_pkt bcm_recv_pkts[] = {
	{ H4_RECV_ACL,      .recv = hci_recv_frame },
	{ H4_RECV_SCO,      .recv = hci_recv_frame },
	{ H4_RECV_EVENT,    .recv = hci_recv_frame },
	{ BCM_RECV_LM_DIAG, .recv = hci_recv_diag  },
	{ BCM_RECV_NULL,    .recv = hci_recv_diag  },
};

static const struct btuart_vnd bcm_vnd = {
	.recv_pkts	= bcm_recv_pkts,
	.recv_pkts_cnt	= ARRAY_SIZE(bcm_recv_pkts),
	.manufacturer	= 15,
	.setup		= bcm_setup,
};

static const struct h4_recv_pkt default_recv_pkts[] = {
	{ H4_RECV_ACL,      .recv = hci_recv_frame },
	{ H4_RECV_SCO,      .recv = hci_recv_frame },
	{ H4_RECV_EVENT,    .recv = hci_recv_frame },
};

static const struct btuart_vnd default_vnd = {
	.recv_pkts	= default_recv_pkts,
	.recv_pkts_cnt	= ARRAY_SIZE(default_recv_pkts),
};

static int btuart_probe(struct serdev_device *serdev)
{
	struct btuart_dev *bdev;
	struct hci_dev *hdev;

	bdev = devm_kzalloc(&serdev->dev, sizeof(*bdev), GFP_KERNEL);
	if (!bdev)
		return -ENOMEM;

	/* Request the vendor specific data and callbacks */
	bdev->vnd = device_get_match_data(&serdev->dev);
	if (!bdev->vnd)
		bdev->vnd = &default_vnd;

	if (bdev->vnd->init)
		bdev->data = bdev->vnd->init(&serdev->dev);

	bdev->serdev = serdev;
	serdev_device_set_drvdata(serdev, bdev);

	serdev_device_set_client_ops(serdev, &btuart_client_ops);

	INIT_WORK(&bdev->tx_work, btuart_tx_work);
	skb_queue_head_init(&bdev->txq);

	/* Initialize and register HCI device */
	hdev = hci_alloc_dev();
	if (!hdev) {
		dev_err(&serdev->dev, "Can't allocate HCI device\n");
		return -ENOMEM;
	}

	bdev->hdev = hdev;

	hdev->bus = HCI_UART;
	hci_set_drvdata(hdev, bdev);

	/* Only when vendor specific setup callback is provided, consider
	 * the manufacturer information valid. This avoids filling in the
	 * value for Ericsson when nothing is specified.
	 */
	if (bdev->vnd->setup)
		hdev->manufacturer = bdev->vnd->manufacturer;

	hdev->open  = btuart_open;
	hdev->close = btuart_close;
	hdev->flush = btuart_flush;
	hdev->setup = btuart_setup;
	hdev->shutdown = btuart_shutdown;
	hdev->send  = btuart_send_frame;
	SET_HCIDEV_DEV(hdev, &serdev->dev);

	if (hci_register_dev(hdev) < 0) {
		dev_err(&serdev->dev, "Can't register HCI device\n");
		hci_free_dev(hdev);
		return -ENODEV;
	}

	return 0;
}

static void btuart_remove(struct serdev_device *serdev)
{
	struct btuart_dev *bdev = serdev_device_get_drvdata(serdev);
	struct hci_dev *hdev = bdev->hdev;

	hci_unregister_dev(hdev);
	hci_free_dev(hdev);
}

#ifdef CONFIG_OF
static const struct of_device_id btuart_of_match_table[] = {
	{ .compatible = "brcm,bcm43438-bt", .data = &bcm_vnd },
	{ }
};
MODULE_DEVICE_TABLE(of, btuart_of_match_table);
#endif

static struct serdev_device_driver btuart_driver = {
	.probe = btuart_probe,
	.remove = btuart_remove,
	.driver = {
		.name = "btuart",
		.of_match_table = of_match_ptr(btuart_of_match_table),
	},
};

module_serdev_device_driver(btuart_driver);

MODULE_AUTHOR("Marcel Holtmann <marcel@holtmann.org>");
MODULE_DESCRIPTION("Generic Bluetooth UART driver ver " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL");
