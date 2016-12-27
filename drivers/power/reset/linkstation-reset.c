/*
 * Buffalo Linkstation power reset driver.
 * It may also be used on following devices:
 *  - KuroBox Pro
 *  - Buffalo Linkstation Pro (LS-GL)
 *  - Buffalo Terastation Pro II/Live
 *  - Buffalo Linkstation Duo (LS-WTGL)
 *  - Buffalo Linkstation Mini (LS-WSGL)
 *
 * Copyright (C) 2016  Roger Shimizu <rogershimizu@gmail.com>
 *
 * Based on the code from:
 *
 * Copyright (C) 2012  Andrew Lunn <andrew@lunn.ch>
 * Copyright (C) 2009  Martin Michlmayr <tbm@cyrius.com>
 * Copyright (C) 2008  Byron Bradley <byron.bbradley@gmail.com>
 * Copyright (C) 2008  Sylver Bruneau <sylver.bruneau@googlemail.com>
 * Copyright (C) 2007  Herbert Valerio Riedel <hvr@gnu.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/serial_reg.h>
#include <linux/kallsyms.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/delay.h>

#define UART1_REG(x)	(base + ((UART_##x) << 2))
#define MICON_CMD_SIZE	4

/* 4-byte magic hello command to UART1-attached microcontroller */
static const unsigned char linkstation_micon_magic[] = {
	0x1b,
	0x00,
	0x07,
	0x00
};

/* for each row, first byte is the size of command */
static const unsigned char linkstation_power_off_cmd[][MICON_CMD_SIZE] = {
	{ 3,	0x01, 0x35, 0x00},
	{ 2,	0x00, 0x0c},
	{ 2,	0x00, 0x06},
	{}
};

struct reset_cfg {
	u32 baud;
	const unsigned char *magic;
	const unsigned char (*cmd)[MICON_CMD_SIZE];
};

static const struct reset_cfg linkstation_power_off_cfg = {
	.baud = 38400,
	.magic = linkstation_micon_magic,
	.cmd = linkstation_power_off_cmd,
};

static const struct of_device_id linkstation_reset_of_match_table[] = {
	{ .compatible = "linkstation,power-off",
	  .data = &linkstation_power_off_cfg,
	},
	{}
};
MODULE_DEVICE_TABLE(of, linkstation_reset_of_match_table);

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

static void __iomem *base;
static unsigned long tclk;
static const struct reset_cfg *cfg;

static void linkstation_reset(void)
{
	const unsigned divisor = ((tclk + (8 * cfg->baud)) / (16 * cfg->baud));
	int i;

	pr_err("%s: triggering power-off...\n", __func__);

	/* hijack UART1 and reset into sane state */
	writel(0x83, UART1_REG(LCR));
	writel(divisor & 0xff, UART1_REG(DLL));
	writel((divisor >> 8) & 0xff, UART1_REG(DLM));
	writel(cfg->magic[0], UART1_REG(LCR));
	writel(cfg->magic[1], UART1_REG(IER));
	writel(cfg->magic[2], UART1_REG(FCR));
	writel(cfg->magic[3], UART1_REG(MCR));

	/* send the power-off command to PIC */
	for(i = 0; cfg->cmd[i][0] > 0; i ++) {
		/* [0] is size of the command; command starts from [1] */
		uart1_micon_send(base, &(cfg->cmd[i][1]), cfg->cmd[i][0]);
	}
}

static int linkstation_reset_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct resource *res;
	struct clk *clk;
	char symname[KSYM_NAME_LEN];

	const struct of_device_id *match =
		of_match_node(linkstation_reset_of_match_table, np);
	cfg = match->data;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "Missing resource");
		return -EINVAL;
	}

	base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!base) {
		dev_err(&pdev->dev, "Unable to map resource");
		return -EINVAL;
	}

	/* We need to know tclk in order to calculate the UART divisor */
	clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(clk)) {
		dev_err(&pdev->dev, "Clk missing");
		return PTR_ERR(clk);
	}

	tclk = clk_get_rate(clk);

	/* Check that nothing else has already setup a handler */
	if (pm_power_off) {
		lookup_symbol_name((ulong)pm_power_off, symname);
		dev_err(&pdev->dev,
			"pm_power_off already claimed %p %s",
			pm_power_off, symname);
		return -EBUSY;
	}
	pm_power_off = linkstation_reset;

	return 0;
}

static int linkstation_reset_remove(struct platform_device *pdev)
{
	pm_power_off = NULL;
	return 0;
}

static struct platform_driver linkstation_reset_driver = {
	.probe	= linkstation_reset_probe,
	.remove	= linkstation_reset_remove,
	.driver	= {
		.name	= "linkstation_reset",
		.of_match_table = of_match_ptr(linkstation_reset_of_match_table),
	},
};

module_platform_driver(linkstation_reset_driver);

MODULE_AUTHOR("Roger Shimizu <rogershimizu@gmail.com>");
MODULE_DESCRIPTION("Linkstation Reset driver");
MODULE_LICENSE("GPL v2");
