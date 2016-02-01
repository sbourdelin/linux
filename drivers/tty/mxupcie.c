/*
 *      mxupcie.c  -- MOXA Smartio/Industio MUE multiport serial driver.
 *
 *      Copyright (C) 1999-2015  Moxa Technologies (support@moxa.com).
 *	Copyright (C) 2016       Mathieu OTHACEHE <m.othacehe@gmail.com>
 *
 *      This code is based on the 1.16.7 moxa driver which is based on
 *	Linux serial driver.
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 */
#include <linux/version.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/serial_reg.h>
#include <linux/delay.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/irq.h>

#include "mxupcie.h"

enum {
	MXUPCIE_BOARD_CP102E = 0,
	MXUPCIE_BOARD_CP102EL,
	MXUPCIE_BOARD_CP132EL,
	MXUPCIE_BOARD_CP114EL,
	MXUPCIE_BOARD_CP104EL_A,
	MXUPCIE_BOARD_CP168EL_A,
	MXUPCIE_BOARD_CP118EL_A,
	MXUPCIE_BOARD_CP118E_A_I,
	MXUPCIE_BOARD_CP138E_A,
	MXUPCIE_BOARD_CP134EL_A,
	MXUPCIE_BOARD_CP116E_A_A,
	MXUPCIE_BOARD_CP116E_A_B
};

struct mxupcie_card_info {
	char *name;
	unsigned int nports;
	int flags;
};

static const struct mxupcie_card_info mxupcie_cards[] = {
	{"CP-102E series", 2, MX_FLAG_232},
	{"CP-102EL series", 2, MX_FLAG_232},
	{"CP-132EL series", 2, MX_FLAG_422 | MX_FLAG_485},
	{"CP-114EL series", 4, MX_FLAG_232 | MX_FLAG_422 | MX_FLAG_485},
	{"CP-104EL-A series", 4, MX_FLAG_232},
	{"CP-168EL-A series", 8, MX_FLAG_232},
	{"CP-118EL-A series", 8, MX_FLAG_232 | MX_FLAG_422 | MX_FLAG_485},
	{"CP-118E-A series", 8, MX_FLAG_422 | MX_FLAG_485},
	{"CP-138E-A series", 8, MX_FLAG_422 | MX_FLAG_485},
	{"CP-134EL-A series", 4, MX_FLAG_422 | MX_FLAG_485},
	{"CP-116E-A series (A)", 8, MX_FLAG_232 | MX_FLAG_422 | MX_FLAG_485},
	{"CP-116E-A series (B)", 8, MX_FLAG_232 | MX_FLAG_422 | MX_FLAG_485}
};

static struct pci_device_id mxupcie_pcibrds[] = {
	{PCI_VDEVICE(MOXA, PCI_DEVICE_ID_CP102E),  MXUPCIE_BOARD_CP102E},
	{PCI_VDEVICE(MOXA, PCI_DEVICE_ID_CP102EL), MXUPCIE_BOARD_CP102EL},
	{PCI_VDEVICE(MOXA, PCI_DEVICE_ID_CP132EL), MXUPCIE_BOARD_CP132EL},
	{PCI_VDEVICE(MOXA, PCI_DEVICE_ID_CP114EL), MXUPCIE_BOARD_CP114EL},
	{PCI_VDEVICE(MOXA, PCI_DEVICE_ID_CP104EL_A), MXUPCIE_BOARD_CP104EL_A},
	{PCI_VDEVICE(MOXA, PCI_DEVICE_ID_CP168EL_A), MXUPCIE_BOARD_CP168EL_A},
	{PCI_VDEVICE(MOXA, PCI_DEVICE_ID_CP118EL_A), MXUPCIE_BOARD_CP118EL_A},
	{PCI_VDEVICE(MOXA, PCI_DEVICE_ID_CP118E_A_I), MXUPCIE_BOARD_CP118E_A_I},
	{PCI_VDEVICE(MOXA, PCI_DEVICE_ID_CP138E_A), MXUPCIE_BOARD_CP138E_A},
	{PCI_VDEVICE(MOXA, PCI_DEVICE_ID_CP134EL_A), MXUPCIE_BOARD_CP134EL_A},
	{PCI_VDEVICE(MOXA, PCI_DEVICE_ID_CP116E_A_A), MXUPCIE_BOARD_CP116E_A_A},
	{PCI_VDEVICE(MOXA, PCI_DEVICE_ID_CP116E_A_B), MXUPCIE_BOARD_CP116E_A_B},
	{0}
};

MODULE_DEVICE_TABLE(pci, mxupcie_pcibrds);

static unsigned char interface;
module_param(interface, byte, 0);

struct mxupcie_port {
	struct tty_port port;
	struct mxupcie_board *board;
	int port_index;
	unsigned char *ioaddr;
	int baud_base;
	int read_status_mask;
	int custom_divisor;
	int close_delay;
	unsigned short closing_wait;
	int ier;
	int mcr;
	int xmit_head;
	int xmit_tail;
	int xmit_cnt;
	struct async_icount icount;
	int timeout;
	int max_baud;
	spinlock_t slock;
	int speed;
	int custom_baud_rate;
	unsigned char uir;
	unsigned long uir_addr;
};

struct mxupcie_board {
	int irq;
	unsigned int index;
	unsigned long iobar3_addr;
	const struct mxupcie_card_info *cinfo;
	struct mxupcie_port ports[MXUPCIE_PORTS_PER_BOARD];
};

static struct tty_driver *mx_drv;
struct mxupcie_board mxupcie_boards[MXUPCIE_BOARDS];

static void mxupcie_init_terminator(struct mxupcie_port *info)
{
	struct mxupcie_board *board = info->board;

	if (board->cinfo->flags & (MX_FLAG_422 | MX_FLAG_485) &&
	    board->cinfo->nports > 2) {
		outb(0xff, board->iobar3_addr + MOXA_PUART_GPIO_EN);
		outb(0x00, board->iobar3_addr + MOXA_PUART_GPIO_OUT);
	}
}

static int mxupcie_set_terminator(struct mxupcie_port *info, unsigned char val)
{
	unsigned char chip_val = 0;
	struct mxupcie_board *board = info->board;

	if (info->uir == MOXA_UIR_RS232)
		return -EINVAL;

	switch (val) {
	case MX_TERM_NONE:
	case MX_TERM_120:
		chip_val = inb(board->iobar3_addr + MOXA_PUART_GPIO_IN);
		if (board->cinfo->nports == 2) {
			chip_val &= ~(1 << (info->port_index + 2));
			chip_val |= (val << (info->port_index + 2));
		} else if (board->cinfo->nports > 2) {
			chip_val &= ~(1 << info->port_index);
			chip_val |= (val << info->port_index);
		}

		outb(0xff, board->iobar3_addr + MOXA_PUART_GPIO_EN);
		outb(chip_val, board->iobar3_addr + MOXA_PUART_GPIO_OUT);

		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int mxupcie_chars_in_buffer(struct tty_struct *tty)
{
	struct mxupcie_port *info = tty->driver_data;

	return info->xmit_cnt;
}

static void mxupcie_flush_buffer(struct tty_struct *tty)
{
	struct mxupcie_port *info = tty->driver_data;
	char fcr;
	int i;
	unsigned long sp_flags;

	spin_lock_irqsave(&info->slock, sp_flags);
	info->xmit_cnt = info->xmit_head = info->xmit_tail = 0;
	spin_unlock_irqrestore(&info->slock, sp_flags);

	/*
	 * TxFIFO has two pointer, w_ptr and r_ptr, but use the different clock.
	 * W_ptr uses pcie clock, and r_ptr uses uart clock.
	 * When set "TX FIFO Flush" bit,
	 * w_ptr will be clear to 0 first as pcie clock is faster.
	 * In this time, r_ptr is not clear to 0, so 795x will consider
	 * there are more data (w_ptr-r_ptr) need be transmitted.
	 *
	 * It is advised to reset 5 times or much more.
	 */
	fcr = ioread8(info->ioaddr + UART_FCR);

	for (i = 0; i < 5; i++) {
		iowrite8((fcr | UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT),
			     info->ioaddr + UART_FCR);
	}

	iowrite8(fcr, info->ioaddr + UART_FCR);

	tty_wakeup(tty);
}

/*
 * This routine is called to set the UART divisor registers to match
 * the specified baud rate for a serial port.
 */
static void write_div_scr(unsigned char *base,
			  signed short div,
			  unsigned char scr)
{
	unsigned char lcr;
	unsigned char oldsfr, sfr;

	oldsfr = ioread8(base + MOXA_PUART_SFR);
	sfr = oldsfr & (~(MOXA_SFR_ENABLE_TCNT));
	iowrite8(sfr, base + MOXA_PUART_SFR);
	iowrite8(scr, base + MOXA_PUART_SCR);
	iowrite8(oldsfr, base + MOXA_PUART_SFR);

	lcr = ioread8(base + UART_LCR);
	iowrite8(lcr | UART_LCR_DLAB, base + UART_LCR);
	iowrite8(div & 0xff, base + MOXA_PUART_LSB);
	iowrite8((div & 0xff00) >> 8, base + MOXA_PUART_MSB);
	iowrite8(lcr, base + UART_LCR);
}

static int set_linear_baud(unsigned char *base, long newspd)
{
	unsigned char scr, cpr;
	unsigned short div;
	int i, j, divisor = 0, sequence = 0;
	int M, N = 0, SCR = 0;
	int set_value, min, ret_value, accuracy;

	M = MIN_CPRM;
	set_value = newspd;

	min = FREQUENCY;
	ret_value = 0;

	for (i = 1; i <= MAXDIVISOR; i++) {
		for (j = MINSEQUENCE; j <= MAXSEQUENCE;) {
			if (FREQUENCY / (i * j) > set_value)
				accuracy = (FREQUENCY / (i * j)) - set_value;
			else
				accuracy = set_value - (FREQUENCY / (i * j));

			if (min > accuracy) {
				min = accuracy;
				ret_value = (FREQUENCY / (i * j));
				divisor = i;
				sequence = j;
			}

			if (j <= MAXSEQUENCE / 2)
				j += 1;
			else
				j += 2;
		}
	}
	if ((min * 100) / (set_value * 100) <= 3) {
		if (sequence > (MAXSEQUENCE / 2)) {
			M = MAX_CPRM;
			sequence /= 2;
		}
		for (i = MAX_SCR; i >= MIN_SCR; i--) {
			for (j = MIN_CPRN; j <= MAX_CPRN; j++) {
				if ((16 - i + j) == sequence) {
					SCR = i;
					N = j;
				}
			}
		}
	}
	scr = (unsigned char)SCR;
	div = (unsigned short)divisor;
	cpr = (M << 3) + N;

	iowrite8(ioread8(base + UART_MCR) | UART_MCR_CLKSEL, base + UART_MCR);
	iowrite8(cpr, base + MOXA_PUART_CPR);
	write_div_scr(base, div, scr);

	return 0;
}

static int mxupcie_set_baud(struct tty_struct *tty, long newspd)
{
	int quot = 0;
	unsigned char cval;
	int custom = 0;
	struct mxupcie_port *info = tty->driver_data;

	if (newspd > info->max_baud)
		return 0;

	if (newspd == 38400 &&
		(info->port.flags & ASYNC_SPD_MASK) == ASYNC_SPD_CUST) {
		info->speed = info->custom_baud_rate;
		custom = 1;

		quot = info->baud_base / info->speed;
		if (info->speed <= 0 || info->speed > info->max_baud)
			quot = 0;
		else
			set_linear_baud(info->ioaddr, info->speed);
	} else {
		if (newspd == 134) {
			quot = (2 * info->baud_base / 269);
			info->speed = 134;
		} else if (newspd) {
			quot = info->baud_base / newspd;
			info->baud_base = 921600;
			if (quot == 0)
				quot = 1;
		} else {
			quot = 0;
		}
	}

	info->timeout = ((MX_TX_FIFO_SIZE * HZ * 10 * quot) / info->baud_base);
	info->timeout += HZ / 50;		/* Add .02 seconds of slop */

	if (quot) {
		info->mcr |= UART_MCR_DTR;
		iowrite8(info->mcr, info->ioaddr + UART_MCR);
	} else {
		info->mcr &= ~UART_MCR_DTR;
		iowrite8(info->mcr, info->ioaddr + UART_MCR);

		return 0;
	}

	if (!custom) {
		cval = ioread8(info->ioaddr + UART_LCR);
		/* set DLAB */
		iowrite8(cval | UART_LCR_DLAB, info->ioaddr + UART_LCR);
		/* LS of divisor */
		iowrite8(quot & 0xff, info->ioaddr + UART_DLL);
		/* MS of divisor */
		iowrite8(quot >> 8, info->ioaddr + UART_DLM);
		/* reset DLAB */
		iowrite8(cval, info->ioaddr + UART_LCR);
		iowrite8(0x08, info->ioaddr + MOXA_PUART_CPR);
	}

	return 0;
}

static int mxupcie_change_speed(struct tty_struct *tty,
				struct ktermios *old_termios)
{
	struct mxupcie_port *info = tty->driver_data;
	unsigned cval, fcr;
	int reg_flag;

	mxupcie_set_baud(tty, tty_get_baud_rate(tty));

	/* byte size and parity */
	switch (C_CSIZE(tty)) {
	case CS5:
		cval = 0x00;
		break;
	case CS6:
		cval = 0x01;
		break;
	case CS7:
		cval = 0x02;
		break;
	case CS8:
		cval = 0x03;
		break;
	default:
		cval = 0x00;
		break;	/* too keep GCC shut... */
	}

	if (C_CSTOPB(tty))
		cval |= 0x04;

	if (C_PARENB(tty))
		cval |= UART_LCR_PARITY;

	if (!C_PARODD(tty))
		cval |= UART_LCR_EPAR;

	fcr = UART_FCR_ENABLE_FIFO;

	/* CTS flow control flag and modem status interrupts */
	info->ier &= ~UART_IER_MSI;

	reg_flag = ioread8(info->ioaddr + MOXA_PUART_EFR);

	if (C_CRTSCTS(tty)) {
		info->ier |= UART_IER_MSI;
		reg_flag |= (MOXA_EFR_AUTO_RTS | MOXA_EFR_AUTO_CTS);
	} else {
		reg_flag &= ~(MOXA_EFR_AUTO_RTS | MOXA_EFR_AUTO_CTS);
	}

	iowrite8(info->mcr, info->ioaddr + UART_MCR);

	if (C_CLOCAL(tty)) {
		info->port.flags &= ~ASYNC_CHECK_CD;
	} else {
		info->port.flags |= ASYNC_CHECK_CD;
		info->ier |= UART_IER_MSI;
	}

	iowrite8(info->ier, info->ioaddr + UART_IER);

	/* Set up parity check flag */
	info->read_status_mask = UART_LSR_OE | UART_LSR_THRE | UART_LSR_DR;

	if (I_INPCK(tty))
		info->read_status_mask |= UART_LSR_FE | UART_LSR_PE;

	if (I_BRKINT(tty) || I_PARMRK(tty))
		info->read_status_mask |= UART_LSR_BI;

	if (I_IGNBRK(tty)) {
		info->read_status_mask |= UART_LSR_BI;
		if (I_IGNPAR(tty)) {
			info->read_status_mask |= UART_LSR_OE |
				UART_LSR_PE | UART_LSR_FE;
		}
	}

	iowrite8(START_CHAR(tty), info->ioaddr + MOXA_PUART_XON1);
	iowrite8(START_CHAR(tty), info->ioaddr + MOXA_PUART_XON2);
	iowrite8(STOP_CHAR(tty), info->ioaddr + MOXA_PUART_XOFF1);
	iowrite8(STOP_CHAR(tty), info->ioaddr + MOXA_PUART_XOFF2);

	if (I_IXON(tty))
		reg_flag |= MOXA_EFR_TX_SW;
	else
		reg_flag &= ~MOXA_EFR_TX_SW;

	if (I_IXOFF(tty))
		reg_flag |= MOXA_EFR_RX_SW;
	else
		reg_flag &= ~MOXA_EFR_RX_SW;

	iowrite8(reg_flag, info->ioaddr + MOXA_PUART_EFR);
	iowrite8(fcr, info->ioaddr + UART_FCR);
	iowrite8(cval, info->ioaddr + UART_LCR);

	return 0;
}

static int mxupcie_activate(struct tty_port *port, struct tty_struct *tty)
{
	struct mxupcie_port *info;
	unsigned long page;
	unsigned char reg_flag;
	unsigned long sp_flags;

	info = container_of(port, struct mxupcie_port, port);

	page = __get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	spin_lock_irqsave(&info->slock, sp_flags);

	if (info->port.xmit_buf)
		free_page(page);
	else
		info->port.xmit_buf = (unsigned char *)page;

	/*
	 * Clear the FIFO buffers and disable them
	 * (they will be reenabled in mxupcie_change_speed())
	 */
	iowrite8((UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT),
		     info->ioaddr + UART_FCR);

	/*
	 * At this point there's no way the LSR could still be 0xFF;
	 * if it is, then bail out, because there's likely no UART
	 * here.
	 */
	if (ioread8(info->ioaddr + UART_LSR) == 0xff) {
		set_bit(TTY_IO_ERROR, &tty->flags);
		goto err;
	}

	/* Clear the interrupt registers. */
	(void)ioread8(info->ioaddr + UART_LSR);
	(void)ioread8(info->ioaddr + UART_RX);
	(void)ioread8(info->ioaddr + UART_IIR);
	(void)ioread8(info->ioaddr + UART_MSR);

	/* Now, initialize the UART */
	iowrite8(UART_LCR_WLEN8, info->ioaddr + UART_LCR);	/* reset DLAB */
	info->mcr = UART_MCR_DTR | UART_MCR_RTS;
	iowrite8(info->mcr, info->ioaddr + UART_MCR);

	/* Initialize enhance mode register */
	reg_flag = MOXA_EFR_ENHANCE;
	iowrite8(reg_flag, info->ioaddr + MOXA_PUART_EFR);

	reg_flag = MOXA_SFR_950 | MOXA_SFR_ENABLE_TCNT;
	iowrite8(reg_flag, info->ioaddr + MOXA_PUART_SFR);

	iowrite8(MX_TX_FIFO_SIZE, info->ioaddr + MOXA_PUART_TTL);
	iowrite8(MOXA_RTL_96, info->ioaddr + MOXA_PUART_RTL);
	iowrite8(MOXA_FCL_16, info->ioaddr + MOXA_PUART_FCL);
	iowrite8(MOXA_FCH_110, info->ioaddr + MOXA_PUART_FCH);

	/* Finally, enable interrupts */
	info->ier = UART_IER_MSI | UART_IER_RLSI | UART_IER_RDI;
	iowrite8(info->ier, info->ioaddr + UART_IER);

	/* And clear the interrupt registers again for luck. */
	(void)ioread8(info->ioaddr + UART_LSR);
	(void)ioread8(info->ioaddr + UART_RX);
	(void)ioread8(info->ioaddr + UART_IIR);
	(void)ioread8(info->ioaddr + UART_MSR);

	info->xmit_cnt = info->xmit_head = info->xmit_tail = 0;
	mxupcie_change_speed(tty, NULL);
	spin_unlock_irqrestore(&info->slock, sp_flags);

	return 0;
err:
	spin_unlock_irqrestore(&info->slock, sp_flags);
	free_page(page);

	return 0;
}

static void mxupcie_shutdown(struct tty_port *port)
{
	struct mxupcie_port *info;
	unsigned char reg_flag;
	int i;
	unsigned long sp_flags;

	info = container_of(port, struct mxupcie_port, port);

	spin_lock_irqsave(&info->slock, sp_flags);

	wake_up_interruptible(&info->port.delta_msr_wait);

	if (info->port.xmit_buf) {
		free_page((unsigned long)info->port.xmit_buf);
		info->port.xmit_buf = NULL;
	}

	reg_flag = 0;
	iowrite8(reg_flag, info->ioaddr + MOXA_PUART_EFR);
	iowrite8(reg_flag, info->ioaddr + MOXA_PUART_SFR);

	info->ier = 0;
	iowrite8(0x00, info->ioaddr + UART_IER);

	if (info->speed < 9600) {
		int sleep_interval = 0;
		int reset_cnt = 0;

		if (info->speed <= 600) {
			sleep_interval = 10;
			reset_cnt = MX_FIFO_RESET_CNT;
		} else {
			sleep_interval = 1;
			reset_cnt = MX_FIFO_RESET_CNT / 10;
		}

		/* Workaround for clear FIFO in low baudrate */
		iowrite8(0x0f, info->ioaddr + MOXA_PUART_ADJ_CLK);
		iowrite8(0x03, info->ioaddr + MOXA_PUART_ADJ_ENABLE);

		/* clear Rx/Tx FIFO's */
		for (i = 0; i < reset_cnt; i++) {
			iowrite8((UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT),
				 info->ioaddr + UART_FCR);
			msleep(sleep_interval);
		}

		iowrite8(0x00, info->ioaddr + MOXA_PUART_ADJ_CLK);
		iowrite8(0x02, info->ioaddr + MOXA_PUART_ADJ_ENABLE);
	} else {
		/* clear Rx/Tx FIFO's */
		iowrite8((UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT),
			     info->ioaddr + UART_FCR);
	}
	/* read data port to reset things */
	(void)ioread8(info->ioaddr + UART_RX);

	spin_unlock_irqrestore(&info->slock, sp_flags);
	clear_bit(ASYNCB_INITIALIZED, &port->flags);
}

static int mxupcie_open(struct tty_struct *tty, struct file *filp)
{
	struct mxupcie_port *info;
	struct mxupcie_board *board;
	int line;

	line = tty->index;
	if (line == MXUPCIE_PORTS)
		return 0;

	if ((line < 0) || (line > MXUPCIE_PORTS))
		return -ENODEV;

	board = &mxupcie_boards[line / MXUPCIE_PORTS_PER_BOARD];
	info = &board->ports[line % MXUPCIE_PORTS_PER_BOARD];

	tty->driver_data = info;

	return tty_port_open(&info->port, tty, filp);
}

static void mxupcie_close_port(struct tty_port *port)
{
	struct mxupcie_port *info;
	unsigned char reg_flag;
	unsigned long timeout;

	info = container_of(port, struct mxupcie_port, port);

	/*
	 * Now we wait for the transmit buffer to clear; and we notify
	 * the line discipline to only process XON/XOFF characters.
	 */
	reg_flag = ioread8(info->ioaddr + MOXA_PUART_EFR);
	reg_flag &= ~MOXA_EFR_AUTO_RTS;
	iowrite8(reg_flag, info->ioaddr + MOXA_PUART_EFR);

	/*
	 * At this point we stop accepting input.  To do this, we
	 * disable the receive line status interrupts, and tell the
	 * interrupt driver to stop checking the data ready bit in the
	 * line status register.
	 */
	info->ier &= ~UART_IER_RLSI;
	iowrite8(info->ier, info->ioaddr + UART_IER);

	/*
	 * Before we drop DTR, make sure the UART transmitter
	 * has completely drained; this is especially
	 * important if there is a transmit FIFO!
	 */
	timeout = jiffies + HZ;
	while (!(ioread8(info->ioaddr + UART_LSR) & UART_LSR_TEMT)) {
		schedule_timeout_interruptible(5);
		if (time_after(jiffies, timeout))
			break;
	}
}

static void mxupcie_close(struct tty_struct *tty, struct file *filp)
{
	struct mxupcie_port *info = tty->driver_data;
	struct tty_port *port = &info->port;

	if (tty->index == MXUPCIE_PORTS || info == NULL)
		return;
	if (tty_port_close_start(port, tty, filp) == 0)
		return;

	mutex_lock(&port->mutex);
	mxupcie_close_port(port);
	mxupcie_flush_buffer(tty);
	if (test_bit(ASYNCB_INITIALIZED, &port->flags)) {
		if (tty->termios.c_cflag & HUPCL)
			tty_port_lower_dtr_rts(port);
	}
	mxupcie_shutdown(port);
	set_bit(TTY_IO_ERROR, &tty->flags);
	mutex_unlock(&port->mutex);
	tty_port_close_end(port, tty);
	tty_port_tty_set(port, NULL);
}

static int mxupcie_write(struct tty_struct *tty,
			 const unsigned char *buf, int count)
{
	int c, total = 0;
	struct mxupcie_port *info = tty->driver_data;
	unsigned long sp_flags;

	if (!info->port.xmit_buf)
		return 0;

	while (1) {
		c = min_t(int, count, min(SERIAL_XMIT_SIZE - info->xmit_cnt - 1,
					  SERIAL_XMIT_SIZE - info->xmit_head));
		if (c <= 0)
			break;

		memcpy(info->port.xmit_buf + info->xmit_head, buf, c);
		spin_lock_irqsave(&info->slock, sp_flags);
		info->xmit_head = (info->xmit_head + c) &
					(SERIAL_XMIT_SIZE - 1);
		info->xmit_cnt += c;
		spin_unlock_irqrestore(&info->slock, sp_flags);

		buf += c;
		count -= c;
		total += c;
	}

	if (info->xmit_cnt && !tty->stopped) {
		spin_lock_irqsave(&info->slock, sp_flags);
		info->ier &= ~UART_IER_THRI;
		iowrite8(info->ier, info->ioaddr + UART_IER);
		info->ier |= UART_IER_THRI;
		iowrite8(info->ier, info->ioaddr + UART_IER);
		spin_unlock_irqrestore(&info->slock, sp_flags);
	}

	return total;
}

static int mxupcie_put_char(struct tty_struct *tty, unsigned char ch)
{
	struct mxupcie_port *info = tty->driver_data;
	unsigned long sp_flags;

	if (!info->port.xmit_buf)
		return 0;

	if (info->xmit_cnt >= SERIAL_XMIT_SIZE - 1)
		return 0;

	spin_lock_irqsave(&info->slock, sp_flags);
	info->port.xmit_buf[info->xmit_head++] = ch;
	info->xmit_head &= SERIAL_XMIT_SIZE - 1;
	info->xmit_cnt++;
	spin_unlock_irqrestore(&info->slock, sp_flags);

	if (!tty->stopped && !tty->hw_stopped) {
		spin_lock_irqsave(&info->slock, sp_flags);
		info->ier &= ~UART_IER_THRI;
		iowrite8(info->ier, info->ioaddr + UART_IER);
		info->ier |= UART_IER_THRI;
		iowrite8(info->ier, info->ioaddr + UART_IER);
		spin_unlock_irqrestore(&info->slock, sp_flags);
	}

	return 1;
}


static void mxupcie_flush_chars(struct tty_struct *tty)
{
	struct mxupcie_port *info = tty->driver_data;
	unsigned long sp_flags;

	if (info->xmit_cnt <= 0 || tty->stopped || !info->port.xmit_buf)
		return;

	spin_lock_irqsave(&info->slock, sp_flags);
	info->ier &= ~UART_IER_THRI;
	iowrite8(info->ier, info->ioaddr + UART_IER);
	info->ier |= UART_IER_THRI;
	iowrite8(info->ier, info->ioaddr + UART_IER);
	spin_unlock_irqrestore(&info->slock, sp_flags);
}


static int mxupcie_write_room(struct tty_struct *tty)
{
	struct mxupcie_port *info = tty->driver_data;
	int ret;

	ret = SERIAL_XMIT_SIZE - info->xmit_cnt - 1;
	if (ret < 0)
		ret = 0;

	return ret;
}

static int mxupcie_get_serial_info(struct tty_struct *tty,
				   struct serial_struct *retinfo)
{
	struct serial_struct tmp;
	struct mxupcie_port *info = tty->driver_data;

	memset(&tmp, 0, sizeof(tmp));
	tmp.line = tty->index;
	tmp.port = *info->ioaddr;
	tmp.irq = info->board->irq;
	tmp.flags = info->port.flags;
	tmp.baud_base = info->baud_base;
	tmp.close_delay = info->close_delay;
	tmp.closing_wait = info->closing_wait;
	tmp.custom_divisor = info->custom_divisor;
	tmp.hub6 = 0;

	if (copy_to_user(retinfo, &tmp, sizeof(*retinfo)))
		return -EFAULT;

	return 0;
}

static int mxupcie_set_serial_info(struct tty_struct *tty,
				   struct serial_struct *new_info)
{
	struct mxupcie_port *info = tty->driver_data;
	struct tty_port *port = &info->port;
	struct serial_struct new_serial;
	unsigned int flags;
	int retval = 0;
	unsigned long sp_flags;

	if (!new_info)
		return -EFAULT;

	if (copy_from_user(&new_serial, new_info, sizeof(new_serial)))
		return -EFAULT;

	if ((new_serial.irq != info->board->irq) ||
	    (new_serial.port != *info->ioaddr))
		return -EINVAL;

	flags = port->flags & ASYNC_SPD_MASK;

	if (!capable(CAP_SYS_ADMIN)) {
		if ((new_serial.baud_base != info->baud_base) ||
		    (new_serial.close_delay != info->close_delay) ||
		    ((new_serial.flags & ~ASYNC_USR_MASK) !=
		     (port->flags & ~ASYNC_USR_MASK)))
			return -EPERM;

		port->flags = ((port->flags & ~ASYNC_USR_MASK) |
				    (new_serial.flags & ASYNC_USR_MASK));
	} else {
		/*
		 * OK, past this point, all the error checking has been done.
		 * At this point, we start making changes.....
		 */
		port->flags = ((port->flags & ~ASYNC_FLAGS) |
			       (new_serial.flags & ASYNC_FLAGS));
		info->close_delay = new_serial.close_delay * HZ / 100;
		info->closing_wait = new_serial.closing_wait * HZ / 100;

		if ((new_serial.baud_base != info->baud_base) ||
		    (new_serial.custom_divisor != info->custom_divisor)) {

			if (new_serial.custom_divisor == 0)
				return -EINVAL;

			info->custom_baud_rate = new_serial.baud_base /
				new_serial.custom_divisor;
		}
	}

	if (test_bit(ASYNCB_INITIALIZED, &port->flags)) {
		if (flags != (port->flags & ASYNC_SPD_MASK)) {
			spin_lock_irqsave(&info->slock, sp_flags);
			mxupcie_change_speed(tty, NULL);
			spin_unlock_irqrestore(&info->slock, sp_flags);
		}
	} else {
		retval = mxupcie_activate(port, tty);
		if (retval == 0)
			set_bit(ASYNCB_INITIALIZED, &port->flags);
	}

	return retval;
}

/*
 * mx_get_lsr_info - get line status register info
 *
 * Purpose: Let user call ioctl() to get info when the UART physically
 *	    is emptied.  On bus types like RS485, the transmitter must
 *	    release the bus after transmitting. This must be done when
 *	    the transmit shift register is empty, not be done when the
 *	    transmit holding register is empty.  This functionality
 *	    allows an RS485 driver to be written in user space.
 */
static int mxupcie_get_lsr_info(struct tty_struct *tty, unsigned int *value)
{
	struct mxupcie_port *info = tty->driver_data;
	unsigned char status;
	unsigned int result;
	unsigned long sp_flags;

	spin_lock_irqsave(&info->slock, sp_flags);
	status = ioread8(info->ioaddr + UART_LSR);
	spin_unlock_irqrestore(&info->slock, sp_flags);
	result = ((status & UART_LSR_TEMT) ? TIOCSER_TEMT : 0);
	put_user(result, value);

	return 0;
}

static void mxupcie_software_break_signal(struct tty_struct *tty,
					  unsigned char state)
{
	struct mxupcie_port *info = tty->driver_data;
	unsigned char cval, reg_flag;
	unsigned char tx_byte = 0x01;
	int origin_speed;

	origin_speed = info->speed;

	if (state == MX_BREAK_ON) {
		cval = ioread8(info->ioaddr + UART_LCR);
		iowrite8(cval | UART_LCR_DLAB, info->ioaddr + UART_LCR);
		iowrite8(0, info->ioaddr + UART_DLL);
		iowrite8(0, info->ioaddr + UART_DLM);
		iowrite8(cval, info->ioaddr + UART_LCR);

		memcpy(info->ioaddr + MOXA_PUART_MEMTHR, &tx_byte, 1);

		reg_flag = ioread8(info->ioaddr + MOXA_PUART_SFR);
		reg_flag |= MOXA_SFR_FORCE_TX;
		iowrite8(reg_flag, info->ioaddr + MOXA_PUART_SFR);

		iowrite8(ioread8(info->ioaddr + UART_LCR) | UART_LCR_SBC,
			     info->ioaddr + UART_LCR);
	}

	if (state == MX_BREAK_OFF) {
		iowrite8(ioread8(info->ioaddr + UART_LCR) & ~UART_LCR_SBC,
			     info->ioaddr + UART_LCR);

		reg_flag = ioread8(info->ioaddr + MOXA_PUART_SFR);
		reg_flag &= ~MOXA_SFR_FORCE_TX;
		iowrite8(reg_flag, info->ioaddr + MOXA_PUART_SFR);

		iowrite8(UART_FCR_CLEAR_XMIT, info->ioaddr + UART_FCR);

		mxupcie_set_baud(tty, origin_speed);
	}
}

static void mxupcie_send_break(struct tty_struct *tty, int duration)
{
	struct mxupcie_port *info = tty->driver_data;
	unsigned long sp_flags;

	set_current_state(TASK_INTERRUPTIBLE);

	spin_lock_irqsave(&info->slock, sp_flags);
	switch (info->uir) {
	case MOXA_UIR_RS485_4W:
	case MOXA_UIR_RS485_2W:
		mxupcie_software_break_signal(tty, MX_BREAK_ON);
		break;
	case MOXA_UIR_RS232:
	case MOXA_UIR_RS422:
		iowrite8(ioread8(info->ioaddr + UART_LCR) | UART_LCR_SBC,
			     info->ioaddr + UART_LCR);
		break;
	}
	spin_unlock_irqrestore(&info->slock, sp_flags);

	schedule_timeout(duration);

	spin_lock_irqsave(&info->slock, sp_flags);
	switch (info->uir) {
	case MOXA_UIR_RS485_4W:
	case MOXA_UIR_RS485_2W:
		mxupcie_software_break_signal(tty, MX_BREAK_OFF);
		break;
	case MOXA_UIR_RS232:
	case MOXA_UIR_RS422:
		iowrite8(ioread8(info->ioaddr + UART_LCR) & ~UART_LCR_SBC,
			     info->ioaddr + UART_LCR);
		break;
	}
	spin_unlock_irqrestore(&info->slock, sp_flags);

	set_current_state(TASK_RUNNING);
}

static int mxupcie_set_interface(struct mxupcie_port *info, unsigned char val)
{
	struct mxupcie_board *board = info->board;
	unsigned char intf = 0, chip_val = 0;
	int ret;

	switch (val) {
	case MOXA_UIR_RS232:
		if (!(board->cinfo->flags & MX_FLAG_232))
			return -EINVAL;
		ret = mxupcie_set_terminator(info, MX_TERM_NONE);
		if (ret < 0)
			return -EINVAL;
	case MOXA_UIR_RS422:
	case MOXA_UIR_RS485_4W:
	case MOXA_UIR_RS485_2W:
		if (board->cinfo->flags & (MX_FLAG_422 | MX_FLAG_485)) {
			info->uir = val;
			chip_val = inb(info->uir_addr);

			if (info->port_index % 2) {
				intf = val << MOXA_UIR_EVEN_PORT_VALUE_OFFSET;
				chip_val &= 0x0F;
				chip_val |= intf;

			} else {
				intf = val;
				chip_val &= 0xF0;
				chip_val |= intf;
			}

			outb(chip_val, info->uir_addr);
		} else {
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}

	return 0;
}

static int mxupcie_cflags_changed(struct mxupcie_port *info, unsigned long arg,
				  struct async_icount *cprev)
{
	struct async_icount cnow;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&info->slock, flags);
	cnow = info->icount;
	spin_unlock_irqrestore(&info->slock, flags);

	ret = ((arg & TIOCM_RNG) && (cnow.rng != cprev->rng)) ||
	      ((arg & TIOCM_DSR) && (cnow.dsr != cprev->dsr)) ||
	      ((arg & TIOCM_CD)  && (cnow.dcd != cprev->dcd)) ||
	      ((arg & TIOCM_CTS) && (cnow.cts != cprev->cts));

	*cprev = cnow;

	return ret;
}

static int mxupcie_ioctl(struct tty_struct *tty, unsigned int cmd,
			 unsigned long arg)
{
	struct mxupcie_port *info = tty->driver_data;
	int retval;
	struct async_icount cnow;
	int ret = 0;
	unsigned long sp_flags;

	switch (cmd) {
	case TCSBRK:
		retval = tty_check_change(tty);
		if (retval)
			return retval;

		tty_wait_until_sent(tty, 0);

		if (!arg)
			mxupcie_send_break(tty, HZ / 4);

		break;
	case TCSBRKP:
		retval = tty_check_change(tty);
		if (retval)
			return retval;

		tty_wait_until_sent(tty, 0);
		mxupcie_send_break(tty, arg ? arg * (HZ / 10) : HZ / 4);

		break;
	case TIOCGSERIAL:
		ret = mxupcie_get_serial_info(tty,
					(struct serial_struct __user *)arg);
		break;
	case TIOCSSERIAL:
		ret = mxupcie_set_serial_info(tty,
					(struct serial_struct __user *)arg);
		break;
	case TIOCSERGETLSR:
		ret = mxupcie_get_lsr_info(tty, (unsigned int __user *)arg);
		break;
	case TIOCMIWAIT:
		spin_lock_irqsave(&info->slock, sp_flags);
		cnow = info->icount;
		spin_unlock_irqrestore(&info->slock, sp_flags);
		ret = wait_event_interruptible(info->port.delta_msr_wait,
				mxupcie_cflags_changed(info, arg, &cnow));
		break;
	default:
		ret = -ENOIOCTLCMD;
	}

	return ret;
}

static void mxupcie_throttle(struct tty_struct *tty)
{
	struct mxupcie_port *info = tty->driver_data;
	unsigned long sp_flags;

	spin_lock_irqsave(&info->slock, sp_flags);
	info->ier &= ~UART_IER_RDI;
	iowrite8(info->ier, info->ioaddr + UART_IER);
	spin_unlock_irqrestore(&info->slock, sp_flags);
}

static void mxupcie_unthrottle(struct tty_struct *tty)
{
	struct mxupcie_port *info = tty->driver_data;
	unsigned long sp_flags;

	spin_lock_irqsave(&info->slock, sp_flags);
	info->ier |= UART_IER_RDI;
	iowrite8(info->ier, info->ioaddr + UART_IER);
	spin_unlock_irqrestore(&info->slock, sp_flags);
}

static void mxupcie_stop(struct tty_struct *tty)
{
	struct mxupcie_port *info = tty->driver_data;
	unsigned long sp_flags;

	spin_lock_irqsave(&info->slock, sp_flags);
	if (info->ier & UART_IER_THRI) {
		info->ier &= ~UART_IER_THRI;
		iowrite8(info->ier, info->ioaddr + UART_IER);
	}
	spin_unlock_irqrestore(&info->slock, sp_flags);
}

static void mxupcie_start(struct tty_struct *tty)
{
	struct mxupcie_port *info = tty->driver_data;
	unsigned long sp_flags;

	spin_lock_irqsave(&info->slock, sp_flags);
	if (info->xmit_cnt && info->port.xmit_buf) {
		info->ier &= ~UART_IER_THRI;
		iowrite8(info->ier, info->ioaddr + UART_IER);
		info->ier |= UART_IER_THRI;
		iowrite8(info->ier, info->ioaddr + UART_IER);
	}
	spin_unlock_irqrestore(&info->slock, sp_flags);
}

static void mxupcie_set_termios(struct tty_struct *tty,
				struct ktermios *old_termios)
{
	struct mxupcie_port *info = tty->driver_data;
	unsigned long sp_flags;

	spin_lock_irqsave(&info->slock, sp_flags);
	mxupcie_change_speed(tty, old_termios);
	spin_unlock_irqrestore(&info->slock, sp_flags);

	if (old_termios &&
	    !tty_termios_hw_change(&tty->termios, old_termios) &&
	    tty->termios.c_iflag == old_termios->c_iflag) {
		dev_dbg(tty->dev, "%s - nothing to change\n", __func__);
		return;
	}

	if ((old_termios->c_cflag & CRTSCTS) &&
	    !(tty->termios.c_cflag & CRTSCTS)) {
		tty->hw_stopped = 0;
		mxupcie_start(tty);
	}

	if ((old_termios->c_iflag & IXON) &&
	    !(tty->termios.c_iflag & IXON)) {
		tty->stopped = 0;
		mxupcie_start(tty);
	}
}

void mxupcie_hangup(struct tty_struct *tty)
{
	struct mxupcie_port *info = tty->driver_data;

	mxupcie_flush_buffer(tty);
	tty_port_hangup(&info->port);
}

static void mxupcie_check_modem_status(struct tty_struct *tty,
				       int status)
{
	struct mxupcie_port *info = tty->driver_data;

	if (status & UART_MSR_TERI)
		info->icount.rng++;
	if (status & UART_MSR_DDSR)
		info->icount.dsr++;
	if (status & UART_MSR_DDCD)
		info->icount.dcd++;
	if (status & UART_MSR_DCTS)
		info->icount.cts++;

	wake_up_interruptible(&info->port.delta_msr_wait);

	if ((info->port.flags & ASYNC_CHECK_CD) && (status & UART_MSR_DDCD)) {
		if (status & UART_MSR_DCD)
			wake_up_interruptible(&info->port.open_wait);
		else
			tty_hangup(tty);
	}

	if (tty_port_cts_enabled(&info->port)) {
		if (tty->hw_stopped) {
			if (status & UART_MSR_CTS) {
				tty->hw_stopped = 0;
				tty_wakeup(tty);
			}
		} else {
			if (!(status & UART_MSR_CTS))
				tty->hw_stopped = 1;
		}
	}
}

static int mxupcie_tiocmget(struct tty_struct *tty)
{
	struct mxupcie_port *info;
	unsigned char control, status;
	unsigned long sp_flags;

	info = tty->driver_data;

	control = info->mcr;

	spin_lock_irqsave(&info->slock, sp_flags);
	status = ioread8(info->ioaddr + UART_MSR);
	spin_unlock_irqrestore(&info->slock, sp_flags);

	return	((control & UART_MCR_RTS) ? TIOCM_RTS : 0)  |
		((control & UART_MCR_DTR) ? TIOCM_DTR : 0)  |
		((status  & UART_MSR_DCD) ? TIOCM_CAR : 0)  |
		((status  & UART_MSR_RI)  ? TIOCM_RNG : 0)  |
		((status  & UART_MSR_DSR) ? TIOCM_DSR : 0)  |
		((status  & UART_MSR_CTS) ? TIOCM_CTS : 0);
}

static int mxupcie_tiocmset(struct tty_struct *tty,
			    unsigned int set, unsigned int clear)
{
	struct mxupcie_port *info = tty->driver_data;
	unsigned long sp_flags;

	spin_lock_irqsave(&info->slock, sp_flags);

	if (set & TIOCM_RTS)
		info->mcr |= UART_MCR_RTS;
	if (set & TIOCM_DTR)
		info->mcr |= UART_MCR_DTR;
	if (clear & TIOCM_RTS)
		info->mcr &= ~UART_MCR_RTS;
	if (clear & TIOCM_DTR)
		info->mcr &= ~UART_MCR_DTR;

	iowrite8(info->mcr, info->ioaddr + UART_MCR);
	spin_unlock_irqrestore(&info->slock, sp_flags);

	return 0;
}

static int mxupcie_rs_break(struct tty_struct *tty, int break_state)
{
	struct mxupcie_port *info = tty->driver_data;
	unsigned long sp_flags;
	int lcr;

	spin_lock_irqsave(&info->slock, sp_flags);
	if (break_state == -1) {
		switch (info->uir) {
		case MOXA_UIR_RS485_4W:
		case MOXA_UIR_RS485_2W:
			mxupcie_software_break_signal(tty, MX_BREAK_ON);
			break;
		case MOXA_UIR_RS232:
		case MOXA_UIR_RS422:
			lcr = ioread8(info->ioaddr + UART_LCR) | UART_LCR_SBC;
			iowrite8(lcr, info->ioaddr + UART_LCR);
			break;
		}
	} else {
		switch (info->uir) {
		case MOXA_UIR_RS485_4W:
		case MOXA_UIR_RS485_2W:
			mxupcie_software_break_signal(tty, MX_BREAK_OFF);
			break;
		case MOXA_UIR_RS232:
		case MOXA_UIR_RS422:
			lcr = ioread8(info->ioaddr + UART_LCR) & ~UART_LCR_SBC;
			iowrite8(lcr, info->ioaddr + UART_LCR);
			break;
		}
	}

	spin_unlock_irqrestore(&info->slock, sp_flags);
	return 0;
}

static void mxupcie_wait_until_sent(struct tty_struct *tty, int timeout)
{
	struct mxupcie_port *info = tty->driver_data;
	unsigned long sp_flags;
	unsigned long orig_jiffies, char_time;
	int lsr;

	orig_jiffies = jiffies;
	/*
	 * Set the check interval to be 1/5 of the estimated time to
	 * send a single character, and make it at least 1.  The check
	 * interval should also be less than the timeout.
	 *
	 * Note: we have to use pretty tight timings here to satisfy
	 * the NIST-PCTS.
	 */
	char_time = (info->timeout - HZ / 50) / MX_TX_FIFO_SIZE;
	char_time = char_time / 5;

	if (char_time == 0)
		char_time = 1;

	if (timeout && timeout < char_time)
		char_time = timeout;
	/*
	 * If the transmitter hasn't cleared in twice the approximate
	 * amount of time to send the entire FIFO, it probably won't
	 * ever clear.  This assumes the UART isn't doing flow
	 * control, which is currently the case.  Hence, if it ever
	 * takes longer than info->timeout, this is probably due to a
	 * UART bug of some kind.  So, we clamp the timeout parameter at
	 * 2*info->timeout.
	 */

	if (!timeout || timeout > 2 * info->timeout)
		timeout = 2 * info->timeout;

	dev_dbg(tty->dev, "%s(%d) - check=%lu", __func__, timeout, char_time);

	spin_lock_irqsave(&info->slock, sp_flags);
	while (!((lsr = ioread8(info->ioaddr + UART_LSR)) & UART_LSR_TEMT)) {
		spin_unlock_irqrestore(&info->slock, sp_flags);
		dev_dbg(tty->dev, "lsr = %d (jiff=%lu)", lsr, jiffies);
		schedule_timeout_interruptible(char_time);
		spin_lock_irqsave(&info->slock, sp_flags);

		if (signal_pending(current))
			break;

		if (timeout && time_after(jiffies, orig_jiffies + timeout))
			break;
	}
	spin_unlock_irqrestore(&info->slock, sp_flags);
	set_current_state(TASK_RUNNING);

	dev_dbg(tty->dev, "lsr = %d (jiff=%lu), done", lsr, jiffies);
}

static void mxupcie_rx_chars(struct tty_struct *tty,
			     int *status)
{
	struct mxupcie_port *info = tty->driver_data;
	unsigned char ch, gdl = 0;
	int cnt = 0;
	int recv_room;
	int max = 256;
	unsigned long flags;

	if (*status & UART_LSR_SPECIAL)
		goto intr_old;

	recv_room = tty_buffer_request_room(&info->port, MX_RX_FIFO_SIZE);
	if (recv_room) {
		gdl = ioread8(info->ioaddr + MOXA_PUART_RCNT);

		if (gdl > recv_room)
			gdl = recv_room;

		if (gdl) {
			tty_insert_flip_string(
				&info->port,
				info->ioaddr + MOXA_PUART_MEMRBR, gdl);

			cnt = gdl;
		}
	} else {
		set_bit(TTY_THROTTLED, &tty->flags);
	}

	goto end_intr;

intr_old:
	do {
		if (max-- < 0)
			break;

		ch = ioread8(info->ioaddr + UART_RX);

		if (*status & UART_LSR_SPECIAL) {
			if (*status & UART_LSR_BI) {
				flags = TTY_BREAK;
				info->icount.brk++;

				if (info->port.flags & ASYNC_SAK)
					do_SAK(tty);
			} else if (*status & UART_LSR_PE) {
				flags = TTY_PARITY;
				info->icount.parity++;
			} else if (*status & UART_LSR_FE) {
				flags = TTY_FRAME;
				info->icount.frame++;
			} else if (*status & UART_LSR_OE) {
				flags = TTY_OVERRUN;
				info->icount.overrun++;
			} else {
				flags = TTY_BREAK;
			}

		} else {
			flags = 0;
		}

		tty_insert_flip_char(&info->port, ch, flags);
		cnt++;

		*status = ioread8(info->ioaddr + UART_LSR);
	} while (*status & UART_LSR_DR);

end_intr:
	tty_flip_buffer_push(&info->port);

	info->icount.rx += cnt;
}

static void mxupcie_tx_chars(struct tty_struct *tty)
{
	struct mxupcie_port *info = tty->driver_data;
	int cnt;
	int tx_cnt;

	if (info->port.xmit_buf == NULL)
		return;

	if (info->xmit_cnt == 0 || tty->stopped) {
		dev_dbg(tty->dev, "%s: tty stopped\n", __func__);
		info->ier &= ~UART_IER_THRI;
		iowrite8(info->ier, info->ioaddr + UART_IER);
		return;
	}

	cnt = info->xmit_cnt;

	tx_cnt = MX_TX_FIFO_SIZE - ioread8(info->ioaddr + MOXA_PUART_TCNT);

	cnt = min(info->xmit_cnt,
		  min(tx_cnt, (int)(SERIAL_XMIT_SIZE - info->xmit_tail)));
	if (cnt) {
		memcpy(info->ioaddr + MOXA_PUART_MEMTHR,
		       info->port.xmit_buf + info->xmit_tail, cnt);
		info->xmit_tail += cnt;
		info->xmit_tail &= (SERIAL_XMIT_SIZE - 1);
		info->xmit_cnt -= cnt;
	}

	info->icount.tx += cnt;

	if (info->xmit_cnt < WAKEUP_CHARS)
		tty_wakeup(tty);
}

static int mxupcie_carrier_raised(struct tty_port *port)
{
	struct mxupcie_port *info;

	info = container_of(port, struct mxupcie_port, port);
	return (ioread8(info->ioaddr + UART_MSR) & UART_MSR_DCD) ? 1 : 0;
}

static void mxupcie_dtr_rts(struct tty_port *port, int raise)
{
	struct mxupcie_port *info;
	unsigned long sp_flags;
	int mcr;

	info = container_of(port, struct mxupcie_port, port);

	spin_lock_irqsave(&info->slock, sp_flags);
	mcr = ioread8(info->ioaddr + UART_MCR);
	if (raise)
		mcr |= UART_MCR_DTR | UART_MCR_RTS;
	else
		mcr &= ~(UART_MCR_DTR | UART_MCR_RTS);

	iowrite8(mcr, info->ioaddr + UART_MCR);
	spin_unlock_irqrestore(&info->slock, sp_flags);
}

static const struct tty_port_operations mxupcie_port_ops = {
	.carrier_raised = mxupcie_carrier_raised,
	.dtr_rts = mxupcie_dtr_rts,
	.activate = mxupcie_activate,
	.shutdown = mxupcie_shutdown
};

static irqreturn_t mxupcie_interrupt(int irq, void *dev_id)
{
	int lsr, iir, i;
	struct mxupcie_board *board;
	struct mxupcie_port *info;
	int max, msr;
	int pass_counter = 0;
	int int_cnt;
	int handled = 0;
	int vect_flag;
	struct tty_struct *tty;

	board = NULL;
	for (i = 0; i < MXUPCIE_BOARDS; i++) {
		if (dev_id == &mxupcie_boards[i]) {
			board = dev_id;
			break;
		}
	}

	if (i == MXUPCIE_BOARDS)
		goto irq_stop;

	if (board == NULL)
		goto irq_stop;

	max = board->cinfo->nports;
	pass_counter = 0;

	do {
		vect_flag = 0;

		for (i = 0; i < max; i++) {
			info = &board->ports[i];
			int_cnt = 0;

			do {
				iir = ioread8(info->ioaddr + UART_IIR);
				if (iir == MOXA_IIR_NO_INT) {
					vect_flag++;
					break;
				}

				tty = tty_port_tty_get(&info->port);
				if (!tty) {
					lsr = ioread8(info->ioaddr + UART_LSR);
					iowrite8(0x27, info->ioaddr + UART_FCR);
					ioread8(info->ioaddr + UART_MSR);
					break;
				}

				handled = 1;

				spin_lock(&info->slock);
				lsr = ioread8(info->ioaddr + UART_LSR);

				if (iir & MOXA_IIR_RDI) {
					lsr &= info->read_status_mask;
					if (lsr & UART_LSR_DR)
						mxupcie_rx_chars(tty, &lsr);
				}

				msr = ioread8(info->ioaddr + UART_MSR);
				if (msr & UART_MSR_ANY_DELTA)
					mxupcie_check_modem_status(tty, msr);

				if (iir & MOXA_IIR_THRI) {
					if (lsr & UART_LSR_THRE)
						mxupcie_tx_chars(tty);
				}

				spin_unlock(&info->slock);
				tty_kref_put(tty);
			} while (int_cnt++ < MXUPCIE_ISR_PASS_LIMIT);
		}

		if (vect_flag == max)
			break;

	} while (pass_counter++ < MXUPCIE_ISR_PASS_LIMIT);

irq_stop:
	return IRQ_RETVAL(handled);
}

int mxupcie_initbrd(struct mxupcie_board *board, struct pci_dev *pdev)
{
	int err;
	int i;
	struct mxupcie_port *info;
	unsigned long addr;
	int temp_interface = 0;

	for (i = 0; i < board->cinfo->nports; i++) {
		info = &board->ports[i];
		tty_port_init(&info->port);
		info->board = board;
		info->port_index = board->index + i;
		info->uir = 0;

		if (board->cinfo->nports == 4 && i == MX_PORT4)
			addr = board->iobar3_addr + MOXA_UIR_OFFSET + 3;
		else
			addr = board->iobar3_addr + MOXA_UIR_OFFSET + (i / 2);

		info->port.ops = &mxupcie_port_ops;
		info->uir_addr = addr;
		info->custom_divisor = info->baud_base * 16;
		info->close_delay = 5 * HZ / 10;
		info->closing_wait = 30 * HZ;
		info->speed = 9600;
		spin_lock_init(&info->slock);

		outb(MOXA_GPIO_SET_ALL_OUTPUT,
		     board->iobar3_addr + MOXA_PUART_GPIO_EN);

		mxupcie_init_terminator(info);

		if (!interface) {
			if (board->cinfo->flags & MX_FLAG_232)
				temp_interface = MOXA_UIR_RS232;
			else
				temp_interface = MOXA_UIR_RS422;
		} else {
			temp_interface = interface;
		}

		mxupcie_set_interface(info, temp_interface);

		/* before set INT ISR, disable all int */
		iowrite8(ioread8(info->ioaddr + UART_IER) & 0xf0,
			 info->ioaddr + UART_IER);
	}

	err = request_irq(board->irq, mxupcie_interrupt, IRQF_SHARED, "mxupcie",
			  board);
	if (err) {
		dev_err(&pdev->dev, "irq %d may be in conflict\n", board->irq);
		return err;
	}

	return 0;
}

static const struct tty_operations mxupcie_ops = {
	.open = mxupcie_open,
	.close = mxupcie_close,
	.write = mxupcie_write,
	.put_char = mxupcie_put_char,
	.flush_chars = mxupcie_flush_chars,
	.write_room = mxupcie_write_room,
	.chars_in_buffer = mxupcie_chars_in_buffer,
	.flush_buffer = mxupcie_flush_buffer,
	.ioctl = mxupcie_ioctl,
	.throttle = mxupcie_throttle,
	.unthrottle = mxupcie_unthrottle,
	.stop = mxupcie_stop,
	.start = mxupcie_start,
	.set_termios = mxupcie_set_termios,
	.hangup = mxupcie_hangup,
	.tiocmget = mxupcie_tiocmget,
	.tiocmset = mxupcie_tiocmset,
	.break_ctl = mxupcie_rs_break,
	.wait_until_sent = mxupcie_wait_until_sent,
};

static int mxupcie_pci_probe(struct pci_dev *pdev,
			     const struct pci_device_id *ent)
{
	struct mxupcie_board *board;
	unsigned char *ioaddress;
	unsigned long iobar_address;
	struct device *tty_dev;
	int i, err = 0;

	for (i = 0; i < MXUPCIE_BOARDS; i++) {
		if (!mxupcie_boards[i].cinfo)
			break;
	}

	if (i >= MXUPCIE_BOARDS) {
		dev_err(&pdev->dev, "too many boards found: %d >= %d\n",
			i, MXUPCIE_BOARDS);
		goto err;
	}

	board = &mxupcie_boards[i];
	board->index = i * MXUPCIE_PORTS_PER_BOARD;

	dev_info(&pdev->dev, "found MOXA %s board(busno=%d,devno=%d)\n",
		 mxupcie_cards[ent->driver_data].name,
		 pdev->bus->number, PCI_SLOT(pdev->devfn));

	memset(&board->ports, '\0', sizeof(board->ports));
	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "pci_enable_device fail\n");
		goto err;
	}

	/* io address */
	if (!request_mem_region(pci_resource_start(pdev, 1),
				pci_resource_len(pdev, 1),
				"mxupcie(MEM)")) {
		err = -EBUSY;
		goto err_dis;
	}

	ioaddress = ioremap(pci_resource_start(pdev, 1),
			pci_resource_len(pdev, 1));
	if (!ioaddress) {
		err = -ENOMEM;
		goto err_rel_mem;
	}

	iobar_address = pci_resource_start(pdev, 2);
	if (!request_region(pci_resource_start(pdev, 2),
			    pci_resource_len(pdev, 2),
			    "mxupcie(IOBAR3)")) {
		err = -EBUSY;
		goto err_unmap;
	}

	board->cinfo = &mxupcie_cards[ent->driver_data];
	for (i = 0; i < board->cinfo->nports; i++) {
		unsigned char *addr;

		board->ports[i].baud_base = 921600;
		board->ports[i].max_baud = 921600;

		if (board->cinfo->nports == 4 && i == MX_PORT4)
			addr = ioaddress + (MX_PORT8 * MX_PUART_SIZE);
		else
			addr = ioaddress + (i * MX_PUART_SIZE);

		board->ports[i].ioaddr = addr;
	}

	board->irq = pdev->irq;
	board->iobar3_addr = iobar_address;

	err = mxupcie_initbrd(board, pdev);
	if (err)
		goto err_zero;

	for (i = 0; i < board->cinfo->nports; i++) {
		dev_info(&pdev->dev, "register ttyMUE%d\n", board->index + i);
		tty_dev = tty_port_register_device(&board->ports[i].port,
						   mx_drv,
						   board->index + i,
						   &pdev->dev);
		if (IS_ERR(tty_dev)) {
			err = PTR_ERR(tty_dev);
			for (; i > 0; i--)
				tty_unregister_device(mx_drv,
					board->index + i - 1);
			goto err_relboard;
		}
	}

	pci_set_drvdata(pdev, board);

	return 0;

err_relboard:
	for (i = 0; i < board->cinfo->nports; i++)
		tty_port_destroy(&board->ports[i].port);
	free_irq(board->irq, board);
err_zero:
	board->cinfo = NULL;
	release_region(pci_resource_start(pdev, 2),
		       pci_resource_len(pdev, 2));
err_unmap:
	iounmap(ioaddress);
err_rel_mem:
	release_mem_region(pci_resource_start(pdev, 1),
			   pci_resource_len(pdev, 1));
err_dis:
	pci_disable_device(pdev);
err:
	return err;
}

static void mxupcie_pci_remove(struct pci_dev *pdev)
{
	struct mxupcie_board *board = pci_get_drvdata(pdev);
	int i;

	for (i = 0; i < board->cinfo->nports; i++) {
		tty_unregister_device(mx_drv, board->index + i);
		tty_port_destroy(&board->ports[i].port);
	}

	free_irq(board->irq, board);

	iounmap(board->ports[1].ioaddr);
	release_mem_region(pci_resource_start(pdev, 1),
			   pci_resource_len(pdev, 1));
	release_region(pci_resource_start(pdev, 2), pci_resource_len(pdev, 2));

	board->cinfo = NULL;
}

static struct pci_driver mxupcie_pci_driver = {
	.name = "mxupcie",
	.id_table = mxupcie_pcibrds,
	.probe = mxupcie_pci_probe,
	.remove = mxupcie_pci_remove
};

static int __init mxupcie_module_init(void)
{
	int err = 0;

	mx_drv = alloc_tty_driver(MXUPCIE_PORTS + 1);
	if (!mx_drv)
		return -ENOMEM;

	/* Initialize the tty_driver structure */
	mx_drv->name = "ttyMUE";
	mx_drv->major = 0;
	mx_drv->minor_start = 0;
	mx_drv->type = TTY_DRIVER_TYPE_SERIAL;
	mx_drv->subtype = SERIAL_TYPE_NORMAL;
	mx_drv->init_termios = tty_std_termios;
	mx_drv->init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	mx_drv->flags = TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV;
	tty_set_operations(mx_drv, &mxupcie_ops);

	err = tty_register_driver(mx_drv);
	if (err) {
		pr_err("failed to register tty driver: %d\n", err);
		goto err_put;
	}

	err = pci_register_driver(&mxupcie_pci_driver);
	if (err) {
		pr_err("failed to register pci driver: %d\n", err);
		goto err_unr;
	}

	return 0;
err_unr:
	tty_unregister_driver(mx_drv);
err_put:
	put_tty_driver(mx_drv);

	return err;
}

static void __exit mxupcie_module_exit(void)
{
	pci_unregister_driver(&mxupcie_pci_driver);

	tty_unregister_driver(mx_drv);
	put_tty_driver(mx_drv);
}

module_init(mxupcie_module_init);
module_exit(mxupcie_module_exit);

MODULE_AUTHOR("Mathieu OTHACEHE");
MODULE_DESCRIPTION("MOXA SmartIO MUE driver");
MODULE_LICENSE("GPL");
