// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 MediaTek Inc.
 *
 * Author:
 *  Min Guo <min.guo@mediatek.com>
 *  Yonglong Wu <yonglong.wu@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/usb/usb_phy_generic.h>
#include "musb_core.h"
#include "musb_dma.h"

#define USB_L1INTS	0x00a0
#define USB_L1INTM	0x00a4
#define MTK_MUSB_TXFUNCADDR	0x0480

#define TX_INT_STATUS		BIT(0)
#define RX_INT_STATUS		BIT(1)
#define USBCOM_INT_STATUS	BIT(2)
#define DMA_INT_STATUS		BIT(3)

#define DMA_INTR_STATUS_MSK		GENMASK(7, 0)
#define DMA_INTR_UNMASK_SET_MSK	GENMASK(31, 24)

enum mtk_vbus_id_state {
	MTK_ID_FLOAT = 1,
	MTK_ID_GROUND,
	MTK_VBUS_OFF,
	MTK_VBUS_VALID,
};

struct mtk_glue {
	struct device *dev;
	struct musb *musb;
	struct platform_device *musb_pdev;
	struct platform_device *usb_phy;
	struct phy *phy;
	struct usb_phy *xceiv;
	enum phy_mode phy_mode;
	struct clk *main;
	struct clk *mcu;
	struct clk *univpll;
	struct regulator *vbus;
	struct extcon_dev *edev;
	struct notifier_block vbus_nb;
	struct notifier_block id_nb;
};

static int mtk_musb_clks_get(struct mtk_glue *glue)
{
	struct device *dev = glue->dev;

	glue->main = devm_clk_get(dev, "main");
	if (IS_ERR(glue->main)) {
		dev_err(dev, "fail to get main clock\n");
		return PTR_ERR(glue->main);
	}

	glue->mcu = devm_clk_get(dev, "mcu");
	if (IS_ERR(glue->mcu)) {
		dev_err(dev, "fail to get mcu clock\n");
		return PTR_ERR(glue->mcu);
	}

	glue->univpll = devm_clk_get(dev, "univpll");
	if (IS_ERR(glue->univpll)) {
		dev_err(dev, "fail to get univpll clock\n");
		return PTR_ERR(glue->univpll);
	}

	return 0;
}

static int mtk_musb_clks_enable(struct mtk_glue *glue)
{
	int ret;

	ret = clk_prepare_enable(glue->main);
	if (ret) {
		dev_err(glue->dev, "failed to enable main clock\n");
		goto err_main_clk;
	}

	ret = clk_prepare_enable(glue->mcu);
	if (ret) {
		dev_err(glue->dev, "failed to enable mcu clock\n");
		goto err_mcu_clk;
	}

	ret = clk_prepare_enable(glue->univpll);
	if (ret) {
		dev_err(glue->dev, "failed to enable univpll clock\n");
		goto err_univpll_clk;
	}

	return 0;

err_univpll_clk:
	clk_disable_unprepare(glue->mcu);
err_mcu_clk:
	clk_disable_unprepare(glue->main);
err_main_clk:
	return ret;
}

static void mtk_musb_clks_disable(struct mtk_glue *glue)
{
	clk_disable_unprepare(glue->univpll);
	clk_disable_unprepare(glue->mcu);
	clk_disable_unprepare(glue->main);
}

static void mtk_musb_set_vbus(struct musb *musb, int is_on)
{
	struct device *dev = musb->controller;
	struct mtk_glue *glue = dev_get_drvdata(dev->parent);
	int ret;

	/* vbus is optional */
	if (!glue->vbus)
		return;

	dev_dbg(musb->controller, "%s, is_on=%d\r\n", __func__, is_on);
	if (is_on) {
		ret = regulator_enable(glue->vbus);
		if (ret) {
			dev_err(glue->dev, "fail to enable vbus regulator\n");
			return;
		}
	} else {
		regulator_disable(glue->vbus);
	}
}

/*
 * switch to host: -> MTK_VBUS_OFF --> MTK_ID_GROUND
 * switch to device: -> MTK_ID_FLOAT --> MTK_VBUS_VALID
 */
static void mtk_musb_set_mailbox(struct mtk_glue *glue,
	enum mtk_vbus_id_state status)
{
	struct musb *musb = glue->musb;
	u8 devctl = 0;

	dev_dbg(glue->dev, "mailbox state(%d)\n", status);
	switch (status) {
	case MTK_ID_GROUND:
		phy_power_on(glue->phy);
		devctl = readb(musb->mregs + MUSB_DEVCTL);
		musb->xceiv->otg->state = OTG_STATE_A_WAIT_VRISE;
		mtk_musb_set_vbus(musb, 1);
		glue->phy_mode = PHY_MODE_USB_HOST;
		phy_set_mode(glue->phy, glue->phy_mode);
		devctl |= MUSB_DEVCTL_SESSION;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
		MUSB_HST_MODE(musb);
		break;
	/*
	 * MTK_ID_FLOAT process is the same as MTK_VBUS_VALID
	 * except that turn off VBUS
	 */
	case MTK_ID_FLOAT:
		mtk_musb_set_vbus(musb, 0);
		/* fall through */
	case MTK_VBUS_OFF:
		musb->xceiv->otg->state = OTG_STATE_B_IDLE;
		devctl &= ~MUSB_DEVCTL_SESSION;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
		phy_power_off(glue->phy);
		break;
	case MTK_VBUS_VALID:
		phy_power_on(glue->phy);
		glue->phy_mode = PHY_MODE_USB_DEVICE;
		phy_set_mode(glue->phy, glue->phy_mode);
		MUSB_DEV_MODE(musb);
		break;
	default:
		dev_err(glue->dev, "invalid state\n");
	}
}

static int mtk_musb_id_notifier(struct notifier_block *nb,
	unsigned long event, void *ptr)
{
	struct mtk_glue *glue = container_of(nb, struct mtk_glue, id_nb);

	if (event)
		mtk_musb_set_mailbox(glue, MTK_ID_GROUND);
	else
		mtk_musb_set_mailbox(glue, MTK_ID_FLOAT);

	return NOTIFY_DONE;
}

static int mtk_musb_vbus_notifier(struct notifier_block *nb,
	unsigned long event, void *ptr)
{
	struct mtk_glue *glue = container_of(nb, struct mtk_glue, vbus_nb);

	if (event)
		mtk_musb_set_mailbox(glue, MTK_VBUS_VALID);
	else
		mtk_musb_set_mailbox(glue, MTK_VBUS_OFF);

	return NOTIFY_DONE;
}

static void mtk_otg_switch_init(struct mtk_glue *glue)
{
	int ret;

	/* extcon is optional */
	if (!glue->edev)
		return;

	glue->vbus_nb.notifier_call = mtk_musb_vbus_notifier;
	ret = devm_extcon_register_notifier(glue->dev, glue->edev, EXTCON_USB,
					&glue->vbus_nb);
	if (ret < 0)
		dev_err(glue->dev, "failed to register notifier for USB\n");

	glue->id_nb.notifier_call = mtk_musb_id_notifier;
	ret = devm_extcon_register_notifier(glue->dev, glue->edev,
					EXTCON_USB_HOST, &glue->id_nb);
	if (ret < 0)
		dev_err(glue->dev, "failed to register notifier for USB-HOST\n");

	dev_dbg(glue->dev, "EXTCON_USB: %d, EXTCON_USB_HOST: %d\n",
		extcon_get_state(glue->edev, EXTCON_USB),
		extcon_get_state(glue->edev, EXTCON_USB_HOST));

	/* default as host, switch to device mode if needed */
	if (extcon_get_state(glue->edev, EXTCON_USB_HOST) == false)
		mtk_musb_set_mailbox(glue, MTK_ID_FLOAT);
	if (extcon_get_state(glue->edev, EXTCON_USB) == true)
		mtk_musb_set_mailbox(glue, MTK_VBUS_VALID);
}

static irqreturn_t generic_interrupt(int irq, void *__hci)
{
	unsigned long flags;
	irqreturn_t retval = IRQ_NONE;
	struct musb *musb = __hci;

	spin_lock_irqsave(&musb->lock, flags);
	musb->int_usb = musb_readb(musb->mregs, MUSB_INTRUSB) &
	    musb_readb(musb->mregs, MUSB_INTRUSBE);
	musb->int_tx = musb_readw(musb->mregs, MUSB_INTRTX)
	    & musb_readw(musb->mregs, MUSB_INTRTXE);
	musb->int_rx = musb_readw(musb->mregs, MUSB_INTRRX)
	    & musb_readw(musb->mregs, MUSB_INTRRXE);
	/* MediaTek controller interrupt status is W1C */
	musb_writew(musb->mregs, MUSB_INTRRX, musb->int_rx);
	musb_writew(musb->mregs, MUSB_INTRTX, musb->int_tx);
	musb_writeb(musb->mregs, MUSB_INTRUSB, musb->int_usb);

	if (musb->int_usb || musb->int_tx || musb->int_rx)
		retval = musb_interrupt(musb);

	spin_unlock_irqrestore(&musb->lock, flags);

	return retval;
}

static irqreturn_t mtk_musb_interrupt(int irq, void *dev_id)
{
	irqreturn_t retval = IRQ_NONE;
	struct musb *musb = (struct musb *)dev_id;
	u32 l1_ints;

	l1_ints = musb_readl(musb->mregs, USB_L1INTS) &
			musb_readl(musb->mregs, USB_L1INTM);

	if (l1_ints & (TX_INT_STATUS | RX_INT_STATUS | USBCOM_INT_STATUS))
		retval = generic_interrupt(irq, musb);

#if defined(CONFIG_USB_INVENTRA_DMA)
	if (l1_ints & DMA_INT_STATUS)
		retval = dma_controller_irq(irq, musb->dma_controller);
#endif
	return retval;
}

static u32 mtk_musb_busctl_offset(u8 epnum, u16 offset)
{
	return MTK_MUSB_TXFUNCADDR + offset + 8 * epnum;
}

static int mtk_musb_init(struct musb *musb)
{
	struct device *dev = musb->controller;
	struct mtk_glue *glue = dev_get_drvdata(dev->parent);
	int ret;

	glue->musb = musb;
	musb->phy = glue->phy;
	musb->xceiv = glue->xceiv;
	musb->is_host = false;
	musb->isr = mtk_musb_interrupt;
	ret = phy_init(glue->phy);
	if (ret)
		return ret;

	ret = phy_power_on(glue->phy);
	if (ret) {
		phy_exit(glue->phy);
		return ret;
	}

	phy_set_mode(glue->phy, glue->phy_mode);

#if defined(CONFIG_USB_INVENTRA_DMA)
	musb_writel(musb->mregs, MUSB_HSDMA_INTR,
		    DMA_INTR_STATUS_MSK | DMA_INTR_UNMASK_SET_MSK);
#endif
	musb_writel(musb->mregs, USB_L1INTM, TX_INT_STATUS | RX_INT_STATUS |
		    USBCOM_INT_STATUS | DMA_INT_STATUS);
	return 0;
}

static int mtk_musb_set_mode(struct musb *musb, u8 mode)
{
	struct device *dev = musb->controller;
	struct mtk_glue *glue = dev_get_drvdata(dev->parent);
	enum phy_mode new_mode;

	switch (mode) {
	case MUSB_HOST:
		new_mode = PHY_MODE_USB_HOST;
		mtk_musb_set_vbus(musb, 1);
		break;
	case MUSB_PERIPHERAL:
		new_mode = PHY_MODE_USB_DEVICE;
		break;
	case MUSB_OTG:
		new_mode = PHY_MODE_USB_HOST;
		break;
	default:
		dev_err(musb->controller->parent,
			"Error requested mode not supported by this kernel\n");
		return -EINVAL;
	}
	if (glue->phy_mode == new_mode)
		return 0;

	mtk_musb_set_mailbox(glue, MTK_ID_GROUND);
	return 0;
}

static int mtk_musb_exit(struct musb *musb)
{
	struct device *dev = musb->controller;
	struct mtk_glue *glue = dev_get_drvdata(dev->parent);

	phy_power_off(glue->phy);
	phy_exit(glue->phy);
	mtk_musb_clks_disable(glue);

	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
	return 0;
}

static const struct musb_platform_ops mtk_musb_ops = {
	.quirks = MUSB_DMA_INVENTRA | MUSB_MTK_QUIRKS,
	.init = mtk_musb_init,
	.exit = mtk_musb_exit,
#ifdef CONFIG_USB_INVENTRA_DMA
	.dma_init = musbhs_dma_controller_create,
	.dma_exit = musbhs_dma_controller_destroy,
#endif
	.busctl_offset = mtk_musb_busctl_offset,
	.set_mode = mtk_musb_set_mode,
	.set_vbus = mtk_musb_set_vbus,
};

#define MTK_MUSB_MAX_EP_NUM	8
#define MTK_MUSB_RAM_BITS	11

static struct musb_fifo_cfg mtk_musb_mode_cfg[] = {
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
	{ .hw_ep_num = 6, .style = FIFO_TX, .maxpacket = 1024, },
	{ .hw_ep_num = 6, .style = FIFO_RX, .maxpacket = 1024, },
	{ .hw_ep_num = 7, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 7, .style = FIFO_RX, .maxpacket = 64, },
};

static const struct musb_hdrc_config mtk_musb_hdrc_config = {
	.fifo_cfg = mtk_musb_mode_cfg,
	.fifo_cfg_size = ARRAY_SIZE(mtk_musb_mode_cfg),
	.multipoint = true,
	.dyn_fifo = true,
	.num_eps = MTK_MUSB_MAX_EP_NUM,
	.ram_bits = MTK_MUSB_RAM_BITS,
};

static const struct platform_device_info mtk_dev_info = {
	.name = "musb-hdrc",
	.id = PLATFORM_DEVID_AUTO,
	.dma_mask = DMA_BIT_MASK(32),
};

static int mtk_musb_probe(struct platform_device *pdev)
{
	struct musb_hdrc_platform_data *pdata;
	struct mtk_glue *glue;
	struct platform_device_info pinfo;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int ret = -ENOMEM;

	glue = devm_kzalloc(dev, sizeof(*glue), GFP_KERNEL);
	if (!glue)
		return -ENOMEM;

	glue->dev = dev;
	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	ret = mtk_musb_clks_get(glue);
	if (ret)
		return ret;

	glue->vbus = devm_regulator_get(dev, "vbus");
	if (IS_ERR(glue->vbus)) {
		dev_err(dev, "fail to get vbus\n");
		return PTR_ERR(glue->vbus);
	}

	pdata->config = &mtk_musb_hdrc_config;
	pdata->platform_ops = &mtk_musb_ops;
	if (of_property_read_bool(np, "extcon")) {
		glue->edev = extcon_get_edev_by_phandle(dev, 0);
		if (IS_ERR(glue->edev)) {
			dev_err(dev, "fail to get extcon\n");
			return PTR_ERR(glue->edev);
		}
	}

	pdata->mode = usb_get_dr_mode(dev);
	switch (pdata->mode) {
	case USB_DR_MODE_HOST:
		glue->phy_mode = PHY_MODE_USB_HOST;
		break;
	case USB_DR_MODE_PERIPHERAL:
		glue->phy_mode = PHY_MODE_USB_DEVICE;
		break;
	default:
		pdata->mode = USB_DR_MODE_OTG;
		/* FALL THROUGH */
	case USB_DR_MODE_OTG:
		glue->phy_mode = PHY_MODE_USB_OTG;
		break;
	}

	glue->phy = devm_phy_get(dev, "usb2-phy");
	if (IS_ERR(glue->phy)) {
		dev_err(dev, "fail to getting phy %ld\n",
			PTR_ERR(glue->phy));
		return PTR_ERR(glue->phy);
	}

	glue->usb_phy = usb_phy_generic_register();
	if (IS_ERR(glue->usb_phy)) {
		dev_err(dev, "fail to registering usb-phy %ld\n",
			PTR_ERR(glue->usb_phy));
		return PTR_ERR(glue->usb_phy);
	}

	glue->xceiv = devm_usb_get_phy(dev, USB_PHY_TYPE_USB2);
	if (IS_ERR(glue->xceiv)) {
		dev_err(dev, "fail to getting usb-phy %d\n", ret);
		ret = PTR_ERR(glue->xceiv);
		goto err_unregister_usb_phy;
	}

	platform_set_drvdata(pdev, glue);
	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	ret = mtk_musb_clks_enable(glue);
	if (ret)
		goto err_enable_clk;

	pinfo = mtk_dev_info;
	pinfo.parent = dev;
	pinfo.res = pdev->resource;
	pinfo.num_res = pdev->num_resources;
	pinfo.data = pdata;
	pinfo.size_data = sizeof(*pdata);

	glue->musb_pdev = platform_device_register_full(&pinfo);
	if (IS_ERR(glue->musb_pdev)) {
		ret = PTR_ERR(glue->musb_pdev);
		dev_err(dev, "failed to register musb device: %d\n", ret);
		goto err_device_register;
	}

	if (pdata->mode == USB_DR_MODE_OTG)
		mtk_otg_switch_init(glue);

	dev_info(dev, "USB probe done!\n");
	return 0;

err_device_register:
	mtk_musb_clks_disable(glue);
err_enable_clk:
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
err_unregister_usb_phy:
	usb_phy_generic_unregister(glue->usb_phy);
	return ret;
}

static int mtk_musb_remove(struct platform_device *pdev)
{
	struct mtk_glue *glue = platform_get_drvdata(pdev);
	struct platform_device *usb_phy = glue->usb_phy;

	platform_device_unregister(glue->musb_pdev);
	usb_phy_generic_unregister(usb_phy);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id mtk_musb_match[] = {
	{.compatible = "mediatek,mtk-musb",},
	{},
};
MODULE_DEVICE_TABLE(of, mtk_musb_match);
#endif

static struct platform_driver mtk_musb_driver = {
	.probe = mtk_musb_probe,
	.remove = mtk_musb_remove,
	.driver = {
		   .name = "musb-mtk",
		   .of_match_table = of_match_ptr(mtk_musb_match),
	},
};

module_platform_driver(mtk_musb_driver);

MODULE_DESCRIPTION("MediaTek MUSB Glue Layer");
MODULE_AUTHOR("Min Guo <min.guo@mediatek.com>");
MODULE_LICENSE("GPL v2");
