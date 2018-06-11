// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/* Linux ARCnet driver for com 20020.
 *
 * datasheet:
 * http://ww1.microchip.com/downloads/en/DeviceDoc/200223vrevc.pdf
 * http://ww1.microchip.com/downloads/en/DeviceDoc/20020.pdf
 *
 * Supported chip version:
 * - com20020
 * - com20022
 * - com20022I-3v3
 */
#include <linux/module.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/netdevice.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/sizes.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include "arcdevice.h"
#include "com20020.h"

/* Reset (5 * xTalFreq), minimal com20020 xTal is 10Mhz */
#define RESET_DELAY 500

static unsigned int io_arc_inb(int addr, int offset)
{
	return ioread8((void *__iomem) addr + offset);
}

static void io_arc_outb(int value, int addr, int offset)
{
	iowrite8(value, (void *__iomem)addr + offset);
}

static void io_arc_insb(int  addr, int offset, void *buffer, int count)
{
	ioread8_rep((void *__iomem) (addr + offset), buffer, count);
}

static void io_arc_outsb(int addr, int offset, void *buffer, int count)
{
	iowrite8_rep((void *__iomem) (addr + offset), buffer, count);
}

enum com20020_xtal_freq {
	freq_10Mhz = 10,
	freq_20Mhz = 20,
};

enum com20020_arcnet_speed {
	arc_speed_10M_bps = 10000000,
	arc_speed_5M_bps = 5000000,
	arc_speed_2M50_bps = 2500000,
	arc_speed_1M25_bps = 1250000,
	arc_speed_625K_bps = 625000,
	arc_speed_312K5_bps = 312500,
	arc_speed_156K25_bps = 156250,
};

enum com20020_timeout {
	arc_timeout_328us =   328000,
	arc_timeout_164us = 164000,
	arc_timeout_82us =  82000,
	arc_timeout_20u5s =  20500,
};

static int setup_clock(int *clockp, int *clockm, int xtal, int arcnet_speed)
{
	int pll_factor, req_clock_frq = 20;

	switch (arcnet_speed) {
	case arc_speed_10M_bps:
		req_clock_frq = 80;
		*clockp = 0;
		break;
	case arc_speed_5M_bps:
		req_clock_frq = 40;
		*clockp = 0;
		break;
	case arc_speed_2M50_bps:
		*clockp = 0;
		break;
	case arc_speed_1M25_bps:
		*clockp = 1;
		break;
	case arc_speed_625K_bps:
		*clockp = 2;
		break;
	case arc_speed_312K5_bps:
		*clockp = 3;
		break;
	case arc_speed_156K25_bps:
		*clockp = 4;
		break;
	default:
		return -EINVAL;
	}

	if (xtal != freq_10Mhz && xtal != freq_20Mhz)
		return -EINVAL;

	pll_factor = (unsigned int)req_clock_frq / xtal;

	switch (pll_factor) {
	case 1:
		*clockm = 0;
		break;
	case 2:
		*clockm = 1;
		break;
	case 4:
		*clockm = 3;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int setup_timeout(int *timeout)
{
	switch (*timeout) {
	case arc_timeout_328us:
		*timeout = 0;
		break;
	case arc_timeout_164us:
		*timeout = 1;
		break;
	case arc_timeout_82us:
		*timeout = 2;
		break;
	case arc_timeout_20u5s:
		*timeout = 3;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int com20020_probe(struct platform_device *pdev)
{
	struct device_node *np;
	struct net_device *dev;
	struct arcnet_local *lp;
	struct resource res, *iores;
	int ret, phy_reset;
	u32 timeout, xtal, arc_speed;
	int clockp, clockm;
	bool backplane = false;
	int ioaddr;

	np = pdev->dev.of_node;

	iores = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	ret = of_address_to_resource(np, 0, &res);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "timeout-ns", &timeout);
	if (ret) {
		dev_err(&pdev->dev, "timeout is required param");
		return ret;
	}

	ret = of_property_read_u32(np, "smsc,xtal-mhz", &xtal);
	if (ret) {
		dev_err(&pdev->dev, "xtal-mhz is required param");
		return ret;
	}

	ret = of_property_read_u32(np, "bus-speed-bps", &arc_speed);
	if (ret) {
		dev_err(&pdev->dev, "Bus speed is required param");
		return ret;
	}

	if (of_property_read_bool(np, "smsc,backplane-enabled"))
		backplane = true;

	phy_reset = of_get_named_gpio(np, "reset-gpios", 0);
	if (!gpio_is_valid(phy_reset)) {
		dev_err(&pdev->dev, "reset gpio not valid");
		return phy_reset;
	}

	ret = devm_gpio_request_one(&pdev->dev, phy_reset, GPIOF_OUT_INIT_LOW,
				    "arcnet-reset");
	if (ret) {
		dev_err(&pdev->dev, "failed to get phy reset gpio: %d\n", ret);
		return ret;
	}

	dev = alloc_arcdev(NULL);
	dev->netdev_ops = &com20020_netdev_ops;
	lp = netdev_priv(dev);

	lp->card_flags = ARC_CAN_10MBIT;

	/* Peak random address,
	 * if required user could set a new-one in userspace
	 */
	get_random_bytes(dev->dev_addr, dev->addr_len);

	if (!devm_request_mem_region(&pdev->dev, res.start, resource_size(&res),
				     lp->card_name))
		return -EBUSY;

	ioaddr = (int)devm_ioremap(&pdev->dev, iores->start,
				 resource_size(iores));
	if (!ioaddr) {
		dev_err(&pdev->dev, "ioremap fallied\n");
		return -ENOMEM;
	}

	gpio_set_value_cansleep(phy_reset, 0);
	ndelay(RESET_DELAY);
	gpio_set_value_cansleep(phy_reset, 1);

	lp->hw.arc_inb = io_arc_inb;
	lp->hw.arc_outb = io_arc_outb;
	lp->hw.arc_insb = io_arc_insb;
	lp->hw.arc_outsb = io_arc_outsb;

	/* ARCNET controller needs this access to detect bustype */
	lp->hw.arc_outb(0x00, ioaddr, COM20020_REG_W_COMMAND);
	lp->hw.arc_inb(ioaddr, COM20020_REG_R_DIAGSTAT);

	dev->base_addr = (unsigned long)ioaddr;

	dev->irq = of_get_named_gpio(np, "interrupts", 0);
	if (dev->irq == -EPROBE_DEFER) {
		return dev->irq;
	} else if (!gpio_is_valid(dev->irq)) {
		dev_err(&pdev->dev, "irq-gpios not valid !");
		return -EIO;
	}
	dev->irq = gpio_to_irq(dev->irq);

	ret = setup_clock(&clockp, &clockm, xtal, arc_speed);
	if (ret) {
		dev_err(&pdev->dev,
			"Impossible use oscillator:%dMhz and arcnet bus speed:%dKbps",
			xtal, arc_speed / 1000);
		return ret;
	}

	ret = setup_timeout(&timeout);
	if (ret) {
		dev_err(&pdev->dev, "Timeout:%d is not valid value", timeout);
		return ret;
	}

	lp->backplane = (int)backplane;
	lp->timeout = timeout;
	lp->clockm = clockm;
	lp->clockp = clockp;
	lp->hw.owner = THIS_MODULE;

	if (lp->hw.arc_inb(ioaddr, COM20020_REG_R_STATUS) == 0xFF) {
		ret = -EIO;
		goto err_release_mem;
	}

	if (com20020_check(dev)) {
		ret = -EIO;
		goto err_release_mem;
	}

	ret = com20020_found(dev, IRQF_TRIGGER_FALLING);
	if (ret)
		goto err_release_mem;

	dev_dbg(&pdev->dev, "probe Done\n");
	return 0;

err_release_mem:
	devm_iounmap(&pdev->dev, (void __iomem *)ioaddr);
	devm_release_mem_region(&pdev->dev, res.start, resource_size(&res));
	dev_err(&pdev->dev, "probe failed!\n");
	return ret;
}

static const struct of_device_id of_com20020_match[] = {
	{ .compatible = "smsc,com20020",	},
	{ },
};

MODULE_DEVICE_TABLE(of, of_com20020_match);

static struct platform_driver of_com20020_driver = {
	.driver			= {
		.name		= "com20020-memory-bus",
		.of_match_table = of_com20020_match,
	},
	.probe			= com20020_probe,
};

static int com20020_init(void)
{
	return platform_driver_register(&of_com20020_driver);
}
late_initcall(com20020_init);

MODULE_LICENSE("GPL");
