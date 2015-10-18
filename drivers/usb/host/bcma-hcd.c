/*
 * Broadcom specific Advanced Microcontroller Bus
 * Broadcom USB-core driver (BCMA bus glue)
 *
 * Copyright 2011-2015 Hauke Mehrtens <hauke@hauke-m.de>
 * Copyright 2015 Felix Fietkau <nbd@openwrt.org>
 *
 * Based on ssb-ohci driver
 * Copyright 2007 Michael Buesch <m@bues.ch>
 *
 * Derived from the OHCI-PCI driver
 * Copyright 1999 Roman Weissgaerber
 * Copyright 2000-2002 David Brownell
 * Copyright 1999 Linus Torvalds
 * Copyright 1999 Gregory P. Smith
 *
 * Derived from the USBcore related parts of Broadcom-SB
 * Copyright 2005-2011 Broadcom Corporation
 *
 * Licensed under the GNU/GPL. See COPYING for details.
 */
#include <linux/bcma/bcma.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/usb/ehci_pdriver.h>
#include <linux/usb/ohci_pdriver.h>
#include <linux/usb/xhci_pdriver.h>

MODULE_AUTHOR("Hauke Mehrtens");
MODULE_DESCRIPTION("Common USB driver for BCMA Bus");
MODULE_LICENSE("GPL");

struct bcma_hcd_device {
	struct bcma_device *core;
	struct platform_device *ehci_dev;
	struct platform_device *ohci_dev;
	struct platform_device *xhci_dev;
};

/* Wait for bitmask in a register to get set or cleared.
 * timeout is in units of ten-microseconds.
 */
static int bcma_wait_bits(struct bcma_device *dev, u16 reg, u32 bitmask,
			  int timeout)
{
	int i;
	u32 val;

	for (i = 0; i < timeout; i++) {
		val = bcma_read32(dev, reg);
		if ((val & bitmask) == bitmask)
			return 0;
		udelay(10);
	}

	return -ETIMEDOUT;
}

static void bcma_hcd_4716wa(struct bcma_device *dev)
{
#ifdef CONFIG_BCMA_DRIVER_MIPS
	/* Work around for 4716 failures. */
	if (dev->bus->chipinfo.id == 0x4716) {
		u32 tmp;

		tmp = bcma_cpu_clock(&dev->bus->drv_mips);
		if (tmp >= 480000000)
			tmp = 0x1846b; /* set CDR to 0x11(fast) */
		else if (tmp == 453000000)
			tmp = 0x1046b; /* set CDR to 0x10(slow) */
		else
			tmp = 0;

		/* Change Shim mdio control reg to fix host not acking at
		 * high frequencies
		 */
		if (tmp) {
			bcma_write32(dev, 0x524, 0x1); /* write sel to enable */
			udelay(500);

			bcma_write32(dev, 0x524, tmp);
			udelay(500);
			bcma_write32(dev, 0x524, 0x4ab);
			udelay(500);
			bcma_read32(dev, 0x528);
			bcma_write32(dev, 0x528, 0x80000000);
		}
	}
#endif /* CONFIG_BCMA_DRIVER_MIPS */
}

/* based on arch/mips/brcm-boards/bcm947xx/pcibios.c */
static void bcma_hcd_init_chip_mips(struct bcma_device *dev)
{
	u32 tmp;

	/*
	 * USB 2.0 special considerations:
	 *
	 * 1. Since the core supports both OHCI and EHCI functions, it must
	 *    only be reset once.
	 *
	 * 2. In addition to the standard SI reset sequence, the Host Control
	 *    Register must be programmed to bring the USB core and various
	 *    phy components out of reset.
	 */
	if (!bcma_core_is_enabled(dev)) {
		bcma_core_enable(dev, 0);
		mdelay(10);
		if (dev->id.rev >= 5) {
			/* Enable Misc PLL */
			tmp = bcma_read32(dev, 0x1e0);
			tmp |= 0x100;
			bcma_write32(dev, 0x1e0, tmp);
			if (bcma_wait_bits(dev, 0x1e0, 1 << 24, 100))
				printk(KERN_EMERG "Failed to enable misc PPL!\n");

			/* Take out of resets */
			bcma_write32(dev, 0x200, 0x4ff);
			udelay(25);
			bcma_write32(dev, 0x200, 0x6ff);
			udelay(25);

			/* Make sure digital and AFE are locked in USB PHY */
			bcma_write32(dev, 0x524, 0x6b);
			udelay(50);
			tmp = bcma_read32(dev, 0x524);
			udelay(50);
			bcma_write32(dev, 0x524, 0xab);
			udelay(50);
			tmp = bcma_read32(dev, 0x524);
			udelay(50);
			bcma_write32(dev, 0x524, 0x2b);
			udelay(50);
			tmp = bcma_read32(dev, 0x524);
			udelay(50);
			bcma_write32(dev, 0x524, 0x10ab);
			udelay(50);
			tmp = bcma_read32(dev, 0x524);

			if (bcma_wait_bits(dev, 0x528, 0xc000, 10000)) {
				tmp = bcma_read32(dev, 0x528);
				printk(KERN_EMERG
				       "USB20H mdio_rddata 0x%08x\n", tmp);
			}
			bcma_write32(dev, 0x528, 0x80000000);
			tmp = bcma_read32(dev, 0x314);
			udelay(265);
			bcma_write32(dev, 0x200, 0x7ff);
			udelay(10);

			/* Take USB and HSIC out of non-driving modes */
			bcma_write32(dev, 0x510, 0);
		} else {
			bcma_write32(dev, 0x200, 0x7ff);

			udelay(1);
		}

		bcma_hcd_4716wa(dev);
	}
}

static void bcma_hcd_init_chip_arm_phy(struct bcma_device *dev)
{
	struct bcma_device *arm_core;
	void __iomem *dmu;

	arm_core = bcma_find_core(dev->bus, BCMA_CORE_ARMCA9);
	if (!arm_core) {
		dev_err(&dev->dev, "can not find ARM Cortex A9 ihost core\n");
		return;
	}

	dmu = ioremap_nocache(arm_core->addr_s[0], 0x1000);
	if (!dmu) {
		dev_err(&dev->dev, "can not map ARM Cortex A9 ihost core\n");
		return;
	}

	/* Unlock DMU PLL settings */
	iowrite32(0x0000ea68, dmu + 0x180);

	/* Write USB 2.0 PLL control setting */
	iowrite32(0x00dd10c3, dmu + 0x164);

	/* Lock DMU PLL settings */
	iowrite32(0x00000000, dmu + 0x180);

	iounmap(dmu);
}

static void bcma_hcd_init_chip_arm_hc(struct bcma_device *dev)
{
	u32 val;

	/*
	 * Delay after PHY initialized to ensure HC is ready to be configured
	 */
	usleep_range(1000, 2000);

	/* Set packet buffer OUT threshold */
	val = bcma_read32(dev, 0x94);
	val &= 0xffff;
	val |= 0x80 << 16;
	bcma_write32(dev, 0x94, val);

	/* Enable break memory transfer */
	val = bcma_read32(dev, 0x9c);
	val |= 1;
	bcma_write32(dev, 0x9c, val);
}

static void bcma_hcd_init_chip_arm(struct bcma_device *dev)
{
	bcma_core_enable(dev, 0);

	if (dev->bus->chipinfo.id == BCMA_CHIP_ID_BCM4707 ||
	    dev->bus->chipinfo.id == BCMA_CHIP_ID_BCM53018) {
		if (dev->bus->chipinfo.pkg == BCMA_PKG_ID_BCM4707 ||
		    dev->bus->chipinfo.pkg == BCMA_PKG_ID_BCM4708)
			bcma_hcd_init_chip_arm_phy(dev);

		bcma_hcd_init_chip_arm_hc(dev);
	}
}

static void bcma_hci_platform_power_gpio(struct bcma_device *dev, bool val)
{
	enum of_gpio_flags of_flags;
	int gpio;

	gpio = of_get_named_gpio_flags(dev->dev.of_node, "vcc-gpio", 0, &of_flags);
	if (!gpio_is_valid(gpio))
		return;

	if (val) {
		unsigned long flags = 0;
		bool active_low = !!(of_flags & OF_GPIO_ACTIVE_LOW);

		flags |= active_low ? GPIOF_ACTIVE_LOW : 0;
		flags |= active_low ? GPIOF_INIT_LOW : GPIOF_INIT_HIGH;
		gpio_request_one(gpio, flags, "bcma-hcd-gpio");
	} else {
		gpiod_set_value(gpio_to_desc(gpio), 0);
		gpio_free(gpio);
	}
}

static const struct usb_ehci_pdata ehci_pdata = {
};

static const struct usb_ohci_pdata ohci_pdata = {
};

static const struct usb_xhci_pdata xhci_pdata = {
	.usb3_fake_doorbell = 1
};

static struct platform_device *bcma_hcd_create_pdev(struct bcma_device *dev,
						    const char *name, u32 addr,
						    const void *data,
						    size_t size)
{
	struct platform_device *hci_dev;
	struct resource hci_res[2];
	int ret;

	memset(hci_res, 0, sizeof(hci_res));

	hci_res[0].start = addr;
	hci_res[0].end = hci_res[0].start + 0x1000 - 1;
	hci_res[0].flags = IORESOURCE_MEM;

	hci_res[1].start = dev->irq;
	hci_res[1].flags = IORESOURCE_IRQ;

	hci_dev = platform_device_alloc(name, 0);
	if (!hci_dev)
		return ERR_PTR(-ENOMEM);

	hci_dev->dev.parent = &dev->dev;
	hci_dev->dev.dma_mask = &hci_dev->dev.coherent_dma_mask;

	ret = platform_device_add_resources(hci_dev, hci_res,
					    ARRAY_SIZE(hci_res));
	if (ret)
		goto err_alloc;
	if (data)
		ret = platform_device_add_data(hci_dev, data, size);
	if (ret)
		goto err_alloc;
	ret = platform_device_add(hci_dev);
	if (ret)
		goto err_alloc;

	return hci_dev;

err_alloc:
	platform_device_put(hci_dev);
	return ERR_PTR(ret);
}

static int bcma_hcd_usb20_init(struct bcma_hcd_device *usb_dev)
{
	struct bcma_device *dev = usb_dev->core;
	struct bcma_chipinfo *chipinfo = &dev->bus->chipinfo;
	u32 ohci_addr;
	int err;

	if (dma_set_mask_and_coherent(dev->dma_dev, DMA_BIT_MASK(32)))
		return -EOPNOTSUPP;

	switch (dev->id.id) {
	case BCMA_CORE_NS_USB20:
		bcma_hcd_init_chip_arm(dev);
		break;
	case BCMA_CORE_USB20_HOST:
		bcma_hcd_init_chip_mips(dev);
		break;
	default:
		return -ENODEV;
	}

	/* In AI chips EHCI is addrspace 0, OHCI is 1 */
	ohci_addr = dev->addr_s[0];
	if ((chipinfo->id == BCMA_CHIP_ID_BCM5357 ||
	     chipinfo->id == BCMA_CHIP_ID_BCM4749)
	    && chipinfo->rev == 0)
		ohci_addr = 0x18009000;

	usb_dev->ohci_dev = bcma_hcd_create_pdev(dev, "ohci-platform",
						 ohci_addr, &ohci_pdata,
						 sizeof(ohci_pdata));
	if (IS_ERR(usb_dev->ohci_dev))
		return PTR_ERR(usb_dev->ohci_dev);

	usb_dev->ehci_dev = bcma_hcd_create_pdev(dev, "ehci-platform",
						 dev->addr, &ehci_pdata,
						 sizeof(ehci_pdata));
	if (IS_ERR(usb_dev->ehci_dev)) {
		err = PTR_ERR(usb_dev->ehci_dev);
		goto err_unregister_ohci_dev;
	}

	return 0;

err_unregister_ohci_dev:
	platform_device_unregister(usb_dev->ohci_dev);
	return err;
}

static bool bcma_wait_reg(struct bcma_bus *bus, void __iomem *addr, u32 mask,
			  u32 value, int timeout)
{
	unsigned long deadline = jiffies + timeout;
	u32 val;

	do {
		val = readl(addr);
		if ((val & mask) == value)
			return true;
		cpu_relax();
		udelay(10);
	} while (!time_after_eq(jiffies, deadline));

	pr_err("Timeout waiting for register %p\n", addr);

	return false;
}

static void bcma_hcd_usb30_phy_init(struct bcma_hcd_device *bcma_hcd)
{
	struct bcma_device *core = bcma_hcd->core;
	struct bcma_bus *bus = core->bus;
	struct bcma_chipinfo *chipinfo = &bus->chipinfo;
	struct bcma_drv_cc_b *ccb = &bus->drv_cc_b;
	struct bcma_device *arm_core;
	void __iomem *dmu = NULL;
	u32 cru_straps_ctrl;

	if (chipinfo->id != BCMA_CHIP_ID_BCM4707 &&
	    chipinfo->id != BCMA_CHIP_ID_BCM53018)
		return;

	arm_core = bcma_find_core(bus, BCMA_CORE_ARMCA9);
	if (!arm_core)
		return;

	dmu = ioremap_nocache(arm_core->addr_s[0], 0x1000);
	if (!dmu)
		goto out;

	/* Check strapping of PCIE/USB3 SEL */
	cru_straps_ctrl = ioread32(dmu + 0x2a0);
	if ((cru_straps_ctrl & 0x10) == 0)
		goto out;

	/* Perform USB3 system soft reset */
	bcma_awrite32(core, BCMA_RESET_CTL, BCMA_RESET_CTL_RESET);

	/* Enable MDIO. Setting MDCDIV as 26  */
	iowrite32(0x0000009a, ccb->mii + 0x000);
	udelay(2);

	switch (chipinfo->id) {
	case BCMA_CHIP_ID_BCM4707:
		if (chipinfo->rev == 4) {
			/* For NS-B0, USB3 PLL Block */
			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x587e8000, ccb->mii + 0x004);

			/* Clear ana_pllSeqStart */
			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x58061000, ccb->mii + 0x004);

			/* CMOS Divider ratio to 25 */
			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x582a6400, ccb->mii + 0x004);

			/* Asserting PLL Reset */
			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x582ec000, ccb->mii + 0x004);

			/* Deaaserting PLL Reset */
			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x582e8000, ccb->mii + 0x004);

			/* Deasserting USB3 system reset */
			bcma_awrite32(core, BCMA_RESET_CTL, 0);

			/* Set ana_pllSeqStart */
			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x58069000, ccb->mii + 0x004);

			/* RXPMD block */
			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x587e8020, ccb->mii + 0x004);

			/* CDR int loop locking BW to 1 */
			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x58120049, ccb->mii + 0x004);

			/* CDR int loop acquisition BW to 1 */
			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x580e0049, ccb->mii + 0x004);

			/* CDR prop loop BW to 1 */
			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x580a005c, ccb->mii + 0x004);

			/* Waiting MII Mgt interface idle */
			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
		} else {
			/* PLL30 block */
			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x587e8000, ccb->mii + 0x004);

			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x582a6400, ccb->mii + 0x004);

			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x587e80e0, ccb->mii + 0x004);

			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x580a009c, ccb->mii + 0x004);

			/* Enable SSC */
			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x587e8040, ccb->mii + 0x004);

			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x580a21d3, ccb->mii + 0x004);

			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
			iowrite32(0x58061003, ccb->mii + 0x004);

			/* Waiting MII Mgt interface idle */
			bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);

			/* Deasserting USB3 system reset */
			bcma_awrite32(core, BCMA_RESET_CTL, 0);
		}
		break;
	case BCMA_CHIP_ID_BCM53018:
		/* USB3 PLL Block */
		bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
		iowrite32(0x587e8000, ccb->mii + 0x004);

		/* Assert Ana_Pllseq start */
		bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
		iowrite32(0x58061000, ccb->mii + 0x004);

		/* Assert CML Divider ratio to 26 */
		bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
		iowrite32(0x582a6400, ccb->mii + 0x004);

		/* Asserting PLL Reset */
		bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
		iowrite32(0x582ec000, ccb->mii + 0x004);

		/* Deaaserting PLL Reset */
		bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
		iowrite32(0x582e8000, ccb->mii + 0x004);

		/* Waiting MII Mgt interface idle */
		bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);

		/* Deasserting USB3 system reset */
		bcma_awrite32(core, BCMA_RESET_CTL, 0);

		/* PLL frequency monitor enable */
		bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
		iowrite32(0x58069000, ccb->mii + 0x004);

		/* PIPE Block */
		bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
		iowrite32(0x587e8060, ccb->mii + 0x004);

		/* CMPMAX & CMPMINTH setting */
		bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
		iowrite32(0x580af30d, ccb->mii + 0x004);

		/* DEGLITCH MIN & MAX setting */
		bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
		iowrite32(0x580e6302, ccb->mii + 0x004);

		/* TXPMD block */
		bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
		iowrite32(0x587e8040, ccb->mii + 0x004);

		/* Enabling SSC */
		bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);
		iowrite32(0x58061003, ccb->mii + 0x004);

		/* Waiting MII Mgt interface idle */
		bcma_wait_reg(bus, ccb->mii + 0x000, 0x0100, 0x0000, 1000);

		break;
	}
out:
	if (dmu)
		iounmap(dmu);
}

static int bcma_hcd_usb30_init(struct bcma_hcd_device *bcma_hcd)
{
	struct bcma_device *core = bcma_hcd->core;

	bcma_core_enable(core, 0);

	bcma_hcd_usb30_phy_init(bcma_hcd);

	bcma_hcd->xhci_dev = bcma_hcd_create_pdev(core, "xhci-hcd", core->addr,
						  &xhci_pdata,
						  sizeof(xhci_pdata));
	if (IS_ERR(bcma_hcd->ohci_dev))
		return PTR_ERR(bcma_hcd->ohci_dev);

	return 0;
}

static int bcma_hcd_probe(struct bcma_device *dev)
{
	int err;
	struct bcma_hcd_device *usb_dev;

	/* TODO: Probably need checks here; is the core connected? */

	usb_dev = devm_kzalloc(&dev->dev, sizeof(struct bcma_hcd_device),
			       GFP_KERNEL);
	if (!usb_dev)
		return -ENOMEM;
	usb_dev->core = dev;

	bcma_hci_platform_power_gpio(dev, true);

	switch (dev->id.id) {
	case BCMA_CORE_USB20_HOST:
	case BCMA_CORE_NS_USB20:
		err = bcma_hcd_usb20_init(usb_dev);
		if (err)
			return err;
		break;
	case BCMA_CORE_NS_USB30:
		err = bcma_hcd_usb30_init(usb_dev);
		if (err)
			return err;
		break;
	default:
		return -ENODEV;
	}

	bcma_set_drvdata(dev, usb_dev);
	return 0;
}

static void bcma_hcd_remove(struct bcma_device *dev)
{
	struct bcma_hcd_device *usb_dev = bcma_get_drvdata(dev);
	struct platform_device *ohci_dev = usb_dev->ohci_dev;
	struct platform_device *ehci_dev = usb_dev->ehci_dev;
	struct platform_device *xhci_dev = usb_dev->xhci_dev;

	if (ohci_dev)
		platform_device_unregister(ohci_dev);
	if (ehci_dev)
		platform_device_unregister(ehci_dev);
	if (xhci_dev)
		platform_device_unregister(xhci_dev);

	bcma_core_disable(dev, 0);
}

static void bcma_hcd_shutdown(struct bcma_device *dev)
{
	bcma_hci_platform_power_gpio(dev, false);
	bcma_core_disable(dev, 0);
}

#ifdef CONFIG_PM

static int bcma_hcd_suspend(struct bcma_device *dev)
{
	bcma_hci_platform_power_gpio(dev, false);
	bcma_core_disable(dev, 0);

	return 0;
}

static int bcma_hcd_resume(struct bcma_device *dev)
{
	bcma_hci_platform_power_gpio(dev, true);
	bcma_core_enable(dev, 0);

	return 0;
}

#else /* !CONFIG_PM */
#define bcma_hcd_suspend	NULL
#define bcma_hcd_resume	NULL
#endif /* CONFIG_PM */

static const struct bcma_device_id bcma_hcd_table[] = {
	BCMA_CORE(BCMA_MANUF_BCM, BCMA_CORE_USB20_HOST, BCMA_ANY_REV, BCMA_ANY_CLASS),
	BCMA_CORE(BCMA_MANUF_BCM, BCMA_CORE_NS_USB20, BCMA_ANY_REV, BCMA_ANY_CLASS),
	BCMA_CORE(BCMA_MANUF_BCM, BCMA_CORE_NS_USB30, BCMA_ANY_REV, BCMA_ANY_CLASS),
	{},
};
MODULE_DEVICE_TABLE(bcma, bcma_hcd_table);

static struct bcma_driver bcma_hcd_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= bcma_hcd_table,
	.probe		= bcma_hcd_probe,
	.remove		= bcma_hcd_remove,
	.shutdown	= bcma_hcd_shutdown,
	.suspend	= bcma_hcd_suspend,
	.resume		= bcma_hcd_resume,
};

static int __init bcma_hcd_init(void)
{
	return bcma_driver_register(&bcma_hcd_driver);
}
module_init(bcma_hcd_init);

static void __exit bcma_hcd_exit(void)
{
	bcma_driver_unregister(&bcma_hcd_driver);
}
module_exit(bcma_hcd_exit);
