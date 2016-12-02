/*  Copyright 2016 National Instruments Corporation
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 *  more details.
 */
#include <linux/init.h>
#include <linux/console.h>
#include <linux/serial_reg.h>
#include <linux/io.h>

#define NI_UART0_REGS_BASE	((unsigned char __iomem *)0xbf380000)

static inline unsigned char serial_in(int offset)
{
	return __raw_readb(NI_UART0_REGS_BASE + offset);
}

static inline void serial_out(int offset, char value)
{
	__raw_writeb(value, NI_UART0_REGS_BASE + offset);
}

int prom_putchar(char c)
{
	while ((serial_in(UART_LSR) & UART_LSR_THRE) == 0)
		;

	serial_out(UART_TX, c);

	return 1;
}
