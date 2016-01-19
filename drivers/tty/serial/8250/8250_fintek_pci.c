/*
 *  Base port operations for Fintek F81504/508/512 PCI-to-UARTs 16550A-type
 *  serial ports
 */
#include <linux/pci.h>
#include <linux/serial_8250.h>
#include <linux/module.h>
#include "8250.h"

#define FINTEK_VID		0x1c29
#define FINTEK_F81504		0x1104
#define FINTEK_F81508		0x1108
#define FINTEK_F81512		0x1112

#define FINTEK_MAX_PORT		12
#define DRIVER_NAME		"f81504_serial"
#define DEV_DESC		"Fintek F81504/508/512 PCIE-to-UART"

#define UART_START_ADDR		0x40
#define UART_MODE_OFFSET	0x07
#define UART_OFFSET		0x08

/* RTS will control by MCR if this bit is 0 */
#define RTS_CONTROL_BY_HW	BIT(4)
/* only worked with FINTEK_RTS_CONTROL_BY_HW on */
#define RTS_INVERT		BIT(5)

#define CLOCK_RATE_MASK		0xc0
#define CLKSEL_1DOT846_MHZ	0x00
#define CLKSEL_18DOT46_MHZ	0x40
#define CLKSEL_24_MHZ		0x80
#define CLKSEL_14DOT77_MHZ	0xc0

#define IRQSEL_REG		0xb8

static u32 baudrate_table[] = { 1500000, 1152000, 921600 };
static u8 clock_table[] = { CLKSEL_24_MHZ, CLKSEL_18DOT46_MHZ,
		CLKSEL_14DOT77_MHZ };

struct f81504_pci_private {
	int line[FINTEK_MAX_PORT];
	u32 uart_count;
};

/* We should do proper H/W transceiver setting before change to RS485 mode */
static int f81504_rs485_config(struct uart_port *port,
			       struct serial_rs485 *rs485)
{
	u8 setting;
	u8 *index = (u8 *)port->private_data;
	struct pci_dev *pci_dev = container_of(port->dev, struct pci_dev, dev);

	pci_read_config_byte(pci_dev, UART_START_ADDR + UART_OFFSET * *index +
			UART_MODE_OFFSET, &setting);

	if (!rs485)
		rs485 = &port->rs485;
	else if (rs485->flags & SER_RS485_ENABLED)
		memset(rs485->padding, 0, sizeof(rs485->padding));
	else
		memset(rs485, 0, sizeof(*rs485));

	/* F81504/508/512 not support RTS delay before or after send */
	rs485->flags &= SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;

	if (rs485->flags & SER_RS485_ENABLED) {
		/* Enable RTS H/W control mode */
		setting |= RTS_CONTROL_BY_HW;

		if (rs485->flags & SER_RS485_RTS_ON_SEND) {
			/* RTS driving high on TX */
			setting &= ~RTS_INVERT;
		} else {
			/* RTS driving low on TX */
			setting |= RTS_INVERT;
		}

		rs485->delay_rts_after_send = 0;
		rs485->delay_rts_before_send = 0;
	} else {
		/* Disable RTS H/W control mode */
		setting &= ~(RTS_CONTROL_BY_HW | RTS_INVERT);
	}

	pci_write_config_byte(pci_dev, UART_START_ADDR + UART_OFFSET * *index +
			UART_MODE_OFFSET, setting);

	if (rs485 != &port->rs485)
		port->rs485 = *rs485;

	return 0;
}

static int f81504_check_baudrate(u32 baud, int *idx)
{
	size_t index;
	u32 quot, rem;

	for (index = 0; index < ARRAY_SIZE(baudrate_table); ++index) {
		/* Clock source must largeer than desire baudrate */
		if (baud > baudrate_table[index])
			continue;

		quot = DIV_ROUND_CLOSEST(baudrate_table[index], baud);
		/* find divisible clock source */
		rem = baudrate_table[index] % baud;

		if (quot && !rem) {
			if (idx)
				*idx = index;
			return 0;
		}
	}

	return -EINVAL;
}

static void f81504_set_termios(struct uart_port *port,
		struct ktermios *termios, struct ktermios *old)
{
	struct pci_dev *dev = container_of(port->dev, struct pci_dev, dev);
	unsigned int baud = tty_termios_baud_rate(termios);
	u8 tmp, *offset = (u8 *)port->private_data;
	int i;

	do {
		/* read current clock source (masked with CLOCK_RATE_MASK) */
		pci_read_config_byte(dev, UART_START_ADDR + *offset *
				UART_OFFSET, &tmp);

		if (baud <= 115200) {
			/*
			 * direct use 1.8432MHz when baudrate smaller then or
			 * equal 115200bps
			 */
			port->uartclk = 115200 * 16;
			pci_write_config_byte(dev, UART_START_ADDR + *offset *
					UART_OFFSET, tmp & ~CLOCK_RATE_MASK);
			break;
		}

		if (!f81504_check_baudrate(baud, &i)) {
			/* had optimize value */
			port->uartclk = baudrate_table[i] * 16;
			tmp = (tmp & ~CLOCK_RATE_MASK) | clock_table[i];
			pci_write_config_byte(dev, UART_START_ADDR + *offset *
					UART_OFFSET, tmp);
			break;
		}

		if (old && !f81504_check_baudrate(tty_termios_baud_rate(old),
				NULL)) {
			/*
			 * If it can't found suitable clock source but had old
			 * accpetable baudrate, we'll use it
			 */
			baud = tty_termios_baud_rate(old);
		} else {
			/*
			 * If it can't found suitable clock source and not old
			 * config, we'll direct set 115200bps for future use
			 */
			baud = 115200;
		}

		if (tty_termios_baud_rate(termios))
			tty_termios_encode_baud_rate(termios, baud, baud);
	} while (1);

	serial8250_do_set_termios(port, termios, old);
}

static int f81504_register_port(struct pci_dev *dev, unsigned long address,
				int idx)
{
	struct uart_8250_port port;
	u8 *data;

	memset(&port, 0, sizeof(port));

	data = devm_kzalloc(&dev->dev, sizeof(u8), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	*data = idx;
	port.port.iotype = UPIO_PORT;
	port.port.mapbase = 0;
	port.port.membase = NULL;
	port.port.regshift = 0;
	port.port.irq = dev->irq;
	port.port.flags = UPF_SKIP_TEST | UPF_FIXED_TYPE | UPF_BOOT_AUTOCONF |
			UPF_SHARE_IRQ;
	port.port.uartclk = 115200 * 16;
	port.port.dev = &dev->dev;
	port.port.iobase = address;
	port.port.type = PORT_16550A;
	port.port.fifosize = 128;
	port.tx_loadsz = 32;
	port.port.private_data = data;	/* save current idx */
	port.port.set_termios = f81504_set_termios;
	port.port.rs485_config = f81504_rs485_config;

	return serial8250_register_8250_port(&port);
}

static int f81504_port_init(struct pci_dev *dev)
{
	size_t i;
	u32 max_port, iobase;
	u32 bar_data[3];
	u16 tmp;
	u8 config_base;
	struct f81504_pci_private *priv = pci_get_drvdata(dev);
	struct uart_8250_port *port;

	switch (dev->device) {
	case FINTEK_F81504: /* 4 ports */
	case FINTEK_F81508: /* 8 ports */
		max_port = dev->device & 0xff;
		break;
	case FINTEK_F81512: /* 12 ports */
		max_port = 12;
		break;
	default:
		return -EINVAL;
	}

	/* Get the UART IO address dispatch from the BIOS */
	pci_read_config_dword(dev, 0x24, &bar_data[0]);
	pci_read_config_dword(dev, 0x20, &bar_data[1]);
	pci_read_config_dword(dev, 0x1c, &bar_data[2]);

	/* Compatible for newer step IC */
	pci_read_config_word(dev, IRQSEL_REG, &tmp);
	tmp |= BIT(8);
	pci_write_config_word(dev, IRQSEL_REG, tmp);

	for (i = 0; i < max_port; ++i) {
		/* UART0 configuration offset start from 0x40 */
		config_base = UART_START_ADDR + UART_OFFSET * i;

		/* Calculate Real IO Port */
		iobase = (bar_data[i / 4] & 0xffffffe0) + (i % 4) * 8;

		/* Enable UART I/O port */
		pci_write_config_byte(dev, config_base + 0x00, 0x01);

		/* Select 128-byte FIFO and 8x FIFO threshold */
		pci_write_config_byte(dev, config_base + 0x01, 0x33);

		/* LSB UART */
		pci_write_config_byte(dev, config_base + 0x04,
				(u8)(iobase & 0xff));

		/* MSB UART */
		pci_write_config_byte(dev, config_base + 0x05,
				(u8)((iobase & 0xff00) >> 8));

		pci_write_config_byte(dev, config_base + 0x06, dev->irq);

		/* First init. force init to RS232 Mode */
		pci_write_config_byte(dev, config_base + 0x07, 0x01);
	}

	if (!priv)
		return 0;

	/* re-apply RS232/485 mode when f81504_resume() */
	for (i = 0; i < priv->uart_count; ++i) {
		port = serial8250_get_port(priv->line[i]);
		f81504_rs485_config(&port->port, NULL);
	}

	return 0;
}

static int f81504_probe(struct pci_dev *dev, const struct pci_device_id
				*dev_id)
{
	int status;
	size_t i;
	u16 iobase;
	u8 tmp;
	struct f81504_pci_private *priv;

	status = pci_enable_device(dev);
	if (status)
		return status;

	/* Init PCI Configuration Space */
	status = f81504_port_init(dev);
	if (status)
		return status;

	priv = devm_kzalloc(&dev->dev, sizeof(struct f81504_pci_private),
				GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	pci_set_drvdata(dev, priv);

	/* Generate UART Ports */
	for (i = 0; i < dev_id->driver_data; ++i) {
		/* Check UART is enabled */
		pci_read_config_byte(dev, UART_START_ADDR + i * UART_OFFSET,
					&tmp);
		if (!tmp)
			continue;

		/* Get UART IO Address */
		pci_read_config_word(dev, UART_START_ADDR + i * UART_OFFSET +
					4, &iobase);

		/* Register to serial port */
		priv->line[priv->uart_count] = f81504_register_port(dev,
								iobase, i);
		++priv->uart_count;
	}

	return 0;
}

static void f81504_remove(struct pci_dev *dev)
{
	struct f81504_pci_private *priv = pci_get_drvdata(dev);
	size_t i;

	for (i = 0; i < priv->uart_count; ++i) {
		if (priv->line[i] < 0)
			continue;

		serial8250_unregister_port(priv->line[i]);
	}

	pci_disable_device(dev);
}

#ifdef CONFIG_PM
static int f81504_suspend(struct pci_dev *dev, pm_message_t state)
{
	size_t i;
	int status;
	struct f81504_pci_private *priv = pci_get_drvdata(dev);

	status = pci_save_state(dev);
	if (status)
		return status;

	status = pci_set_power_state(dev, pci_choose_state(dev, state));
	if (status)
		return status;

	for (i = 0; i < priv->uart_count; ++i) {
		if (priv->line[i] < 0)
			continue;

		serial8250_suspend_port(priv->line[i]);
	}

	return 0;
}

static int f81504_resume(struct pci_dev *dev)
{
	size_t i;
	int status;
	struct f81504_pci_private *priv = pci_get_drvdata(dev);

	status = pci_set_power_state(dev, PCI_D0);
	if (status)
		return status;

	pci_restore_state(dev);

	/* Init PCI Configuration Space */
	status = f81504_port_init(dev);
	if (status)
		return status;

	for (i = 0; i < priv->uart_count; ++i) {
		if (priv->line[i] < 0)
			continue;

		serial8250_resume_port(priv->line[i]);
	}

	return 0;
}
#else
#define f81504_suspend NULL
#define f81504_resume NULL
#endif

static const struct pci_device_id f81504_dev_table[] = {
	/* Fintek PCI serial cards */
	{PCI_DEVICE(FINTEK_VID, FINTEK_F81504), .driver_data = 4},
	{PCI_DEVICE(FINTEK_VID, FINTEK_F81508), .driver_data = 8},
	{PCI_DEVICE(FINTEK_VID, FINTEK_F81512), .driver_data = 12},
	{}
};

MODULE_DEVICE_TABLE(pci, f81504_dev_table);

static struct pci_driver f81504_driver = {
	.name = DRIVER_NAME,
	.probe = f81504_probe,
	.remove = f81504_remove,
	.suspend = f81504_suspend,
	.resume = f81504_resume,
	.id_table = f81504_dev_table,
};

module_pci_driver(f81504_driver);

MODULE_DESCRIPTION(DEV_DESC);
MODULE_AUTHOR("Peter Hong <Peter_Hong@fintek.com.tw>");
MODULE_LICENSE("GPL");
