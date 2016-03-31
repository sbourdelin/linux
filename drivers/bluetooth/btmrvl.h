/* Bluetooth support for Marvell devices
 *
 * Copyright (C) 2016, Marvell International Ltd.
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

struct fw_data {
	wait_queue_head_t init_wait_q __aligned(4);
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

#define MRVL_HELPER_NAME	"mrvl/helper_uart_3000000.bin"
#define MRVL_8997_FW_NAME	"mrvl/uart8997_bt.bin"
#define MRVL_WAIT_TIMEOUT	12000
#define MRVL_MAX_FW_BLOCK_SIZE	1024
#define MRVL_MAX_RETRY_SEND	12
#define MRVL_DNLD_DELAY		100
#define MRVL_ACK		0x5A
#define MRVL_NAK		0xBF
#define MRVL_HDR_REQ_FW		0xA5
#define MRVL_HDR_CHIP_VER	0xAA
#define MRVL_HCI_OP_SET_BAUD	0xFC09
#define MRVL_FW_HDR_LEN		5
