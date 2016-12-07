/*
 * Common control routine of micro-controller of KuroBox-Pro
 * and it's variants.
 * It can be used on following devices:
 * - KuroBox Pro
 * - Buffalo Linkstation Pro (LS-GL)
 * - Buffalo Terastation Pro II/Live
 *
 * Copyright (C) 2016 Roger Shimizu <rogershimizu@gmail.com>
 *
 * Based on the code from:
 *
 * Copyright (C) 2008  Sylver Bruneau <sylver.bruneau@googlemail.com>
 * Copyright (C) 2007  Herbert Valerio Riedel <hvr@gnu.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/serial_reg.h>
#include <linux/io.h>
#include <linux/delay.h>
#include "kuroboxpro-common.h"

static int uart1_micon_read(void *base, unsigned char *buf, int count)
{
	int i;
	int timeout;

	for (i = 0; i < count; i++) {
		timeout = 10;

		while (!(readl(UART1_REG(LSR)) & UART_LSR_DR)) {
			if (--timeout == 0)
				break;
			udelay(1000);
		}

		if (timeout == 0)
			break;
		buf[i] = readl(UART1_REG(RX));
	}

	/* return read bytes */
	return i;
}

static int uart1_micon_write(void *base, const unsigned char *buf, int count)
{
	int i = 0;

	while (count--) {
		while (!(readl(UART1_REG(LSR)) & UART_LSR_THRE))
			barrier();
		writel(buf[i++], UART1_REG(TX));
	}

	return 0;
}

int uart1_micon_send(void *base, const unsigned char *data, int count)
{
	int i;
	unsigned char checksum = 0;
	unsigned char recv_buf[40];
	unsigned char send_buf[40];
	unsigned char correct_ack[3];
	int retry = 2;

	/* Generate checksum */
	for (i = 0; i < count; i++)
		checksum -=  data[i];

	do {
		/* Send data */
		uart1_micon_write(base, data, count);

		/* send checksum */
		uart1_micon_write(base, &checksum, 1);

		if (uart1_micon_read(base, recv_buf, sizeof(recv_buf)) <= 3) {
			printk(KERN_ERR ">%s: receive failed.\n", __func__);

			/* send preamble to clear the receive buffer */
			memset(&send_buf, 0xff, sizeof(send_buf));
			uart1_micon_write(base, send_buf, sizeof(send_buf));

			/* make dummy reads */
			mdelay(100);
			uart1_micon_read(base, recv_buf, sizeof(recv_buf));
		} else {
			/* Generate expected ack */
			correct_ack[0] = 0x01;
			correct_ack[1] = data[1];
			correct_ack[2] = 0x00;

			/* checksum Check */
			if ((recv_buf[0] + recv_buf[1] + recv_buf[2] +
			     recv_buf[3]) & 0xFF) {
				printk(KERN_ERR ">%s: Checksum Error : "
					"Received data[%02x, %02x, %02x, %02x]"
					"\n", __func__, recv_buf[0],
					recv_buf[1], recv_buf[2], recv_buf[3]);
			} else {
				/* Check Received Data */
				if (correct_ack[0] == recv_buf[0] &&
				    correct_ack[1] == recv_buf[1] &&
				    correct_ack[2] == recv_buf[2]) {
					/* Interval for next command */
					mdelay(10);

					/* Receive ACK */
					return 0;
				}
			}
			/* Received NAK or illegal Data */
			printk(KERN_ERR ">%s: Error : NAK or Illegal Data "
					"Received\n", __func__);
		}
	} while (retry--);

	/* Interval for next command */
	mdelay(10);

	return -1;
}
