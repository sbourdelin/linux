/*
 * charger.c -- USB charger driver
 *
 * Copyright (C) 2015 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/extcon.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/usb_charger.h>
#include <linux/power_supply.h>

#define DEFAULT_CUR_PROTECT	(50)
#define DEFAULT_SDP_CUR_LIMIT	(500 - DEFAULT_CUR_PROTECT)
#define DEFAULT_DCP_CUR_LIMIT	(1500 - DEFAULT_CUR_PROTECT)
#define DEFAULT_CDP_CUR_LIMIT	(1500 - DEFAULT_CUR_PROTECT)
#define DEFAULT_ACA_CUR_LIMIT	(1500 - DEFAULT_CUR_PROTECT)
#define UCHGER_STATE_LENGTH	(50)

static DEFINE_IDA(usb_charger_ida);
static struct bus_type usb_charger_subsys = {
	.name           = "usb-charger",
	.dev_name       = "usb-charger",
};

static struct usb_charger *dev_to_uchger(struct device *udev)
{
	return container_of(udev, struct usb_charger, dev);
}

/*
 * Sysfs attributes:
 *
 * These sysfs attributes are used for showing and setting different type
 * (SDP/DCP/CDP/ACA) chargers' current limitation.
 */
static ssize_t sdp_limit_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct usb_charger *uchger = dev_to_uchger(dev);

	return sprintf(buf, "%d\n", uchger->cur_limit.sdp_cur_limit);
}

static ssize_t sdp_limit_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct usb_charger *uchger = dev_to_uchger(dev);
	unsigned int sdp_limit;
	int ret;

	ret = kstrtouint(buf, 10, &sdp_limit);
	if (ret < 0)
		return ret;

	ret = usb_charger_set_cur_limit_by_type(uchger, SDP_TYPE, sdp_limit);
	if (ret < 0)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(sdp_limit);

static ssize_t dcp_limit_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct usb_charger *uchger = dev_to_uchger(dev);

	return sprintf(buf, "%d\n", uchger->cur_limit.dcp_cur_limit);
}

static ssize_t dcp_limit_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct usb_charger *uchger = dev_to_uchger(dev);
	unsigned int dcp_limit;
	int ret;

	ret = kstrtouint(buf, 10, &dcp_limit);
	if (ret < 0)
		return ret;

	ret = usb_charger_set_cur_limit_by_type(uchger, DCP_TYPE, dcp_limit);
	if (ret < 0)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(dcp_limit);

static ssize_t cdp_limit_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct usb_charger *uchger = dev_to_uchger(dev);

	return sprintf(buf, "%d\n", uchger->cur_limit.cdp_cur_limit);
}

static ssize_t cdp_limit_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct usb_charger *uchger = dev_to_uchger(dev);
	unsigned int cdp_limit;
	int ret;

	ret = kstrtouint(buf, 10, &cdp_limit);
	if (ret < 0)
		return ret;

	ret = usb_charger_set_cur_limit_by_type(uchger, CDP_TYPE, cdp_limit);
	if (ret < 0)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(cdp_limit);

static ssize_t aca_limit_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct usb_charger *uchger = dev_to_uchger(dev);

	return sprintf(buf, "%d\n", uchger->cur_limit.aca_cur_limit);
}

static ssize_t aca_limit_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct usb_charger *uchger = dev_to_uchger(dev);
	unsigned int aca_limit;
	int ret;

	ret = kstrtouint(buf, 10, &aca_limit);
	if (ret < 0)
		return ret;

	ret = usb_charger_set_cur_limit_by_type(uchger, ACA_TYPE, aca_limit);
	if (ret < 0)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(aca_limit);

static struct attribute *usb_charger_attrs[] = {
	&dev_attr_sdp_limit.attr,
	&dev_attr_dcp_limit.attr,
	&dev_attr_cdp_limit.attr,
	&dev_attr_aca_limit.attr,
	NULL
};
ATTRIBUTE_GROUPS(usb_charger);

/*
 * usb_charger_find_by_name - Get the usb charger device by name.
 * @name - usb charger device name.
 *
 * return the instance of usb charger device, the device must be
 * released with usb_charger_put().
 */
struct usb_charger *usb_charger_find_by_name(const char *name)
{
	struct device *udev;

	if (!name)
		return ERR_PTR(-EINVAL);

	udev = bus_find_device_by_name(&usb_charger_subsys, NULL, name);
	if (!udev)
		return ERR_PTR(-ENODEV);

	return dev_to_uchger(udev);
}
EXPORT_SYMBOL_GPL(usb_charger_find_by_name);

/*
 * usb_charger_get() - Reference a usb charger.
 * @uchger - usb charger
 */
struct usb_charger *usb_charger_get(struct usb_charger *uchger)
{
	return (uchger && get_device(&uchger->dev)) ? uchger : NULL;
}
EXPORT_SYMBOL_GPL(usb_charger_get);

/*
 * usb_charger_put() - Dereference a usb charger.
 * @uchger - charger to release
 */
void usb_charger_put(struct usb_charger *uchger)
{
	if (uchger)
		put_device(&uchger->dev);
}
EXPORT_SYMBOL_GPL(usb_charger_put);

/*
 * usb_charger_register_notify() - Register a notifiee to get notified by
 * any attach status changes from the usb charger detection.
 * @uchger - the usb charger device which is monitored.
 * @nb - a notifier block to be registered.
 */
int usb_charger_register_notify(struct usb_charger *uchger,
				struct notifier_block *nb)
{
	int ret;

	if (!uchger || !nb)
		return -EINVAL;

	mutex_lock(&uchger->lock);
	ret = raw_notifier_chain_register(&uchger->uchger_nh, nb);

	/* Generate an initial notify so users start in the right state */
	if (!ret) {
		usb_charger_detect_type(uchger);
		raw_notifier_call_chain(&uchger->uchger_nh,
					usb_charger_get_cur_limit(uchger),
					uchger);
	}
	mutex_unlock(&uchger->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(usb_charger_register_notify);

/*
 * usb_charger_unregister_notify() - Unregister a notifiee from the usb charger.
 * @uchger - the usb charger device which is monitored.
 * @nb - a notifier block to be unregistered.
 */
int usb_charger_unregister_notify(struct usb_charger *uchger,
				  struct notifier_block *nb)
{
	int ret;

	if (!uchger || !nb)
		return -EINVAL;

	mutex_lock(&uchger->lock);
	ret = raw_notifier_chain_unregister(&uchger->uchger_nh, nb);
	mutex_unlock(&uchger->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(usb_charger_unregister_notify);

/*
 * usb_charger_detect_type() - Get the usb charger type by the callback
 * which is implemented by gadget operations.
 * @uchger - the usb charger device.
 *
 * return the usb charger type.
 */
enum usb_charger_type
usb_charger_detect_type(struct usb_charger *uchger)
{
	if (uchger->gadget && uchger->gadget->ops
	    && uchger->gadget->ops->get_charger_type) {
		uchger->type =
			uchger->gadget->ops->get_charger_type(uchger->gadget);
	} else if (uchger->psy) {
		union power_supply_propval val;

		power_supply_get_property(uchger->psy,
					  POWER_SUPPLY_PROP_CHARGE_TYPE,
					  &val);
		switch (val.intval) {
		case POWER_SUPPLY_TYPE_USB:
			uchger->type = SDP_TYPE;
			break;
		case POWER_SUPPLY_TYPE_USB_DCP:
			uchger->type = DCP_TYPE;
			break;
		case POWER_SUPPLY_TYPE_USB_CDP:
			uchger->type = CDP_TYPE;
			break;
		case POWER_SUPPLY_TYPE_USB_ACA:
			uchger->type = ACA_TYPE;
			break;
		default:
			uchger->type = UNKNOWN_TYPE;
			break;
		}
	} else if (uchger->get_charger_type) {
		uchger->type = uchger->get_charger_type(uchger);
	} else {
		uchger->type = UNKNOWN_TYPE;
	}

	return uchger->type;
}
EXPORT_SYMBOL_GPL(usb_charger_detect_type);

/*
 * usb_charger_set_cur_limit_by_type() - Set the current limitation
 * by charger type.
 * @uchger - the usb charger device.
 * @type - the usb charger type.
 * @cur_limit - the current limitation.
 */
int usb_charger_set_cur_limit_by_type(struct usb_charger *uchger,
				      enum usb_charger_type type,
				      unsigned int cur_limit)
{
	if (!uchger)
		return -EINVAL;

	switch (type) {
	case SDP_TYPE:
		uchger->cur_limit.sdp_cur_limit = cur_limit;
		break;
	case DCP_TYPE:
		uchger->cur_limit.dcp_cur_limit = cur_limit;
		break;
	case CDP_TYPE:
		uchger->cur_limit.cdp_cur_limit	= cur_limit;
		break;
	case ACA_TYPE:
		uchger->cur_limit.aca_cur_limit	= cur_limit;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(usb_charger_set_cur_limit_by_type);

/*
 * usb_charger_set_cur_limit() - Set the current limitation.
 * @uchger - the usb charger device.
 * @cur_limit_set - the current limitation.
 */
int usb_charger_set_cur_limit(struct usb_charger *uchger,
			      struct usb_charger_cur_limit *cur_limit_set)
{
	if (!uchger || !cur_limit_set)
		return -EINVAL;

	uchger->cur_limit.sdp_cur_limit = cur_limit_set->sdp_cur_limit;
	uchger->cur_limit.dcp_cur_limit = cur_limit_set->dcp_cur_limit;
	uchger->cur_limit.cdp_cur_limit = cur_limit_set->cdp_cur_limit;
	uchger->cur_limit.aca_cur_limit = cur_limit_set->aca_cur_limit;
	return 0;
}
EXPORT_SYMBOL_GPL(usb_charger_set_cur_limit);

/*
 * usb_charger_get_cur_limit() - Get the current limitation by
 * different usb charger type.
 * @uchger - the usb charger device.
 *
 * return the current limitation to set.
 */
unsigned int
usb_charger_get_cur_limit(struct usb_charger *uchger)
{
	enum usb_charger_type uchger_type = usb_charger_detect_type(uchger);
	unsigned int cur_limit;

	switch (uchger_type) {
	case SDP_TYPE:
		cur_limit = uchger->cur_limit.sdp_cur_limit;
		break;
	case DCP_TYPE:
		cur_limit = uchger->cur_limit.dcp_cur_limit;
		break;
	case CDP_TYPE:
		cur_limit = uchger->cur_limit.cdp_cur_limit;
		break;
	case ACA_TYPE:
		cur_limit = uchger->cur_limit.aca_cur_limit;
		break;
	default:
		return 0;
	}

	return cur_limit;
}
EXPORT_SYMBOL_GPL(usb_charger_get_cur_limit);

/*
 * usb_charger_notifier_others() - It will notify other device registered
 * on usb charger when the usb charger state is changed.
 * @uchger - the usb charger device.
 * @state - the state of the usb charger.
 */
static void
usb_charger_notify_others(struct usb_charger *uchger,
			  enum usb_charger_state state)
{
	char uchger_state[UCHGER_STATE_LENGTH];
	char *envp[] = { uchger_state, NULL };

	mutex_lock(&uchger->lock);
	uchger->state = state;

	switch (state) {
	case USB_CHARGER_PRESENT:
		usb_charger_detect_type(uchger);
		raw_notifier_call_chain(&uchger->uchger_nh,
			usb_charger_get_cur_limit(uchger),
			uchger);
		snprintf(uchger_state, UCHGER_STATE_LENGTH,
			 "USB_CHARGER_STATE=%s", "USB_CHARGER_PRESENT");
		break;
	case USB_CHARGER_REMOVE:
		uchger->type = UNKNOWN_TYPE;
		raw_notifier_call_chain(&uchger->uchger_nh, 0, uchger);
		snprintf(uchger_state, UCHGER_STATE_LENGTH,
			 "USB_CHARGER_STATE=%s", "USB_CHARGER_REMOVE");
		break;
	default:
		dev_warn(&uchger->dev, "Unknown USB charger state: %d\n",
			 state);
		mutex_unlock(&uchger->lock);
		return;
	}

	kobject_uevent_env(&uchger->dev.kobj, KOBJ_CHANGE, envp);
	mutex_unlock(&uchger->lock);
}

/*
 * usb_charger_plug_by_extcon() - The notifier call function which is registered
 * on the extcon device.
 * @nb - the notifier block that notified by extcon device.
 * @state - the extcon device state.
 * @data - here specify a extcon device.
 *
 * return the notify flag.
 */
static int
usb_charger_plug_by_extcon(struct notifier_block *nb,
			   unsigned long state, void *data)
{
	struct usb_charger_nb *extcon_nb =
		container_of(nb, struct usb_charger_nb, nb);
	struct usb_charger *uchger = extcon_nb->uchger;
	enum usb_charger_state uchger_state;

	if (!uchger)
		return NOTIFY_BAD;

	/* Report event to power to setting the current limitation
	 * for this usb charger when one usb charger is added or removed
	 * with detecting by extcon device.
	 */
	if (state)
		uchger_state = USB_CHARGER_PRESENT;
	else
		uchger_state = USB_CHARGER_REMOVE;

	usb_charger_notify_others(uchger, uchger_state);

	return NOTIFY_OK;
}

/*
 * usb_charger_plug_by_gadget() - Set the usb charger current limitation
 * according to the usb gadget device state.
 * @gadget - the usb gadget device.
 * @state - the usb gadget state.
 */
int usb_charger_plug_by_gadget(struct usb_gadget *gadget,
			       unsigned long state)
{
	struct usb_charger *uchger = gadget->charger;
	enum usb_charger_state uchger_state;

	if (!uchger)
		return -EINVAL;

	/* Report event to power to setting the current limitation
	 * for this usb charger when one usb charger state is changed
	 * with detecting by usb gadget state.
	 */
	if (uchger->old_gadget_state != state) {
		uchger->old_gadget_state = state;

		if (state >= USB_STATE_ATTACHED)
			uchger_state = USB_CHARGER_PRESENT;
		else if (state == USB_STATE_NOTATTACHED)
			uchger_state = USB_CHARGER_REMOVE;
		else
			uchger_state = USB_CHARGER_DEFAULT;

		usb_charger_notify_others(uchger, uchger_state);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(usb_charger_plug_by_gadget);

static int devm_uchger_dev_match(struct device *dev, void *res, void *data)
{
	struct usb_charger **r = res;

	if (WARN_ON(!r || !*r))
		return 0;

	return *r == data;
}

static void usb_charger_release(struct device *dev)
{
	struct usb_charger *uchger = dev_get_drvdata(dev);

	kfree(uchger);
}

/*
 * usb_charger_unregister() - Unregister a usb charger device.
 * @uchger - the usb charger to be unregistered.
 */
static int usb_charger_unregister(struct usb_charger *uchger)
{
	if (!uchger)
		return -EINVAL;

	device_unregister(&uchger->dev);
	return 0;
}

static void devm_uchger_dev_unreg(struct device *dev, void *res)
{
	usb_charger_unregister(*(struct usb_charger **)res);
}

void devm_usb_charger_unregister(struct device *dev,
				 struct usb_charger *uchger)
{
	devres_release(dev, devm_uchger_dev_unreg,
		       devm_uchger_dev_match, uchger);
}

/*
 * usb_charger_register() - Register a new usb charger device
 * which is created by the usb charger framework.
 * @parent - the parent device of the new usb charger.
 * @uchger - the new usb charger device.
 */
static int usb_charger_register(struct device *parent,
				struct usb_charger *uchger)
{
	int ret;

	if (!uchger)
		return -EINVAL;

	uchger->dev.parent = parent;
	uchger->dev.release = usb_charger_release;
	uchger->dev.bus = &usb_charger_subsys;
	uchger->dev.groups = usb_charger_groups;

	ret = ida_simple_get(&usb_charger_ida, 0, 0, GFP_KERNEL);
	if (ret < 0)
		goto fail_ida;

	uchger->id = ret;
	dev_set_name(&uchger->dev, "usb-charger.%d", uchger->id);
	dev_set_drvdata(&uchger->dev, uchger);

	ret = device_register(&uchger->dev);
	if (ret)
		goto fail_register;

	return 0;

fail_register:
	put_device(&uchger->dev);
	ida_simple_remove(&usb_charger_ida, uchger->id);
	uchger->id = -1;
fail_ida:
	dev_err(parent, "Failed to register usb charger: %d\n", ret);
	return ret;
}

int devm_usb_charger_register(struct device *dev,
			      struct usb_charger *uchger)
{
	struct usb_charger **ptr;
	int ret;

	ptr = devres_alloc(devm_uchger_dev_unreg, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;

	ret = usb_charger_register(dev, uchger);
	if (ret) {
		devres_free(ptr);
		return ret;
	}

	*ptr = uchger;
	devres_add(dev, ptr);

	return 0;
}

int usb_charger_init(struct usb_gadget *ugadget)
{
	struct usb_charger *uchger;
	struct extcon_dev *edev;
	struct power_supply *psy;
	int ret;

	if (!ugadget)
		return -EINVAL;

	uchger = kzalloc(sizeof(struct usb_charger), GFP_KERNEL);
	if (!uchger)
		return -ENOMEM;

	uchger->type = UNKNOWN_TYPE;
	uchger->state = USB_CHARGER_DEFAULT;
	uchger->id = -1;
	uchger->cur_limit.sdp_cur_limit = DEFAULT_SDP_CUR_LIMIT;
	uchger->cur_limit.dcp_cur_limit = DEFAULT_DCP_CUR_LIMIT;
	uchger->cur_limit.cdp_cur_limit = DEFAULT_CDP_CUR_LIMIT;
	uchger->cur_limit.aca_cur_limit = DEFAULT_ACA_CUR_LIMIT;
	uchger->get_charger_type = NULL;

	mutex_init(&uchger->lock);
	RAW_INIT_NOTIFIER_HEAD(&uchger->uchger_nh);

	/* register a notifier on a extcon device if it is exsited */
	edev = extcon_get_edev_by_phandle(ugadget->dev.parent, 0);
	if (!IS_ERR_OR_NULL(edev)) {
		uchger->extcon_dev = edev;
		uchger->extcon_nb.nb.notifier_call = usb_charger_plug_by_extcon;
		uchger->extcon_nb.uchger = uchger;
		extcon_register_notifier(edev, EXTCON_USB,
					 &uchger->extcon_nb.nb);
	}

	/* to check if the usb charger is link to a power supply */
	psy = devm_power_supply_get_by_phandle(ugadget->dev.parent,
					       "power-supplies");
	if (!IS_ERR_OR_NULL(psy))
		uchger->psy = psy;
	else
		uchger->psy = NULL;

	/* register a notifier on a usb gadget device */
	uchger->gadget = ugadget;
	ugadget->charger = uchger;
	uchger->old_gadget_state = ugadget->state;

	/* register a new usb charger */
	ret = usb_charger_register(&ugadget->dev, uchger);
	if (ret)
		goto fail;

	return 0;

fail:
	if (uchger->extcon_dev)
		extcon_unregister_notifier(uchger->extcon_dev,
					   EXTCON_USB, &uchger->extcon_nb.nb);

	kfree(uchger);
	return ret;
}

int usb_charger_exit(struct usb_gadget *ugadget)
{
	struct usb_charger *uchger = ugadget->charger;

	if (!uchger)
		return -EINVAL;

	if (uchger->extcon_dev)
		extcon_unregister_notifier(uchger->extcon_dev,
					   EXTCON_USB, &uchger->extcon_nb.nb);

	ida_simple_remove(&usb_charger_ida, uchger->id);

	return usb_charger_unregister(uchger);
}

static int __init usb_charger_sysfs_init(void)
{
	return subsys_system_register(&usb_charger_subsys, NULL);
}
core_initcall(usb_charger_sysfs_init);

MODULE_AUTHOR("Baolin Wang <baolin.wang@linaro.org>");
MODULE_DESCRIPTION("USB charger driver");
MODULE_LICENSE("GPL");
