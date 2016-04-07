/*
 * Microchip PIC32 MUSB Dual-Role Controller "glue layer".
 *
 * Cristian Birsan <cristian.birsan@microchip.com>
 * Purna Chandra Mandal <purna.mandal@microchip.com>
 * Copyright (C) 2015 Microchip Technology Inc.  All rights reserved.
 *
 * Based on the am35x and dsps "glue layer" code.
 *
 * This program is free software; you can distribute it and/or modify it
 * under the terms of the GNU General Public License (Version 2) as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/usb/of.h>

#include "musb_core.h"

#define MUSB_SOFTRST		0x7f
#define  MUSB_SOFTRST_NRST      BIT(0)
#define  MUSB_SOFTRST_NRSTX     BIT(1)

/* USB Clock & Reset Control */
#define USBCRCON		0x0
#define  USBCRCON_USBWKUPEN	BIT(0)  /* Enable remote wakeup interrupt */
#define  USBCRCON_USBRIE	BIT(1)  /* Enable Remote resume interrupt */
#define  USBCRCON_USBIE		BIT(2)  /* Enable USB General interrupt */
#define  USBCRCON_SENDMONEN     BIT(3)  /* Enable Session End VBUS monitoring */
#define  USBCRCON_BSVALMONEN    BIT(4)  /* Enable B-Device VBUS monitoring */
#define  USBCRCON_ASVALMONEN    BIT(5)  /* Enable A-Device VBUS monitoring */
#define  USBCRCON_VBUSMONEN     BIT(6)  /* Enable VBUS monitoring */
#define  USBCRCON_PHYIDEN	BIT(7)  /* Enabale USBPhy USBID monitoring */
#define  USBCRCON_USBIDVAL	BIT(8)  /* USBID override value */
#define  USBCRCON_USBIDOVEN	BIT(9)  /* Enable USBID override */
#define  USBCRCON_USBWKUP	BIT(24) /* Remote wakeup status */
#define  USBCRCON_USBRF		BIT(25) /* USB Remote resume status */
#define  USBCRCON_USBIF		BIT(26) /* USB General interrupt status */

#define PIC32_TX_EP_MASK	0xffff	/* EP0 + 15 Tx EPs */
#define PIC32_RX_EP_MASK	0xfffe	/* 15 Rx EPs */

#define	POLL_SECONDS		2

struct pic32_musb {
	void __iomem		*cru;
	struct clk		*clk;
	int			oc_irq;
	struct platform_device	*platdev;
	struct timer_list	timer;		/* otg_workaround timer */
	unsigned long		last_timer;	/* last timer data for */
};

static irqreturn_t pic32_over_current(int irq, void *d)
{
	struct device *dev = d;

	dev_err(dev, "USB Host over-current detected !\n");

	return IRQ_HANDLED;
}

static void pic32_musb_enable(struct musb *musb)
{
	struct device *dev = musb->controller;
	struct pic32_musb *glue = dev_get_drvdata(dev->parent);

	/* Enable additional interrupts */
	enable_irq(glue->oc_irq);
}

static void pic32_musb_disable(struct musb *musb)
{
	struct device *dev = musb->controller;
	struct pic32_musb *glue = dev_get_drvdata(dev->parent);

	musb_writeb(musb->mregs, MUSB_DEVCTL, 0);

	/* Disable additional interrupts */
	disable_irq(glue->oc_irq);
}

static void pic32_musb_set_vbus(struct musb *musb, int is_on)
{
	WARN_ON(is_on && is_peripheral_active(musb));
}

static void otg_timer(unsigned long _musb)
{
	struct musb *musb = (void *)_musb;
	struct device *dev = musb->controller;
	struct pic32_musb *glue;
	int skip_session = 0;
	unsigned long flags;
	u8 devctl;

	glue = dev_get_drvdata(dev->parent);
	/*
	 * We poll because IP's won't expose several OTG-critical
	 * status change events (from the transceiver) otherwise.
	 */
	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
	dev_dbg(dev, "Poll devctl %02x (%s)\n", devctl,
		usb_otg_state_string(musb->xceiv->otg->state));

	spin_lock_irqsave(&musb->lock, flags);
	switch (musb->xceiv->otg->state) {
	case OTG_STATE_A_WAIT_BCON:
		musb_writeb(musb->mregs, MUSB_DEVCTL, 0);
		skip_session = 1;
		/* fall */
	case OTG_STATE_A_IDLE:
	case OTG_STATE_B_IDLE:
		if (devctl & MUSB_DEVCTL_BDEVICE) {
			musb->xceiv->otg->state = OTG_STATE_B_IDLE;
			MUSB_DEV_MODE(musb);
		} else {
			musb->xceiv->otg->state = OTG_STATE_A_IDLE;
			MUSB_HST_MODE(musb);
		}
		if (!(devctl & MUSB_DEVCTL_SESSION) && !skip_session)
			musb_writeb(musb->mregs,
				    MUSB_DEVCTL, MUSB_DEVCTL_SESSION);
		mod_timer(&glue->timer, jiffies + POLL_SECONDS * HZ);
		break;
	case OTG_STATE_A_WAIT_VFALL:
		musb->xceiv->otg->state = OTG_STATE_A_WAIT_VRISE;
		break;
	default:
		break;
	}
	spin_unlock_irqrestore(&musb->lock, flags);
}

static void pic32_musb_try_idle(struct musb *musb, unsigned long timeout)
{
	struct device *dev = musb->controller;
	struct pic32_musb *glue = dev_get_drvdata(dev);

	if (timeout == 0)
		timeout = jiffies + msecs_to_jiffies(3);

	/* Never idle if active, or when VBUS timeout is not set as host */
	if (musb->is_active ||
	    (musb->a_wait_bcon == 0 &&
	     musb->xceiv->otg->state == OTG_STATE_A_WAIT_BCON)) {
		dev_dbg(dev, "%s active, deleting timer\n",
			usb_otg_state_string(musb->xceiv->otg->state));
		del_timer(&glue->timer);
		glue->last_timer = jiffies;
		return;
	}

	if (musb->port_mode != MUSB_PORT_MODE_DUAL_ROLE)
		return;

	if (!musb->g.dev.driver)
		return;

	if (time_after(glue->last_timer, timeout) &&
	    timer_pending(&glue->timer)) {
		dev_dbg(dev, "Longer idle timer already pending, ignoring\n");
		return;
	}
	glue->last_timer = timeout;

	dev_dbg(dev, "%s inactive, starting idle timer for %u ms\n",
		usb_otg_state_string(musb->xceiv->otg->state),
		jiffies_to_msecs(timeout - jiffies));
	mod_timer(&glue->timer, timeout);
}

static irqreturn_t pic32_musb_interrupt(int irq, void *hci)
{
	struct musb  *musb = hci;
	struct device *dev = musb->controller;
	struct pic32_musb *glue;
	unsigned long flags;
	irqreturn_t ret = IRQ_NONE;

	glue = dev_get_drvdata(dev->parent);
	spin_lock_irqsave(&musb->lock, flags);

	/* Get endpoint interrupts */
	musb->int_rx = musb_readw(musb->mregs, MUSB_INTRRX) & PIC32_RX_EP_MASK;
	musb->int_tx = musb_readw(musb->mregs, MUSB_INTRTX) & PIC32_TX_EP_MASK;

	/* Get usb core interrupts */
	musb->int_usb = musb_readb(musb->mregs, MUSB_INTRUSB);
	if (!musb->int_usb && !(musb->int_rx || musb->int_tx)) {
		dev_err(dev, "Got USB spurious interrupt !\n");
		goto irq_done;
	}

	if (is_host_active(musb) && musb->int_usb & MUSB_INTR_BABBLE)
		dev_err(dev, "CAUTION: Babble interrupt occurred!\n");

	/* Drop spurious RX and TX if device is disconnected */
	if (musb->int_usb & MUSB_INTR_DISCONNECT) {
		musb->int_tx = 0;
		musb->int_rx = 0;
	}

	if (musb->int_tx || musb->int_rx || musb->int_usb)
		ret |= musb_interrupt(musb);

irq_done:
	/* Poll for ID change in OTG port mode */
	if (musb->xceiv->otg->state == OTG_STATE_B_IDLE &&
	    musb->port_mode == MUSB_PORT_MODE_DUAL_ROLE)
		mod_timer(&glue->timer, jiffies + POLL_SECONDS * HZ);

	spin_unlock_irqrestore(&musb->lock, flags);

	return ret;
}

static int pic32_musb_set_mode(struct musb *musb, u8 mode)
{
	struct device *dev = musb->controller;
	struct pic32_musb *glue;
	u32 crcon;

	glue = dev_get_drvdata(dev->parent);
	crcon = musb_readl(glue->cru, USBCRCON);
	switch (mode) {
	case MUSB_HOST:
		crcon &= ~USBCRCON_USBIDVAL;
		crcon |= USBCRCON_USBIDOVEN;
		musb_writel(glue->cru, USBCRCON, crcon);
		dev_dbg(dev, "MUSB Host mode enabled\n");
		break;

	case MUSB_PERIPHERAL:
		crcon |= USBCRCON_USBIDVAL;
		crcon |= USBCRCON_USBIDOVEN;
		musb_writel(glue->cru, USBCRCON, crcon);
		dev_dbg(dev, "MUSB Device mode enabled\n");
		break;

	case MUSB_OTG:
		/* enable OTG mode using usb_id irq */
		dev_warn(dev, "MUSB OTG mode enabled\n");
		break;

	default:
		dev_err(dev, "unsupported mode %d\n", mode);
		return -EINVAL;
	}

	return 0;
}

static int pic32_musb_init(struct musb *musb)
{
	struct device *dev = musb->controller;
	struct pic32_musb *glue;
	u16 hwvers;
	int ret;

	glue = dev_get_drvdata(dev->parent);

	/* Returns zero if e.g. not clocked */
	hwvers = musb_read_hwvers(musb->mregs);
	if (!hwvers)
		return -ENODEV;

	/* The PHY transceiver is registered using device tree */
	musb->xceiv = usb_get_phy(USB_PHY_TYPE_USB2);
	if (IS_ERR_OR_NULL(musb->xceiv))
		return -EPROBE_DEFER;

	setup_timer(&glue->timer, otg_timer, (unsigned long)musb);

	/* On-chip PHY and PLL is enabled by default */
	musb->isr = pic32_musb_interrupt;

	/* Request other interrupts */
	irq_set_status_flags(glue->oc_irq, IRQ_NOAUTOEN);
	ret = devm_request_irq(dev, glue->oc_irq, pic32_over_current,
			       0, dev_name(dev), dev);
	if (ret) {
		dev_err(dev, "failed to request irq: %d\n", ret);
		return ret;
	}

	switch (musb->port_mode) {
	case MUSB_PORT_MODE_DUAL_ROLE:
	case MUSB_PORT_MODE_HOST:
	case MUSB_PORT_MODE_GADGET:
		break;
	default:
		dev_err(dev, "unsupported mode %d\n", musb->port_mode);
		return -EINVAL;
	}

	musb_writel(glue->cru, USBCRCON,
		    USBCRCON_USBIDOVEN | USBCRCON_PHYIDEN |
		    USBCRCON_USBIE | USBCRCON_USBRIE |
		    USBCRCON_USBWKUPEN | USBCRCON_VBUSMONEN);

	/* soft reset */
	musb_writeb(musb->mregs, MUSB_SOFTRST, MUSB_SOFTRST_NRSTX);

	return 0;
}

static int pic32_musb_exit(struct musb *musb)
{
	struct device *dev = musb->controller;
	struct pic32_musb *glue;

	glue = dev_get_drvdata(dev->parent);
	del_timer_sync(&glue->timer);
	/* no way to shutdown on-chip PHY and its PLL */
	usb_put_phy(musb->xceiv);

	return 0;
}

/* PIC32 supports only 32bit read operation */
static void pic32_read_fifo(struct musb_hw_ep *hw_ep, u16 len, u8 *dst)
{
	void __iomem *fifo = hw_ep->fifo;
	u32 val, rem = len % 4;

	/* USB stack ensures dst is always 32bit aligned. */
	readsl(fifo, dst, len / 4);

	if (rem) {
		dst += len & ~0x03;
		val = musb_readl(fifo, 0);
		memcpy(dst, &val, rem);
	}
}

static const struct musb_platform_ops pic32_musb_ops = {
	.quirks         = MUSB_DMA_INVENTRA | MUSB_INDEXED_EP,
	.init		= pic32_musb_init,
	.exit		= pic32_musb_exit,
	.read_fifo	= pic32_read_fifo,
#ifdef CONFIG_USB_INVENTRA_DMA
	.dma_init	= musbhs_dma_controller_create,
	.dma_exit	= musbhs_dma_controller_destroy,
#endif
	.enable		= pic32_musb_enable,
	.disable	= pic32_musb_disable,
	.set_mode	= pic32_musb_set_mode,
	.try_idle	= pic32_musb_try_idle,
	.set_vbus	= pic32_musb_set_vbus,
};

static u64 musb_dmamask = DMA_BIT_MASK(32);

static int get_musb_port_mode(struct device *dev)
{
	enum usb_dr_mode mode;

	mode = usb_get_dr_mode(dev);
	switch (mode) {
	case USB_DR_MODE_HOST:
		return MUSB_PORT_MODE_HOST;
	case USB_DR_MODE_PERIPHERAL:
		return MUSB_PORT_MODE_GADGET;
	case USB_DR_MODE_UNKNOWN:
	case USB_DR_MODE_OTG:
	default:
		return MUSB_PORT_MODE_DUAL_ROLE;
	}
}

/* Microchip FIFO config 0 - fits in 8KB */
static struct musb_fifo_cfg pic32_musb_fifo_cfg0[] = {
	{ .hw_ep_num = 1, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 1, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 2, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 2, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 3, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 3, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 4, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 4, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 5, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 5, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 6, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 6, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 7, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 7, .style = FIFO_RX, .maxpacket = 512, },
};

/* Microchip FIFO config 1 - fits in 8KB */
static struct musb_fifo_cfg pic32_musb_fifo_cfg1[] = {
	{ .hw_ep_num = 1, .style = FIFO_TX,   .maxpacket = 512, },
	{ .hw_ep_num = 1, .style = FIFO_RX,   .maxpacket = 512, },
	{ .hw_ep_num = 2, .style = FIFO_TX,   .maxpacket = 512, },
	{ .hw_ep_num = 2, .style = FIFO_RX,   .maxpacket = 512, },
	{ .hw_ep_num = 3, .style = FIFO_TX,   .maxpacket = 512, },
	{ .hw_ep_num = 3, .style = FIFO_RX,   .maxpacket = 512, },
	{ .hw_ep_num = 4, .style = FIFO_RXTX, .maxpacket = 4096, },
};

static int pic32_probe_musb_device(struct pic32_musb *glue,
				   struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct musb_hdrc_platform_data pdata;
	struct musb_hdrc_config	*mconfig;
	struct platform_device *platdev;
	struct resource	resources[3];
	struct resource	*res;
	int fifo_mode = 0;
	int ret;

	memset(resources, 0, sizeof(resources));
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mc");
	if (!res) {
		dev_err(&pdev->dev, "failed to get memory.\n");
		return -EINVAL;
	}
	resources[0] = *res;
	resources[0].name = "mc";

	res = platform_get_resource_byname(pdev, IORESOURCE_IRQ, "mc");
	if (!res) {
		dev_err(&pdev->dev, "failed to get irq.\n");
		return -EINVAL;
	}
	resources[1] = *res;
	resources[1].name = "mc";

	res = platform_get_resource_byname(pdev, IORESOURCE_IRQ, "dma");
	if (!res) {
		dev_warn(&pdev->dev, "No MUSB DMA irq provided. Assuming PIO mode.\n");
	} else {
		resources[2] = *res;
		resources[2].name = "dma";
	}

	/* allocate child platform device for musb platform driver */
	platdev = platform_device_alloc("musb-hdrc", PLATFORM_DEVID_AUTO);
	if (!platdev)
		return -ENOMEM;

	glue->platdev = platdev;

	platdev->dev.parent = &pdev->dev;
	platdev->dev.dma_mask = &musb_dmamask;
	platdev->dev.coherent_dma_mask = 0;

	ret = platform_device_add_resources(platdev, resources,
					    ARRAY_SIZE(resources));
	if (ret) {
		dev_err(&pdev->dev, "failed to add resources\n");
		goto err_pdev;
	}

	mconfig = devm_kzalloc(&pdev->dev, sizeof(*mconfig), GFP_KERNEL);
	if (!mconfig) {
		ret = -ENOMEM;
		goto err_pdev;
	}

	mconfig->host_port_deassert_reset_at_resume = 1;
	mconfig->multipoint = of_property_read_bool(np, "mentor,multipoint");
	of_property_read_u32(np, "mentor,num-eps", (u32 *)&mconfig->num_eps);
	of_property_read_u32(np, "mentor,ram-bits", (u32 *)&mconfig->ram_bits);

	/* FiFo configuration */
	of_property_read_u32(np, "microchip,fifo-mode", (u32 *)&fifo_mode);
	dev_info(&pdev->dev, "using fifo mode %d\n", fifo_mode);
	switch (fifo_mode) {
	case 1:
		mconfig->fifo_cfg = pic32_musb_fifo_cfg1;
		mconfig->fifo_cfg_size = ARRAY_SIZE(pic32_musb_fifo_cfg1);
		break;
	default:
		mconfig->fifo_cfg = pic32_musb_fifo_cfg0;
		mconfig->fifo_cfg_size = ARRAY_SIZE(pic32_musb_fifo_cfg0);
		break;
	}

	/* add platform data */
	pdata.config = mconfig;
	pdata.platform_ops = &pic32_musb_ops;
	pdata.mode = get_musb_port_mode(&pdev->dev);

	/* DT keeps this entry in mA, but musb expects it as per USB spec */
	of_property_read_u32(np, "mentor,power", (u32 *)&pdata.power);
	pdata.power /= 2;

	ret = platform_device_add_data(platdev, &pdata, sizeof(pdata));
	if (ret) {
		dev_err(&pdev->dev, "failed to add platform_data\n");
		goto err_pdev;
	}

	ret = platform_device_add(platdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to register musb device\n");
		goto err_pdev;
	}

	return 0;

err_pdev:
	platform_device_put(platdev);
	return ret;
}

static int pic32_musb_probe(struct platform_device *pdev)
{
	struct device_node *dev_oc;
	struct pic32_musb *glue;
	struct resource	*cru;
	int ret;

	/* allocate glue */
	glue = devm_kzalloc(&pdev->dev, sizeof(*glue), GFP_KERNEL);
	if (!glue)
		return -ENOMEM;

	dev_oc = of_parse_phandle(pdev->dev.of_node, "usb_overcurrent", 0);
	if (!dev_oc) {
		dev_err(&pdev->dev, "error usb_overcurrent property missing\n");
		return -EINVAL;
	}

	glue->oc_irq = irq_of_parse_and_map(dev_oc, 0);
	if (!glue->oc_irq) {
		dev_err(&pdev->dev, "cannot get over current irq!\n");
		ret = -EINVAL;
		goto err_put_oc;
	}

	/* clock-reset module */
	cru = platform_get_resource_byname(pdev, IORESOURCE_MEM, "usbcr");
	glue->cru = devm_ioremap_resource(&pdev->dev, cru);
	if (IS_ERR(glue->cru)) {
		ret = PTR_ERR(glue->cru);
		goto err_put_oc;
	}

	glue->clk = devm_clk_get(&pdev->dev, "usb_clk");
	if (IS_ERR(glue->clk)) {
		dev_err(&pdev->dev, "failed to get usb_clk %d\n", ret);
		ret = PTR_ERR(glue->clk);
		goto err_put_oc;
	}

	ret = clk_prepare_enable(glue->clk);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable usb_clk %d\n", ret);
		goto err_put_oc;
	}

	platform_set_drvdata(pdev, glue);

	ret = pic32_probe_musb_device(glue, pdev);
	if (ret) {
		clk_disable_unprepare(glue->clk);
		goto err_put_oc;
	}

err_put_oc:
	of_node_put(dev_oc);
	return ret;
}

static int pic32_remove(struct platform_device *pdev)
{
	struct pic32_musb *glue = platform_get_drvdata(pdev);

	platform_device_unregister(glue->platdev);
	clk_disable_unprepare(glue->clk);

	return 0;
}

static const struct of_device_id pic32_musb_of_match[] = {
	{ .compatible = "microchip,pic32mzda-usb" },
	{ }
};
MODULE_DEVICE_TABLE(of, pic32_musb_of_match);

static struct platform_driver pic32_musb_driver = {
	.probe	= pic32_musb_probe,
	.remove	= pic32_remove,
	.driver	= {
		.name		= "musb-pic32mz",
		.of_match_table	= pic32_musb_of_match,
	},
};

MODULE_DESCRIPTION("Microchip PIC32 MUSB Glue Layer");
MODULE_AUTHOR("Cristian Birsan <cristian.birsan@microchip.com>");
MODULE_LICENSE("GPL v2");

module_platform_driver(pic32_musb_driver);
