/*
 * Core operations for Fintek F81504/508/512 PCIE-to-UART/GPIO device
 */
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/mfd/core.h>
#include <linux/mfd/f81504.h>

#define F81504_DRIVER_NAME	"f81504_core"
#define F81504_DEV_DESC		"Fintek F81504/508/512 PCIE-to-UART core"
#define F81504_IO_REGION	8

const u8 fintek_gpio_mapping[F81504_MAX_GPIO_CNT] = { 2, 3, 8, 9, 10, 11 };
EXPORT_SYMBOL(fintek_gpio_mapping);

static int f81504_port_init(struct pci_dev *dev)
{
	size_t i, j;
	u32 max_port, iobase, gpio_addr;
	u32 bar_data[3];
	u16 tmp;
	u8 config_base, gpio_en, f0h_data, f3h_data;
	bool is_gpio;
	struct f81504_pci_private *priv = pci_get_drvdata(dev);

	/* Init GPIO IO Address */
	pci_read_config_dword(dev, 0x18, &gpio_addr);
	gpio_addr &= 0xffffffe0;
	pci_write_config_byte(dev, F81504_GPIO_IO_LSB_REG, gpio_addr & 0xff);
	pci_write_config_byte(dev, F81504_GPIO_IO_MSB_REG, (gpio_addr >> 8) &
			0xff);

	/*
	 * The PCI board is multi-function, some serial port can converts to
	 * GPIO function. Customers could changes the F0/F3h values in EEPROM
	 *
	 * F0h bit0~5: Enable GPIO0~5
	 *     bit6~7: Reserve
	 *
	 * F3h bit0~5: Multi-Functional Flag (0:GPIO/1:UART)
	 *		bit0: UART2 pin out for UART2 / GPIO0
	 *		bit1: UART3 pin out for UART3 / GPIO1
	 *		bit2: UART8 pin out for UART8 / GPIO2
	 *		bit3: UART9 pin out for UART9 / GPIO3
	 *		bit4: UART10 pin out for UART10 / GPIO4
	 *		bit5: UART11 pin out for UART11 / GPIO5
	 *     bit6~7: Reserve
	 */
	if (priv) {
		/* Reinit from resume(), read the previous value from priv */
		gpio_en = priv->gpio_en;

		/* re-save GPIO IO addr for called by resume() */
		priv->gpio_ioaddr = gpio_addr;
	} else {
		/* Driver first init */
		pci_read_config_byte(dev, F81504_GPIO_ENABLE_REG, &f0h_data);
		pci_read_config_byte(dev, F81504_GPIO_MODE_REG, &f3h_data);

		/* find the max set of GPIOs */
		gpio_en = f0h_data | ~f3h_data;
	}

	switch (dev->device) {
	case FINTEK_F81504: /* 4 ports */
		/* F81504 max 2 sets of GPIO, others are max 6 sets*/
		gpio_en &= 0x03;
	case FINTEK_F81508: /* 8 ports */
		max_port = dev->device & 0xff;
		break;
	case FINTEK_F81512: /* 12 ports */
		max_port = 12;
		break;
	default:
		return -EINVAL;
	}

	/* rewrite GPIO Mode setting */
	pci_write_config_byte(dev, F81504_GPIO_ENABLE_REG, gpio_en & 0x3f);
	pci_write_config_byte(dev, F81504_GPIO_MODE_REG, ~gpio_en & 0x3f);

	/* Get the UART IO address dispatch from the BIOS */
	pci_read_config_dword(dev, 0x24, &bar_data[0]);
	pci_read_config_dword(dev, 0x20, &bar_data[1]);
	pci_read_config_dword(dev, 0x1c, &bar_data[2]);

	/* Compatible bit for newer step IC */
	pci_read_config_word(dev, F81504_IRQSEL_REG, &tmp);
	tmp |= BIT(8);
	pci_write_config_word(dev, F81504_IRQSEL_REG, tmp);

	for (i = 0; i < max_port; ++i) {
		/* UART0 configuration offset start from 0x40 */
		config_base = F81504_UART_START_ADDR + F81504_UART_OFFSET * i;
		is_gpio = false;

		/* find every port to check is multi-function port? */
		for (j = 0; j < ARRAY_SIZE(fintek_gpio_mapping); ++j) {
			if (fintek_gpio_mapping[j] != i || !(gpio_en & BIT(j)))
				continue;

			/*
			 * This port is multi-function and enabled as gpio
			 * mode. So we'll not configure it as serial port.
			 */
			is_gpio = true;
			break;
		}

		/*
		 * If the serial port is setting to gpio mode, don't init it.
		 * Disable the serial port for user-space application to
		 * control.
		 */
		if (is_gpio) {
			/* Disable current serial port */
			pci_write_config_byte(dev, config_base + 0x00, 0x00);
			continue;
		}

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

		/*
		 * Force init to RS232 / Share Mode, recovery previous mode
		 * will done in F81504 8250 platform driver resume()
		 */
		pci_write_config_byte(dev, config_base + 0x07, 0x01);
	}

	return 0;
}

static int f81504_prepage_serial_port(struct pci_dev *dev, int max_port)
{
	size_t i;
	int status;
	u8 tmp;
	u16 iobase;
	struct resource	resources = DEFINE_RES_IO(0, 0);
	struct mfd_cell f81504_serial_cell = {
		.name = F81504_SERIAL_NAME,
		.num_resources	= 1,
	};

	for (i = 0; i < max_port; ++i) {
		/* Check UART is enabled */
		pci_read_config_byte(dev, F81504_UART_START_ADDR + i *
				F81504_UART_OFFSET, &tmp);
		if (!tmp)
			continue;

		/* Get UART IO Address */
		pci_read_config_word(dev, F81504_UART_START_ADDR + i *
				F81504_UART_OFFSET + 4, &iobase);

		resources.start = iobase;
		resources.end = iobase + F81504_IO_REGION - 1;

		f81504_serial_cell.resources = &resources;
		f81504_serial_cell.pdata_size = sizeof(i);
		f81504_serial_cell.platform_data = &i;

		status = mfd_add_devices(&dev->dev, PLATFORM_DEVID_AUTO,
					&f81504_serial_cell, 1, NULL, dev->irq,
					NULL);
		if (status) {
			dev_warn(&dev->dev, "%s: add device failed: %d\n",
					__func__, status);
			return status;
		}
	}

	return 0;
}

static int f81504_prepage_gpiolib(struct pci_dev *dev)
{
	size_t i;
	int status;
	struct f81504_pci_private *priv = pci_get_drvdata(dev);
	struct mfd_cell f81504_gpio_cell = {
		.name = F81504_GPIO_NAME,
	};

	for (i = 0; i < ARRAY_SIZE(fintek_gpio_mapping); ++i) {
		if (!(priv->gpio_en & BIT(i)))
			continue;

		f81504_gpio_cell.pdata_size = sizeof(i);
		f81504_gpio_cell.platform_data = &i;

		status = mfd_add_devices(&dev->dev, PLATFORM_DEVID_AUTO,
					&f81504_gpio_cell, 1, NULL, dev->irq,
					NULL);
		if (status) {
			dev_warn(&dev->dev, "%s: add device failed: %d\n",
					__func__, status);
			return status;
		}
	}

	return 0;
}

static int f81504_probe(struct pci_dev *dev, const struct pci_device_id
			*dev_id)
{
	int status;
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

	/* Save the GPIO_ENABLE_REG after f81504_port_init() for future use */
	pci_read_config_byte(dev, F81504_GPIO_ENABLE_REG, &priv->gpio_en);

	/* Save GPIO IO Addr to private data */
	pci_read_config_byte(dev, F81504_GPIO_IO_MSB_REG, &tmp);
	priv->gpio_ioaddr = tmp << 8;
	pci_read_config_byte(dev, F81504_GPIO_IO_LSB_REG, &tmp);
	priv->gpio_ioaddr |= tmp;

	pci_set_drvdata(dev, priv);

	/* Generate UART Ports */
	status = f81504_prepage_serial_port(dev, dev_id->driver_data);
	if (status)
		goto failed;

	/* Generate GPIO Sets */
	status = f81504_prepage_gpiolib(dev);
	if (status)
		goto failed;

	return 0;

failed:
	mfd_remove_devices(&dev->dev);
	pci_disable_device(dev);
	return status;
}

static void f81504_remove(struct pci_dev *dev)
{
	mfd_remove_devices(&dev->dev);
	pci_disable_device(dev);
}

#ifdef CONFIG_PM_SLEEP
static int f81504_suspend(struct device *dev)
{
	return 0;
}

static int f81504_resume(struct device *dev)
{
	int status;
	struct pci_dev *pdev = to_pci_dev(dev);

	status = pci_enable_device(pdev);
	if (status)
		return status;

	/* Re-init PCI Configuration Space */
	status = f81504_port_init(pdev);
	if (status)
		return status;

	return 0;
}
#endif

static const struct pci_device_id f81504_dev_table[] = {
	/* Fintek PCI serial cards */
	{PCI_DEVICE(FINTEK_VID, FINTEK_F81504), .driver_data = 4},
	{PCI_DEVICE(FINTEK_VID, FINTEK_F81508), .driver_data = 8},
	{PCI_DEVICE(FINTEK_VID, FINTEK_F81512), .driver_data = 12},
	{}
};

static SIMPLE_DEV_PM_OPS(f81504_pm_ops, f81504_suspend, f81504_resume);

static struct pci_driver f81504_driver = {
	.name = F81504_DRIVER_NAME,
	.probe = f81504_probe,
	.remove = f81504_remove,
	.driver		= {
		.pm	= &f81504_pm_ops,
		.owner	= THIS_MODULE,
	},
	.id_table = f81504_dev_table,
};

module_pci_driver(f81504_driver);

MODULE_DEVICE_TABLE(pci, f81504_dev_table);
MODULE_DESCRIPTION(F81504_DEV_DESC);
MODULE_AUTHOR("Peter Hong <Peter_Hong@fintek.com.tw>");
MODULE_LICENSE("GPL");
