#include <linux/pci.h>
#include <linux/serial_8250.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/mfd/f81504.h>

#include "8250.h"

static u32 baudrate_table[] = { 1500000, 1152000, 921600 };
static u8 clock_table[] = { F81504_CLKSEL_24_MHZ, F81504_CLKSEL_18DOT46_MHZ,
				F81504_CLKSEL_14DOT77_MHZ };

/* We should do proper H/W transceiver setting before change to RS485 mode */
static int f81504_rs485_config(struct uart_port *port,
			       struct serial_rs485 *rs485)
{
	u8 setting;
	u8 *index = (u8 *) port->private_data;
	struct platform_device *pdev = container_of(port->dev,
					struct platform_device, dev);
	struct pci_dev *pci_dev = to_pci_dev(pdev->dev.parent);

	pci_read_config_byte(pci_dev, F81504_UART_START_ADDR +
			F81504_UART_OFFSET * *index + F81504_UART_MODE_OFFSET,
			&setting);

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
		setting |= F81504_RTS_CONTROL_BY_HW;

		if (rs485->flags & SER_RS485_RTS_ON_SEND) {
			/* RTS driving high on TX */
			setting &= ~F81504_RTS_INVERT;
		} else {
			/* RTS driving low on TX */
			setting |= F81504_RTS_INVERT;
		}

		rs485->delay_rts_after_send = 0;
		rs485->delay_rts_before_send = 0;
	} else {
		/* Disable RTS H/W control mode */
		setting &= ~(F81504_RTS_CONTROL_BY_HW | F81504_RTS_INVERT);
	}

	pci_write_config_byte(pci_dev, F81504_UART_START_ADDR +
			F81504_UART_OFFSET * *index + F81504_UART_MODE_OFFSET,
			setting);

	if (rs485 != &port->rs485)
		port->rs485 = *rs485;

	return 0;
}

static int f81504_check_baudrate(u32 baud, size_t *idx)
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
	struct platform_device *pdev = container_of(port->dev,
					struct platform_device, dev);
	struct pci_dev *dev = to_pci_dev(pdev->dev.parent);
	unsigned int baud = tty_termios_baud_rate(termios);
	u8 tmp, *offset = (u8 *) port->private_data;
	size_t i;

	do {
		/* read current clock source (masked with CLOCK_RATE_MASK) */
		pci_read_config_byte(dev, F81504_UART_START_ADDR + *offset *
				F81504_UART_OFFSET, &tmp);

		if (baud <= 115200) {
			/*
			 * direct use 1.8432MHz when baudrate smaller then or
			 * equal 115200bps
			 */
			port->uartclk = 115200 * 16;
			pci_write_config_byte(dev, F81504_UART_START_ADDR +
					*offset * F81504_UART_OFFSET,
					tmp & ~F81504_CLOCK_RATE_MASK);
			break;
		}

		if (!f81504_check_baudrate(baud, &i)) {
			/* had optimize value */
			port->uartclk = baudrate_table[i] * 16;
			tmp = (tmp & ~F81504_CLOCK_RATE_MASK) | clock_table[i];
			pci_write_config_byte(dev, F81504_UART_START_ADDR +
					*offset * F81504_UART_OFFSET, tmp);
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

static int f81504_register_port(struct platform_device *dev,
		unsigned long address, int idx)
{
	struct pci_dev *pci_dev = to_pci_dev(dev->dev.parent);
	struct uart_8250_port port;
	u8 *data;

	memset(&port, 0, sizeof(port));
	data = devm_kzalloc(&dev->dev, sizeof(u8), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	*data = idx;
	port.port.iotype = UPIO_PORT;
	port.port.irq = pci_dev->irq;
	port.port.flags = UPF_SKIP_TEST | UPF_FIXED_TYPE | UPF_BOOT_AUTOCONF |
			UPF_SHARE_IRQ;
	port.port.uartclk = 1843200;
	port.port.dev = &dev->dev;
	port.port.iobase = address;
	port.port.type = PORT_16550A;
	port.port.fifosize = 128;
	port.port.rs485_config = f81504_rs485_config;
	port.port.set_termios = f81504_set_termios;
	port.tx_loadsz = 32;
	port.port.private_data = data;	/* save current idx */

	return serial8250_register_8250_port(&port);
}

static int f81504_serial_probe(struct platform_device *pdev)
{
	int line;
	size_t *index = (size_t *)dev_get_platdata(&pdev->dev);
	struct resource *io = platform_get_resource(pdev, IORESOURCE_IO, 0);

	line = f81504_register_port(pdev, io->start, *index);
	if (line < 0)
		return line;

	/*
	 * Re-assign line to replace old port PCIE configuration space idx,
	 * port idx is saved in per-port private data.
	 */
	*index = line;
	return 0;
}

static int f81504_serial_remove(struct platform_device *pdev)
{
	size_t *line = (size_t *)dev_get_platdata(&pdev->dev);

	serial8250_unregister_port(*line);
	return 0;
}


#ifdef CONFIG_PM_SLEEP
static int f81504_serial_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	size_t *line = (size_t *)dev_get_platdata(&pdev->dev);

	serial8250_suspend_port(*line);
	return 0;
}

static int f81504_serial_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	size_t *line = (size_t *)dev_get_platdata(&pdev->dev);
	struct uart_8250_port *port = serial8250_get_port(*line);

	f81504_rs485_config(&port->port, NULL);
	serial8250_resume_port(*line);
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(f81504_serial_pm_ops, f81504_serial_suspend,
		f81504_serial_resume);

static struct platform_driver f81504_serial_driver = {
	.driver = {
		.name	= F81504_SERIAL_NAME,
		.owner	= THIS_MODULE,
		.pm     = &f81504_serial_pm_ops,
	},
	.probe		= f81504_serial_probe,
	.remove		= f81504_serial_remove,
};

static int __init f81504_serial_init(void)
{
	return platform_driver_register(&f81504_serial_driver);
}
subsys_initcall(f81504_serial_init);

static void __exit f81504_serial_exit(void)
{
	platform_driver_unregister(&f81504_serial_driver);
}
module_exit(f81504_serial_exit);

MODULE_AUTHOR("Peter Hong <Peter_Hong@fintek.com.tw>");
MODULE_DESCRIPTION("Fintek F81504/508/512 PCIE 16550A serial port driver");
MODULE_LICENSE("GPL");
