/*
 * mtu3_dr.c - dual role switch and host glue layer
 *
 * Copyright (C) 2016 MediaTek Inc.
 *
 * Author: Chunfeng Yun <chunfeng.yun@mediatek.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/of_device.h>

#include "mtu3.h"
#include "mtu3_dr.h"

#define USB2_PORT 2
#define USB3_PORT 3

enum mtu3_vbus_id_state {
	MTU3_ID_FLOAT = 1,
	MTU3_ID_GROUND,
	MTU3_VBUS_OFF,
	MTU3_VBUS_VALID,
};

static void toggle_opstate(struct ssusb_mtk *ssusb)
{
	if (!ssusb->otg_switch.is_u3_drd) {
		mtu3_setbits(ssusb->mac_base, U3D_DEVICE_CONTROL, DC_SESSION);
		mtu3_setbits(ssusb->mac_base, U3D_POWER_MANAGEMENT, SOFT_CONN);
	}
}

/* only port0 supports dual-role mode */
static int ssusb_port0_switch(struct ssusb_mtk *ssusb,
		int version, bool tohost)
{
	void __iomem *ibase = ssusb->ippc_base;
	u32 temp;

	dev_dbg(ssusb->dev, "%s (switch u%d port0 to %s)\n", __func__,
		version, tohost ? "host" : "device");

	if (version == USB2_PORT) {
		/* 1. power off and disable u2 port0 */
		temp = mtu3_readl(ibase, SSUSB_U2_CTRL(0));
		temp |= SSUSB_U2_PORT_PDN | SSUSB_U2_PORT_DIS;
		mtu3_writel(ibase, SSUSB_U2_CTRL(0), temp);

		/* 2. power on, enable u2 port0 and select its mode */
		temp = mtu3_readl(ibase, SSUSB_U2_CTRL(0));
		temp &= ~(SSUSB_U2_PORT_PDN | SSUSB_U2_PORT_DIS);
		temp = tohost ? (temp | SSUSB_U2_PORT_HOST_SEL) :
			(temp & (~SSUSB_U2_PORT_HOST_SEL));
		mtu3_writel(ibase, SSUSB_U2_CTRL(0), temp);
	} else {
		/* 1. power off and disable u3 port0 */
		temp = mtu3_readl(ibase, SSUSB_U3_CTRL(0));
		temp |= SSUSB_U3_PORT_PDN | SSUSB_U3_PORT_DIS;
		mtu3_writel(ibase, SSUSB_U3_CTRL(0), temp);

		/* 2. power on, enable u3 port0 and select its mode */
		temp = mtu3_readl(ibase, SSUSB_U3_CTRL(0));
		temp &= ~(SSUSB_U3_PORT_PDN | SSUSB_U3_PORT_DIS);
		temp = tohost ? (temp | SSUSB_U3_PORT_HOST_SEL) :
			(temp & (~SSUSB_U3_PORT_HOST_SEL));
		mtu3_writel(ibase, SSUSB_U3_CTRL(0), temp);
	}

	return 0;
}

static void switch_port_to_host(struct ssusb_mtk *ssusb)
{
	u32 check_clk = 0;

	dev_dbg(ssusb->dev, "%s\n", __func__);

	ssusb_port0_switch(ssusb, USB2_PORT, true);

	if (ssusb->otg_switch.is_u3_drd) {
		ssusb_port0_switch(ssusb, USB3_PORT, true);
		check_clk = SSUSB_U3_MAC_RST_B_STS;
	}

	ssusb_check_clocks(ssusb, check_clk);

	/* after all clocks are stable */
	toggle_opstate(ssusb);
}

static void switch_port_to_device(struct ssusb_mtk *ssusb)
{
	u32 check_clk = 0;

	dev_dbg(ssusb->dev, "%s\n", __func__);

	ssusb_port0_switch(ssusb, USB2_PORT, false);

	if (ssusb->otg_switch.is_u3_drd) {
		ssusb_port0_switch(ssusb, USB3_PORT, false);
		check_clk = SSUSB_U3_MAC_RST_B_STS;
	}

	ssusb_check_clocks(ssusb, check_clk);
}

int ssusb_set_vbus(struct otg_switch_mtk *otg_sx, int is_on)
{
	struct ssusb_mtk *ssusb =
		container_of(otg_sx, struct ssusb_mtk, otg_switch);
	struct regulator *vbus = otg_sx->vbus;
	int ret;

	/* vbus is optional */
	if (!vbus)
		return 0;

	dev_dbg(ssusb->dev, "%s: turn %s\n", __func__, is_on ? "on" : "off");

	if (is_on) {
		ret = regulator_enable(vbus);
		if (ret) {
			dev_err(ssusb->dev, "vbus regulator enable failed\n");
			return ret;
		}
	} else {
		regulator_disable(vbus);
	}

	return 0;
}

/*
 * switch to host: -> MTU3_VBUS_OFF --> MTU3_ID_GROUND
 * switch to device: -> MTU3_ID_FLOAT --> MTU3_VBUS_VALID
 */
static void ssusb_set_mailbox(struct otg_switch_mtk *otg_sx,
	enum mtu3_vbus_id_state status)
{
	struct ssusb_mtk *ssusb =
		container_of(otg_sx, struct ssusb_mtk, otg_switch);
	struct mtu3 *mtu = ssusb->u3d;

	dev_dbg(ssusb->dev, "mailbox state(%d)\n", status);

	switch (status) {
	case MTU3_ID_GROUND:
		switch_port_to_host(ssusb);
		ssusb_set_vbus(otg_sx, 1);
		ssusb->is_host = true;
		break;
	case MTU3_ID_FLOAT:
		ssusb->is_host = false;
		ssusb_set_vbus(otg_sx, 0);
		switch_port_to_device(ssusb);
		break;
	case MTU3_VBUS_OFF:
		mtu3_stop(mtu);
		pm_relax(ssusb->dev);
		break;
	case MTU3_VBUS_VALID:
		/* avoid suspend when works as device */
		pm_stay_awake(ssusb->dev);
		mtu3_start(mtu);
		break;
	default:
		dev_err(ssusb->dev, "invalid state\n");
	}
}

static int ssusb_id_notifier(struct notifier_block *nb,
	unsigned long event, void *ptr)
{
	struct otg_switch_mtk *otg_sx =
		container_of(nb, struct otg_switch_mtk, id_nb);

	if (event)
		ssusb_set_mailbox(otg_sx, MTU3_ID_GROUND);
	else
		ssusb_set_mailbox(otg_sx, MTU3_ID_FLOAT);

	return NOTIFY_DONE;
}

static int ssusb_vbus_notifier(struct notifier_block *nb,
	unsigned long event, void *ptr)
{
	struct otg_switch_mtk *otg_sx =
		container_of(nb, struct otg_switch_mtk, vbus_nb);

	if (event)
		ssusb_set_mailbox(otg_sx, MTU3_VBUS_VALID);
	else
		ssusb_set_mailbox(otg_sx, MTU3_VBUS_OFF);

	return NOTIFY_DONE;
}

static int ssusb_extcon_register(struct otg_switch_mtk *otg_sx)
{
	struct ssusb_mtk *ssusb =
		container_of(otg_sx, struct ssusb_mtk, otg_switch);
	struct extcon_dev *edev = otg_sx->edev;
	int ret;

	/* extcon is optional */
	if (!edev)
		return 0;

	otg_sx->vbus_nb.notifier_call = ssusb_vbus_notifier;
	ret = extcon_register_notifier(edev, EXTCON_USB,
					&otg_sx->vbus_nb);
	if (ret < 0)
		dev_err(ssusb->dev, "failed to register notifier for USB\n");

	otg_sx->id_nb.notifier_call = ssusb_id_notifier;
	ret = extcon_register_notifier(edev, EXTCON_USB_HOST,
					&otg_sx->id_nb);
	if (ret < 0)
		dev_err(ssusb->dev, "failed to register notifier for USB-HOST\n");

	dev_dbg(ssusb->dev, "EXTCON_USB: %d, EXTCON_USB_HOST: %d\n",
		extcon_get_cable_state_(edev, EXTCON_USB),
		extcon_get_cable_state_(edev, EXTCON_USB_HOST));

	/* default as host, switch to device mode if needed */
	if (extcon_get_cable_state_(edev, EXTCON_USB_HOST) == false)
		ssusb_set_mailbox(otg_sx, MTU3_ID_FLOAT);
	if (extcon_get_cable_state_(edev, EXTCON_USB) == true)
		ssusb_set_mailbox(otg_sx, MTU3_VBUS_VALID);

	return 0;
}

static void extcon_register_dwork(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct otg_switch_mtk *otg_sx =
	    container_of(dwork, struct otg_switch_mtk, extcon_reg_dwork);

	ssusb_extcon_register(otg_sx);
}

/*
 * When use TYPE-A receptacle but also want to support dual-role mode,
 * we provide an interface via sysfs to switch between host and device modes
 * depending on user input.
 * It generates cable state changes by pulling up/down IDPIN and
 * notifies driver to switch mode by "extcon-usb-gpio".
 * NOTE: when use MICRO receptacle, should not enable this interface.
 */
static void ssusb_mode_sysfs_switch(struct ssusb_mtk *ssusb, int to_host)
{
	struct otg_switch_mtk *otg_sx = &ssusb->otg_switch;

	if (!otg_sx->manual_drd_enabled)
		return;

	if (to_host)
		pinctrl_select_state(otg_sx->id_pinctrl, otg_sx->id_ground);
	else
		pinctrl_select_state(otg_sx->id_pinctrl, otg_sx->id_float);
}

static ssize_t ssusb_mode_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct ssusb_mtk *ssusb = platform_get_drvdata(pdev);
	int ret;

	ret = sprintf(buf, "current mode: %s(%s drd)\n(echo device/host)\n",
		ssusb->is_host ? "host" : "device",
		ssusb->otg_switch.manual_drd_enabled ? "manual" : "auto");

	return ret;
}

static ssize_t ssusb_mode_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct ssusb_mtk *ssusb = platform_get_drvdata(pdev);

	if (sysfs_streq(buf, "host") && !ssusb->is_host)
		ssusb_mode_sysfs_switch(ssusb, 1);
	else if (sysfs_streq(buf, "device") && ssusb->is_host)
		ssusb_mode_sysfs_switch(ssusb, 0);
	else
		dev_err(dev, "wrong or duplicated setting\n");

	return count;
}

static DEVICE_ATTR(mode, 0664, ssusb_mode_show, ssusb_mode_store);

static struct attribute *ssusb_attributes[] = {
	&dev_attr_mode.attr,
	NULL
};

static const struct attribute_group ssusb_attr_group = {
	.attrs = ssusb_attributes,
};

int ssusb_otg_switch_init(struct ssusb_mtk *ssusb)
{
	struct otg_switch_mtk *otg_sx = &ssusb->otg_switch;
	int ret;

	INIT_DELAYED_WORK(&otg_sx->extcon_reg_dwork, extcon_register_dwork);

	ret = sysfs_create_group(&ssusb->dev->kobj, &ssusb_attr_group);
	if (ret) {
		dev_err(ssusb->dev, "Cannot register sysfs attributes: %d\n",
			ret);
		return ret;
	}

	/* It is enough to delay 1s for waiting for host initialization */
	schedule_delayed_work(&otg_sx->extcon_reg_dwork, HZ);

	return 0;
}

void ssusb_otg_switch_exit(struct ssusb_mtk *ssusb)
{
	struct otg_switch_mtk *otg_sx = &ssusb->otg_switch;

	if (otg_sx->edev) {
		extcon_unregister_notifier(otg_sx->edev,
			EXTCON_USB, &otg_sx->vbus_nb);
		extcon_unregister_notifier(otg_sx->edev,
			EXTCON_USB_HOST, &otg_sx->id_nb);
	}
	sysfs_remove_group(&ssusb->dev->kobj, &ssusb_attr_group);
}
