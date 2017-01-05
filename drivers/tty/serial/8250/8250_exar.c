/*
 *  Probe module for 8250/16550-type PCI serial ports.
 *
 *  Based on drivers/char/serial.c, by Linus Torvalds, Theodore Ts'o.
 *
 *  Copyright (C) 2001 Russell King, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License.
 */
#undef DEBUG
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/tty.h>
#include <linux/serial_reg.h>
#include <linux/serial_core.h>
#include <linux/8250_pci.h>
#include <linux/bitops.h>
#include <linux/io.h>

#include <asm/byteorder.h>

#include "8250.h"

/*
 * init function returns:
 *  > 0 - number of ports
 *  = 0 - use board->num_ports
 *  < 0 - error
 */
struct pci_serial_quirk {
	u32	vendor;
	u32	device;
	u32	subvendor;
	u32	subdevice;
	int	(*probe)(struct pci_dev *dev);
	int	(*init)(struct pci_dev *dev);
	int	(*setup)(struct serial_private *,
			 const struct pciserial_board *,
			 struct uart_8250_port *, int);
	void	(*exit)(struct pci_dev *dev);
};

#define PCI_NUM_BAR_RESOURCES	6

struct serial_private {
	struct pci_dev		*dev;
	unsigned int		nr;
	struct pci_serial_quirk	*quirk;
	int			line[0];
};

static int
setup_port(struct serial_private *priv, struct uart_8250_port *port,
	   int bar, int offset, int regshift)
{
	struct pci_dev *dev = priv->dev;

	if (bar >= PCI_NUM_BAR_RESOURCES)
		return -EINVAL;

	if (pci_resource_flags(dev, bar) & IORESOURCE_MEM) {
		if (!pcim_iomap(dev, bar, 0) && !pcim_iomap_table(dev))
			return -ENOMEM;

		port->port.iotype = UPIO_MEM;
		port->port.iobase = 0;
		port->port.mapbase = pci_resource_start(dev, bar) + offset;
		port->port.membase = pcim_iomap_table(dev)[bar] + offset;
		port->port.regshift = regshift;
	} else {
		port->port.iotype = UPIO_PORT;
		port->port.iobase = pci_resource_start(dev, bar) + offset;
		port->port.mapbase = 0;
		port->port.membase = NULL;
		port->port.regshift = 0;
	}
	return 0;
}

static int pci_default_setup(struct serial_private *priv,
			     const struct pciserial_board *board,
			     struct uart_8250_port *port, int idx)
{
	unsigned int bar, offset = board->first_offset, maxnr;

	bar = FL_GET_BASE(board->flags);
	if (board->flags & FL_BASE_BARS)
		bar += idx;
	else
		offset += idx * board->uart_offset;

	maxnr = (pci_resource_len(priv->dev, bar) - board->first_offset) >>
		(board->reg_shift + 3);

	if (board->flags & FL_REGION_SZ_CAP && idx >= maxnr)
		return 1;

	return setup_port(priv, port, bar, offset, board->reg_shift);
}

#define PCI_DEVICE_ID_EXAR_XR17V4358	0x4358
#define PCI_DEVICE_ID_EXAR_XR17V8358	0x8358

#define UART_EXAR_MPIOINT_7_0	0x8f	/* MPIOINT[7:0] */
#define UART_EXAR_MPIOLVL_7_0	0x90	/* MPIOLVL[7:0] */
#define UART_EXAR_MPIO3T_7_0	0x91	/* MPIO3T[7:0] */
#define UART_EXAR_MPIOINV_7_0	0x92	/* MPIOINV[7:0] */
#define UART_EXAR_MPIOSEL_7_0	0x93	/* MPIOSEL[7:0] */
#define UART_EXAR_MPIOOD_7_0	0x94	/* MPIOOD[7:0] */
#define UART_EXAR_MPIOINT_15_8	0x95	/* MPIOINT[15:8] */
#define UART_EXAR_MPIOLVL_15_8	0x96	/* MPIOLVL[15:8] */
#define UART_EXAR_MPIO3T_15_8	0x97	/* MPIO3T[15:8] */
#define UART_EXAR_MPIOINV_15_8	0x98	/* MPIOINV[15:8] */
#define UART_EXAR_MPIOSEL_15_8	0x99	/* MPIOSEL[15:8] */
#define UART_EXAR_MPIOOD_15_8	0x9a	/* MPIOOD[15:8] */

static int
pci_xr17c154_setup(struct serial_private *priv,
		   const struct pciserial_board *board,
		  struct uart_8250_port *port, int idx)
{
	port->port.flags |= UPF_EXAR_EFR;
	return pci_default_setup(priv, board, port, idx);
}

static inline int
xr17v35x_has_slave(struct serial_private *priv)
{
	const int dev_id = priv->dev->device;

	return ((dev_id == PCI_DEVICE_ID_EXAR_XR17V4358) ||
		(dev_id == PCI_DEVICE_ID_EXAR_XR17V8358));
}

static int
pci_xr17v35x_setup(struct serial_private *priv,
		   const struct pciserial_board *board,
		  struct uart_8250_port *port, int idx)
{
	u8 __iomem *p;
	int ret;

	p = pci_ioremap_bar(priv->dev, 0);
	if (!p)
		return -ENOMEM;

	port->port.flags |= UPF_EXAR_EFR;

	/*
	 * Setup the uart clock for the devices on expansion slot to
	 * half the clock speed of the main chip (which is 125MHz)
	 */
	if (xr17v35x_has_slave(priv) && idx >= 8)
		port->port.uartclk = (7812500 * 16 / 2);

	/*
	 * Setup Multipurpose Input/Output pins.
	 */
	if (idx == 0) {
		writeb(0x00, p + UART_EXAR_MPIOINT_7_0);
		writeb(0x00, p + UART_EXAR_MPIOLVL_7_0);
		writeb(0x00, p + UART_EXAR_MPIO3T_7_0);
		writeb(0x00, p + UART_EXAR_MPIOINV_7_0);
		writeb(0x00, p + UART_EXAR_MPIOSEL_7_0);
		writeb(0x00, p + UART_EXAR_MPIOOD_7_0);
		writeb(0x00, p + UART_EXAR_MPIOINT_15_8);
		writeb(0x00, p + UART_EXAR_MPIOLVL_15_8);
		writeb(0x00, p + UART_EXAR_MPIO3T_15_8);
		writeb(0x00, p + UART_EXAR_MPIOINV_15_8);
		writeb(0x00, p + UART_EXAR_MPIOSEL_15_8);
		writeb(0x00, p + UART_EXAR_MPIOOD_15_8);
	}
	writeb(0x00, p + UART_EXAR_8XMODE);
	writeb(UART_FCTR_EXAR_TRGD, p + UART_EXAR_FCTR);
	writeb(128, p + UART_EXAR_TXTRG);
	writeb(128, p + UART_EXAR_RXTRG);
	iounmap(p);

	ret = pci_default_setup(priv, board, port, idx);
	if (ret)
		return ret;

	if (idx == 0) {
		struct platform_device *device;

		device = platform_device_alloc("gpio_exar",
					       PLATFORM_DEVID_AUTO);
		if (!device)
			return -ENOMEM;

		if (platform_device_add(device) < 0) {
			platform_device_put(device);
			return -ENODEV;
		}

		port->port.private_data = device;
		platform_set_drvdata(device, priv->dev);
	}

	return 0;
}

static void pci_xr17v35x_exit(struct pci_dev *dev)
{
	struct serial_private *priv = pci_get_drvdata(dev);
	struct uart_8250_port *port = serial8250_get_port(priv->line[0]);
	struct platform_device *pdev = port->port.private_data;

	if (pdev) {
		platform_device_unregister(pdev);
		port->port.private_data = NULL;
	}
}

#define PCI_DEVICE_ID_COMMTECH_4224PCIE	0x0020
#define PCI_DEVICE_ID_COMMTECH_4228PCIE	0x0021
#define PCI_DEVICE_ID_COMMTECH_4222PCIE	0x0022

static struct pci_serial_quirk pci_serial_quirks[] __refdata = {
	/*
	 * Exar cards
	 */
	{
		.vendor = PCI_VENDOR_ID_EXAR,
		.device = PCI_DEVICE_ID_EXAR_XR17C152,
		.subvendor	= PCI_ANY_ID,
		.subdevice	= PCI_ANY_ID,
		.setup		= pci_xr17c154_setup,
	},
	{
		.vendor = PCI_VENDOR_ID_EXAR,
		.device = PCI_DEVICE_ID_EXAR_XR17C154,
		.subvendor	= PCI_ANY_ID,
		.subdevice	= PCI_ANY_ID,
		.setup		= pci_xr17c154_setup,
	},
	{
		.vendor = PCI_VENDOR_ID_EXAR,
		.device = PCI_DEVICE_ID_EXAR_XR17C158,
		.subvendor	= PCI_ANY_ID,
		.subdevice	= PCI_ANY_ID,
		.setup		= pci_xr17c154_setup,
	},
	{
		.vendor = PCI_VENDOR_ID_EXAR,
		.device = PCI_DEVICE_ID_EXAR_XR17V352,
		.subvendor	= PCI_ANY_ID,
		.subdevice	= PCI_ANY_ID,
		.setup		= pci_xr17v35x_setup,
		.exit		= pci_xr17v35x_exit,
	},
	{
		.vendor = PCI_VENDOR_ID_EXAR,
		.device = PCI_DEVICE_ID_EXAR_XR17V354,
		.subvendor	= PCI_ANY_ID,
		.subdevice	= PCI_ANY_ID,
		.setup		= pci_xr17v35x_setup,
		.exit		= pci_xr17v35x_exit,
	},
	{
		.vendor = PCI_VENDOR_ID_EXAR,
		.device = PCI_DEVICE_ID_EXAR_XR17V358,
		.subvendor	= PCI_ANY_ID,
		.subdevice	= PCI_ANY_ID,
		.setup		= pci_xr17v35x_setup,
		.exit		= pci_xr17v35x_exit,
	},
	{
		.vendor = PCI_VENDOR_ID_EXAR,
		.device = PCI_DEVICE_ID_EXAR_XR17V4358,
		.subvendor	= PCI_ANY_ID,
		.subdevice	= PCI_ANY_ID,
		.setup		= pci_xr17v35x_setup,
		.exit		= pci_xr17v35x_exit,
	},
	{
		.vendor = PCI_VENDOR_ID_EXAR,
		.device = PCI_DEVICE_ID_EXAR_XR17V8358,
		.subvendor	= PCI_ANY_ID,
		.subdevice	= PCI_ANY_ID,
		.setup		= pci_xr17v35x_setup,
		.exit		= pci_xr17v35x_exit,
	},
};

static inline int quirk_id_matches(u32 quirk_id, u32 dev_id)
{
	return quirk_id == PCI_ANY_ID || quirk_id == dev_id;
}

static struct pci_serial_quirk *find_quirk(struct pci_dev *dev)
{
	struct pci_serial_quirk *quirk;

	for (quirk = pci_serial_quirks; ; quirk++)
		if (quirk_id_matches(quirk->vendor, dev->vendor) &&
		    quirk_id_matches(quirk->device, dev->device) &&
		    quirk_id_matches(quirk->subvendor, dev->subsystem_vendor) &&
		    quirk_id_matches(quirk->subdevice, dev->subsystem_device))
			break;
	return quirk;
}

static inline int get_pci_irq(struct pci_dev *dev,
			      const struct pciserial_board *board)
{
	if (board->flags & FL_NOIRQ)
		return 0;
	else
		return dev->irq;
}

/*
 * This is the configuration table for all of the PCI serial boards
 * which we support.  It is directly indexed by the pci_board_num_t enum
 * value, which is encoded in the pci_device_id PCI probe table's
 * driver_data member.
 *
 * The makeup of these names are:
 *  pbn_bn{_bt}_n_baud{_offsetinhex}
 *
 *  bn		= PCI BAR number
 *  bt		= Index using PCI BARs
 *  n		= number of serial ports
 *  baud	= baud rate
 *  offsetinhex	= offset for each sequential port (in hex)
 *
 * This table is sorted by (in order): bn, bt, baud, offsetindex, n.
 *
 * Please note: in theory if n = 1, _bt infix should make no difference.
 * ie, pbn_b0_1_115200 is the same as pbn_b0_bt_1_115200
 */
enum pci_board_num_t {
	pbn_b0_2_1843200_200 = 0,
	pbn_b0_4_1843200_200,
	pbn_b0_8_1843200_200,

	/*
	 * Board-specific versions.
	 */
	pbn_exar_XR17C152,
	pbn_exar_XR17C154,
	pbn_exar_XR17C158,
	pbn_exar_XR17V352,
	pbn_exar_XR17V354,
	pbn_exar_XR17V358,
	pbn_exar_XR17V4358,
	pbn_exar_XR17V8358,
	pbn_exar_ibm_saturn,
};

/*
 * uart_offset - the space between channels
 * reg_shift   - describes how the UART registers are mapped
 *               to PCI memory by the card.
 * For example IER register on SBS, Inc. PMC-OctPro is located at
 * offset 0x10 from the UART base, while UART_IER is defined as 1
 * in include/linux/serial_reg.h,
 * see first lines of serial_in() and serial_out() in 8250.c
 */

static struct pciserial_board pci_boards[] = {
	[pbn_b0_2_1843200_200] = {
		.flags		= FL_BASE0,
		.num_ports	= 2,
		.base_baud	= 1843200,
		.uart_offset	= 0x200,
	},
	[pbn_b0_4_1843200_200] = {
		.flags		= FL_BASE0,
		.num_ports	= 4,
		.base_baud	= 1843200,
		.uart_offset	= 0x200,
	},
	[pbn_b0_8_1843200_200] = {
		.flags		= FL_BASE0,
		.num_ports	= 8,
		.base_baud	= 1843200,
		.uart_offset	= 0x200,
	},
	/*
	 * Exar Corp. XR17C15[248] Dual/Quad/Octal UART
	 *  Only basic 16550A support.
	 *  XR17C15[24] are not tested, but they should work.
	 */
	[pbn_exar_XR17C152] = {
		.flags		= FL_BASE0,
		.num_ports	= 2,
		.base_baud	= 921600,
		.uart_offset	= 0x200,
	},
	[pbn_exar_XR17C154] = {
		.flags		= FL_BASE0,
		.num_ports	= 4,
		.base_baud	= 921600,
		.uart_offset	= 0x200,
	},
	[pbn_exar_XR17C158] = {
		.flags		= FL_BASE0,
		.num_ports	= 8,
		.base_baud	= 921600,
		.uart_offset	= 0x200,
	},
	[pbn_exar_XR17V352] = {
		.flags		= FL_BASE0,
		.num_ports	= 2,
		.base_baud	= 7812500,
		.uart_offset	= 0x400,
		.reg_shift	= 0,
		.first_offset	= 0,
	},
	[pbn_exar_XR17V354] = {
		.flags		= FL_BASE0,
		.num_ports	= 4,
		.base_baud	= 7812500,
		.uart_offset	= 0x400,
		.reg_shift	= 0,
		.first_offset	= 0,
	},
	[pbn_exar_XR17V358] = {
		.flags		= FL_BASE0,
		.num_ports	= 8,
		.base_baud	= 7812500,
		.uart_offset	= 0x400,
		.reg_shift	= 0,
		.first_offset	= 0,
	},
	[pbn_exar_XR17V4358] = {
		.flags		= FL_BASE0,
		.num_ports	= 12,
		.base_baud	= 7812500,
		.uart_offset	= 0x400,
		.reg_shift	= 0,
		.first_offset	= 0,
	},
	[pbn_exar_XR17V8358] = {
		.flags		= FL_BASE0,
		.num_ports	= 16,
		.base_baud	= 7812500,
		.uart_offset	= 0x400,
		.reg_shift	= 0,
		.first_offset	= 0,
	},
	[pbn_exar_ibm_saturn] = {
		.flags		= FL_BASE0,
		.num_ports	= 1,
		.base_baud	= 921600,
		.uart_offset	= 0x200,
	},
};

static struct serial_private *
init_ports(struct pci_dev *dev, const struct pciserial_board *board)
{
	struct uart_8250_port uart;
	struct serial_private *priv;
	struct pci_serial_quirk *quirk;
	int nr_ports, i;

	nr_ports = board->num_ports;

	/*
	 * Find an init and setup quirks.
	 */
	quirk = find_quirk(dev);

	priv = kzalloc(sizeof(*priv) + sizeof(unsigned int) * nr_ports,
		       GFP_KERNEL);
	if (!priv) {
		priv = ERR_PTR(-ENOMEM);
		goto err_deinit;
	}

	priv->dev = dev;
	priv->quirk = quirk;

	memset(&uart, 0, sizeof(uart));
	uart.port.flags = UPF_SKIP_TEST | UPF_BOOT_AUTOCONF | UPF_SHARE_IRQ;
	uart.port.uartclk = board->base_baud * 16;
	uart.port.irq = dev->irq;
	uart.port.dev = &dev->dev;

	for (i = 0; i < nr_ports; i++) {
		if (quirk->setup(priv, board, &uart, i))
			break;

		dev_dbg(&dev->dev, "Setup PCI port: port %lx, irq %d, type %d\n",
			uart.port.iobase, uart.port.irq, uart.port.iotype);

		priv->line[i] = serial8250_register_8250_port(&uart);
		if (priv->line[i] < 0) {
			dev_err(&dev->dev,
				"Couldn't register serial port %lx, irq %d, type %d, error %d\n",
				uart.port.iobase, uart.port.irq,
				uart.port.iotype, priv->line[i]);
			break;
		}
	}
	priv->nr = i;
	return priv;

err_deinit:
	if (quirk->exit)
		quirk->exit(dev);
	return priv;
}

/*
 * Probe one serial board.  Unfortunately, there is no rhyme nor reason
 * to the arrangement of serial ports on a PCI card.
 */
static int
exar_pci_init(struct pci_dev *dev, const struct pci_device_id *ent)
{
	struct serial_private *priv;
	const struct pciserial_board *board;
	int rc;

	if (ent->driver_data >= ARRAY_SIZE(pci_boards)) {
		dev_err(&dev->dev, "invalid driver_data: %ld\n",
			ent->driver_data);
		return -EINVAL;
	}

	board = &pci_boards[ent->driver_data];

	rc = pcim_enable_device(dev);
	pci_save_state(dev);
	if (rc)
		return rc;

	priv = init_ports(dev, board);
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	pci_set_drvdata(dev, priv);
	return 0;
}

static void exar_pci_remove(struct pci_dev *dev)
{
	struct serial_private *priv = pci_get_drvdata(dev);

	pciserial_remove_ports(priv);
}

#ifdef CONFIG_PM_SLEEP
static int exar_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct serial_private *priv = pci_get_drvdata(pdev);

	if (priv)
		pciserial_suspend_ports(priv);

	return 0;
}

static int exar_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct serial_private *priv = pci_get_drvdata(pdev);
	int err;

	if (priv) {
		/*
		 * The device may have been disabled.  Re-enable it.
		 */
		err = pci_enable_device(pdev);
		/* FIXME: We cannot simply error out here */
		if (err)
			dev_err(dev, "Unable to re-enable ports, trying to continue.\n");
		pciserial_resume_ports(priv);
	}
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(exar_pci_pm, exar_suspend, exar_resume);

static struct pci_device_id exar_pci_tbl[] = {
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C152,
		PCI_SUBVENDOR_ID_CONNECT_TECH,
		PCI_SUBDEVICE_ID_CONNECT_TECH_PCI_UART_2_232, 0, 0,
		pbn_b0_2_1843200_200 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C154,
		PCI_SUBVENDOR_ID_CONNECT_TECH,
		PCI_SUBDEVICE_ID_CONNECT_TECH_PCI_UART_4_232, 0, 0,
		pbn_b0_4_1843200_200 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C158,
		PCI_SUBVENDOR_ID_CONNECT_TECH,
		PCI_SUBDEVICE_ID_CONNECT_TECH_PCI_UART_8_232, 0, 0,
		pbn_b0_8_1843200_200 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C152,
		PCI_SUBVENDOR_ID_CONNECT_TECH,
		PCI_SUBDEVICE_ID_CONNECT_TECH_PCI_UART_1_1, 0, 0,
		pbn_b0_2_1843200_200 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C154,
		PCI_SUBVENDOR_ID_CONNECT_TECH,
		PCI_SUBDEVICE_ID_CONNECT_TECH_PCI_UART_2_2, 0, 0,
		pbn_b0_4_1843200_200 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C158,
		PCI_SUBVENDOR_ID_CONNECT_TECH,
		PCI_SUBDEVICE_ID_CONNECT_TECH_PCI_UART_4_4, 0, 0,
		pbn_b0_8_1843200_200 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C152,
		PCI_SUBVENDOR_ID_CONNECT_TECH,
		PCI_SUBDEVICE_ID_CONNECT_TECH_PCI_UART_2, 0, 0,
		pbn_b0_2_1843200_200 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C154,
		PCI_SUBVENDOR_ID_CONNECT_TECH,
		PCI_SUBDEVICE_ID_CONNECT_TECH_PCI_UART_4, 0, 0,
		pbn_b0_4_1843200_200 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C158,
		PCI_SUBVENDOR_ID_CONNECT_TECH,
		PCI_SUBDEVICE_ID_CONNECT_TECH_PCI_UART_8, 0, 0,
		pbn_b0_8_1843200_200 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C152,
		PCI_SUBVENDOR_ID_CONNECT_TECH,
		PCI_SUBDEVICE_ID_CONNECT_TECH_PCI_UART_2_485, 0, 0,
		pbn_b0_2_1843200_200 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C154,
		PCI_SUBVENDOR_ID_CONNECT_TECH,
		PCI_SUBDEVICE_ID_CONNECT_TECH_PCI_UART_4_485, 0, 0,
		pbn_b0_4_1843200_200 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C158,
		PCI_SUBVENDOR_ID_CONNECT_TECH,
		PCI_SUBDEVICE_ID_CONNECT_TECH_PCI_UART_8_485, 0, 0,
		pbn_b0_8_1843200_200 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C152,
		PCI_VENDOR_ID_IBM, PCI_SUBDEVICE_ID_IBM_SATURN_SERIAL_ONE_PORT,
		0, 0, pbn_exar_ibm_saturn },
	/*
	 * Exar Corp. XR17C15[248] Dual/Quad/Octal UART
	 */
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C152,
		PCI_ANY_ID, PCI_ANY_ID,
		0,
		0, pbn_exar_XR17C152 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C154,
		PCI_ANY_ID, PCI_ANY_ID,
		0,
		0, pbn_exar_XR17C154 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17C158,
		PCI_ANY_ID, PCI_ANY_ID,
		0,
		0, pbn_exar_XR17C158 },
	/*
	 * Exar Corp. XR17V[48]35[248] Dual/Quad/Octal/Hexa PCIe UARTs
	 */
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17V352,
		PCI_ANY_ID, PCI_ANY_ID,
		0,
		0, pbn_exar_XR17V352 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17V354,
		PCI_ANY_ID, PCI_ANY_ID,
		0,
		0, pbn_exar_XR17V354 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17V358,
		PCI_ANY_ID, PCI_ANY_ID,
		0,
		0, pbn_exar_XR17V358 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17V4358,
		PCI_ANY_ID, PCI_ANY_ID,
		0,
		0, pbn_exar_XR17V4358 },
	{	PCI_VENDOR_ID_EXAR, PCI_DEVICE_ID_EXAR_XR17V8358,
		PCI_ANY_ID, PCI_ANY_ID,
		0,
		0, pbn_exar_XR17V8358 },
	{	PCI_VENDOR_ID_COMMTECH, PCI_DEVICE_ID_COMMTECH_4222PCIE,
		PCI_ANY_ID, PCI_ANY_ID,
		0,
		0, pbn_exar_XR17V352 },
	{	PCI_VENDOR_ID_COMMTECH, PCI_DEVICE_ID_COMMTECH_4224PCIE,
		PCI_ANY_ID, PCI_ANY_ID,
		0,
		0, pbn_exar_XR17V354 },
	{	PCI_VENDOR_ID_COMMTECH, PCI_DEVICE_ID_COMMTECH_4228PCIE,
		PCI_ANY_ID, PCI_ANY_ID,
		0,
		0, pbn_exar_XR17V358 },
	{ 0, }
};

static struct pci_driver exar_pci_driver = {
	.name		= "exar_serial",
	.probe		= exar_pci_init,
	.remove		= exar_pci_remove,
	.driver         = {
		.pm     = &exar_pci_pm,
	},
	.id_table	= exar_pci_tbl,
};

module_pci_driver(exar_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Exar Serial Dricer");
MODULE_DEVICE_TABLE(pci, exar_pci_tbl);
