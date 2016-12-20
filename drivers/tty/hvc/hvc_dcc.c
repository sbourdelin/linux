/* Copyright (c) 2010, 2014 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/serial_core.h>

#include <asm/dcc.h>
#include <asm/processor.h>

#include "hvc_console.h"

/* DCC Status Bits */
#define DCC_STATUS_RX		(1 << 30)
#define DCC_STATUS_TX		(1 << 29)

static int hvc_dcc_put_chars(uint32_t vt, const char *buf, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		while (__dcc_getstatus() & DCC_STATUS_TX)
			cpu_relax();

		__dcc_putchar(buf[i]);
	}

	return count;
}

static int hvc_dcc_get_chars(uint32_t vt, char *buf, int count)
{
	int i;

	for (i = 0; i < count; ++i)
		if (__dcc_getstatus() & DCC_STATUS_RX)
			buf[i] = __dcc_getchar();
		else
			break;

	return i;
}

static bool hvc_dcc_check(void)
{
	unsigned long time = jiffies + (HZ / 10);

	/* Write a test character to check if it is handled */
	__dcc_putchar('\n');

	while (time_is_after_jiffies(time)) {
		if (!(__dcc_getstatus() & DCC_STATUS_TX))
			return true;
	}

	return false;
}

static const struct hv_ops hvc_dcc_get_put_ops = {
	.get_chars = hvc_dcc_get_chars,
	.put_chars = hvc_dcc_put_chars,
};

static int __init hvc_dcc_console_init(void)
{
	int ret;

	if (!hvc_dcc_check())
		return -ENODEV;

	/* Returns -1 if error */
	ret = hvc_instantiate(0, 0, &hvc_dcc_get_put_ops);

	return ret < 0 ? -ENODEV : 0;
}
console_initcall(hvc_dcc_console_init);

static int __init hvc_dcc_init(void)
{
	struct hvc_struct *p;

	if (!hvc_dcc_check())
		return -ENODEV;

	p = hvc_alloc(0, 0, &hvc_dcc_get_put_ops, 128);

	return PTR_ERR_OR_ZERO(p);
}
device_initcall(hvc_dcc_init);

static int hvc_dcc_earlyputc(int c)
{
	unsigned long count = 0xFFFFFFFF;
	static bool dead_dcc_earlycon;

	if (dead_dcc_earlycon)
		return -EBUSY;

	while (count--) {
		if (!(__dcc_getstatus() & DCC_STATUS_TX))
			break;
	}
	if (!count) {
		dead_dcc_earlycon = true;
		return -EBUSY;
	}
	__dcc_putchar(c);
	return 0;
}

static void hvc_dcc_earlywrite(struct console *con, const char *s,
			       unsigned int n)
{
	int r;

	while (n--) {
		r = hvc_dcc_earlyputc(*s);
		if (r)
			break;
		s++;
	}
}

static int
__init early_hvc_dcc_setup(struct earlycon_device *device, const char *opt)
{
	device->con->write = hvc_dcc_earlywrite;
	return 0;
}

EARLYCON_DECLARE(hvcdcc, early_hvc_dcc_setup);
