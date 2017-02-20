/*
 * charger.c -- USB charger driver
 *
 * Copyright (C) 2016 Linaro Ltd.
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
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/charger.h>

/* Default current range by charger type. */
#define DEFAULT_SDP_CUR_MIN	2
#define DEFAULT_SDP_CUR_MAX	500
#define DEFAULT_SDP_CUR_MIN_SS	150
#define DEFAULT_SDP_CUR_MAX_SS	900
#define DEFAULT_DCP_CUR_MIN	500
#define DEFAULT_DCP_CUR_MAX	5000
#define DEFAULT_CDP_CUR_MIN	1500
#define DEFAULT_CDP_CUR_MAX	5000
#define DEFAULT_ACA_CUR_MIN	1500
#define DEFAULT_ACA_CUR_MAX	5000

static DEFINE_IDA(usb_charger_ida);
static LIST_HEAD(charger_list);
static DEFINE_MUTEX(charger_lock);

static struct usb_charger *dev_to_uchger(struct device *dev)
{
	return NULL;
}

/*
 * charger_current_show() - Show the charger current.
 */
static ssize_t charger_current_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct usb_charger *uchger = dev_to_uchger(dev);
	unsigned int min, max;

	usb_charger_get_current(uchger, &min, &max);
	return sprintf(buf, "%u-%u\n", min, max);
}
static DEVICE_ATTR_RO(charger_current);

/*
 * charger_type_show() - Show the charger type.
 *
 * It can be SDP/DCP/CDP/ACA type, else for unknown type.
 */
static ssize_t charger_type_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct usb_charger *uchger = dev_to_uchger(dev);
	enum usb_charger_type type = usb_charger_get_type(uchger);
	int cnt;

	switch (type) {
	case SDP_TYPE:
		cnt = sprintf(buf, "%s\n", "SDP");
		break;
	case DCP_TYPE:
		cnt = sprintf(buf, "%s\n", "DCP");
		break;
	case CDP_TYPE:
		cnt = sprintf(buf, "%s\n", "CDP");
		break;
	case ACA_TYPE:
		cnt = sprintf(buf, "%s\n", "ACA");
		break;
	default:
		cnt = sprintf(buf, "%s\n", "UNKNOWN");
		break;
	}

	return cnt;
}
static DEVICE_ATTR_RO(charger_type);

/*
 * charger_state_show() - Show the charger state.
 *
 * Charger state can be present or removed.
 */
static ssize_t charger_state_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct usb_charger *uchger = dev_to_uchger(dev);
	enum usb_charger_state state = usb_charger_get_state(uchger);
	int cnt;

	switch (state) {
	case USB_CHARGER_PRESENT:
		cnt = sprintf(buf, "%s\n", "PRESENT");
		break;
	case USB_CHARGER_ABSENT:
		cnt = sprintf(buf, "%s\n", "REMOVE");
		break;
	default:
		cnt = sprintf(buf, "%s\n", "UNKNOWN");
		break;
	}

	return cnt;
}
static DEVICE_ATTR_RO(charger_state);

static struct attribute *usb_charger_attrs[] = {
	&dev_attr_charger_current.attr,
	&dev_attr_charger_type.attr,
	&dev_attr_charger_state.attr,
	NULL
};

static const struct attribute_group usb_charger_group = {
	.name = "charger",
	.attrs = usb_charger_attrs,
};
__ATTRIBUTE_GROUPS(usb_charger);

/*
 * usb_charger_get_instance() - Get the first usb charger instance.
 *
 * Note: We assume that there is only one USB charger in the system.
 */
struct usb_charger *usb_charger_get_instance()
{
	struct usb_charger *uchger;

	mutex_lock(&charger_lock);
	list_for_each_entry(uchger, &charger_list, list) {
		if (uchger->name)
			break;
	}
	mutex_unlock(&charger_lock);

	if (WARN(!uchger, "can't find usb charger"))
		return ERR_PTR(-ENODEV);

	return uchger;
}
EXPORT_SYMBOL_GPL(usb_charger_get_instance);

/*
 * usb_charger_get_type() - get the usb charger type with lock protection.
 * @uchger - usb charger instance.
 *
 * Users can get the charger type by this safe API, rather than using the
 * usb_charger structure directly.
 */
enum usb_charger_type usb_charger_get_type(struct usb_charger *uchger)
{
	enum usb_charger_type type;

	mutex_lock(&uchger->lock);
	type = uchger->type;
	mutex_unlock(&uchger->lock);

	return type;
}
EXPORT_SYMBOL_GPL(usb_charger_get_type);

/*
 * usb_charger_get_state() - Get the charger state with lock protection.
 * @uchger - the usb charger instance.
 *
 * Users should get the charger state by this safe API.
 */
enum usb_charger_state usb_charger_get_state(struct usb_charger *uchger)
{
	enum usb_charger_state state;

	mutex_lock(&uchger->lock);
	state = uchger->state;
	mutex_unlock(&uchger->lock);

	return state;
}
EXPORT_SYMBOL_GPL(usb_charger_get_state);

/*
 * usb_charger_detect_type() - detect the charger type manually.
 * @uchger - usb charger instance.
 *
 * Note: You should ensure you need to detect the charger type manually on
 * your platform. You should call it at the right gadget state to avoid
 * affecting gadget enumeration.
 */
int usb_charger_detect_type(struct usb_charger *uchger)
{
	enum usb_charger_type type;

	if (!uchger->charger_detect)
		return -EINVAL;

	type = uchger->charger_detect(uchger);

	mutex_lock(&uchger->lock);
	uchger->type = type;
	mutex_unlock(&uchger->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_charger_detect_type);

/*
 * usb_charger_set_default_current() - Set default current for charger.
 * @uchger - usb charger instance.
 */
static void usb_charger_set_default_current(struct usb_charger *uchger)
{
	uchger->cur.sdp_min = DEFAULT_SDP_CUR_MIN;
	uchger->cur.sdp_max = DEFAULT_SDP_CUR_MAX;
	uchger->cur.dcp_min = DEFAULT_DCP_CUR_MIN;
	uchger->cur.dcp_max = DEFAULT_DCP_CUR_MAX;
	uchger->cur.cdp_min = DEFAULT_CDP_CUR_MIN;
	uchger->cur.cdp_max = DEFAULT_CDP_CUR_MAX;
	uchger->cur.aca_min = DEFAULT_ACA_CUR_MIN;
	uchger->cur.aca_max = DEFAULT_ACA_CUR_MAX;
	uchger->sdp_default_cur_change = 0;
}

/*
 * __usb_charger_get_current() - Get the charger current.
 * @uchger - the usb charger instance.
 * @min - return the minimum current.
 * @max - return the maximum current.
 *
 * Callers should get the charger lock before issuing this function.
 */
static void __usb_charger_get_current(struct usb_charger *uchger,
				      unsigned int *min,
				      unsigned int *max)
{
	enum usb_charger_type type = uchger->type;

	switch (type) {
	case SDP_TYPE:
		/*
		 * For super speed gadget, the default charger maximum current
		 * should be 900 mA and the default minimum current should be
		 * 150mA.
		 */
		if (uchger->gadget &&
		    uchger->gadget->speed >= USB_SPEED_SUPER) {
			if (!uchger->sdp_default_cur_change)
				uchger->cur.sdp_max = DEFAULT_SDP_CUR_MAX_SS;

			uchger->cur.sdp_min = DEFAULT_SDP_CUR_MIN_SS;
		}

		*min = uchger->cur.sdp_min;
		*max = uchger->cur.sdp_max;
		break;
	case DCP_TYPE:
		*min = uchger->cur.dcp_min;
		*max = uchger->cur.dcp_max;
		break;
	case CDP_TYPE:
		*min = uchger->cur.cdp_min;
		*max = uchger->cur.cdp_max;
		break;
	case ACA_TYPE:
		*min = uchger->cur.aca_min;
		*max = uchger->cur.aca_max;
		break;
	default:
		*min = 0;
		*max = 0;
		break;
	}
}

/*
 * usb_charger_get_cur_limit() - Get the maximum charger current.
 * @uchger - the usb charger instance.
 *
 * This function should under the charger lock protection.
 */
static unsigned int usb_charger_get_cur_limit(struct usb_charger *uchger)
{
	unsigned int min_cur, max_cur;

	__usb_charger_get_current(uchger, &min_cur, &max_cur);
	return max_cur;
}

/*
 * usb_charger_get_current() - Get the charger current with lock protection.
 * @uchger - the usb charger instance.
 * @min - return the minimum current.
 * @max - return the maximum current.
 *
 * Users should get the charger current by this safe API.
 */
int usb_charger_get_current(struct usb_charger *uchger,
			    unsigned int *min,
			    unsigned int *max)
{
	if (!uchger)
		return -EINVAL;

	mutex_lock(&uchger->lock);
	__usb_charger_get_current(uchger, min, max);
	mutex_unlock(&uchger->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_charger_get_current);

/*
 * usb_charger_notify_work() - Notify users the current has changed by work.
 * @work - the work instance.
 *
 * Note: When users receive the charger present event, they should check the
 * charger current by usb_charger_get_current() API.
 */
static void usb_charger_notify_work(struct work_struct *work)
{
	struct usb_charger *uchger = work_to_charger(work);

	mutex_lock(&uchger->lock);
	if (uchger->state == USB_CHARGER_PRESENT) {
		raw_notifier_call_chain(&uchger->uchger_nh,
					usb_charger_get_cur_limit(uchger),
					uchger);
	}
	mutex_unlock(&uchger->lock);
}

/*
 * __usb_charger_set_cur_limit_by_type() - Set the current limitation
 * by charger type.
 * @uchger - the usb charger instance.
 * @type - the usb charger type.
 * @cur_limit - the current limitation.
 */
static int __usb_charger_set_cur_limit_by_type(struct usb_charger *uchger,
					       enum usb_charger_type type,
					       unsigned int cur_limit)
{
	switch (type) {
	case SDP_TYPE:
		if (uchger->gadget && uchger->gadget->speed >= USB_SPEED_SUPER) {
			uchger->cur.sdp_max =
				(cur_limit > DEFAULT_SDP_CUR_MAX_SS) ?
				DEFAULT_SDP_CUR_MAX_SS : cur_limit;
		} else {
			uchger->cur.sdp_max =
				(cur_limit > DEFAULT_SDP_CUR_MAX) ?
				DEFAULT_SDP_CUR_MAX : cur_limit;
		}

		uchger->sdp_default_cur_change = 1;
		break;
	case DCP_TYPE:
		uchger->cur.dcp_max = (cur_limit > DEFAULT_DCP_CUR_MAX) ?
			DEFAULT_DCP_CUR_MAX : cur_limit;
		break;
	case CDP_TYPE:
		uchger->cur.cdp_max = (cur_limit > DEFAULT_CDP_CUR_MAX) ?
			DEFAULT_CDP_CUR_MAX : cur_limit;
		break;
	case ACA_TYPE:
		uchger->cur.aca_max = (cur_limit > DEFAULT_ACA_CUR_MAX) ?
			DEFAULT_ACA_CUR_MAX : cur_limit;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * usb_charger_set_cur_limit_by_gadget() - Set the current limitation from
 * gadget layer.
 * @gadget - the usb gadget device.
 * @cur_limit - the current limitation.
 *
 * Note: This function is used in atomic contexts without mutex lock.
 */
int usb_charger_set_cur_limit_by_gadget(struct usb_gadget *gadget,
					unsigned int cur_limit)
{
	return 0;
}
EXPORT_SYMBOL_GPL(usb_charger_set_cur_limit_by_gadget);

/*
 * usb_charger_set_cur_limit_by_type() - Set the current limitation
 * by charger type with lock protection.
 * @uchger - the usb charger instance.
 * @type - the usb charger type.
 * @cur_limit - the current limitation.
 *
 * Users should set the current limitation by this lock protection API.
 */
int usb_charger_set_cur_limit_by_type(struct usb_charger *uchger,
				      enum usb_charger_type type,
				      unsigned int cur_limit)
{
	int ret;

	if (!uchger)
		return -EINVAL;

	mutex_lock(&uchger->lock);
	ret = __usb_charger_set_cur_limit_by_type(uchger, type, cur_limit);
	mutex_unlock(&uchger->lock);
	if (ret)
		return ret;

	schedule_work(&uchger->work);
	return ret;
}
EXPORT_SYMBOL_GPL(usb_charger_set_cur_limit_by_type);

/*
 * usb_charger_register_notify() - Register a notifiee to get notified by
 * any attach status changes from the usb charger detection.
 * @uchger - the usb charger instance which is monitored.
 * @nb - a notifier block to be registered.
 */
int usb_charger_register_notify(struct usb_charger *uchger,
				struct notifier_block *nb)
{
	int ret;

	if (!uchger || !nb) {
		pr_err("Charger or nb can not be NULL.\n");
		return -EINVAL;
	}

	mutex_lock(&uchger->lock);
	ret = raw_notifier_chain_register(&uchger->uchger_nh, nb);
	mutex_unlock(&uchger->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(usb_charger_register_notify);

/*
 * usb_charger_unregister_notify() - Unregister a notifiee from the usb charger.
 * @uchger - the usb charger instance which is monitored.
 * @nb - a notifier block to be unregistered.
 */
int usb_charger_unregister_notify(struct usb_charger *uchger,
				  struct notifier_block *nb)
{
	int ret;

	if (!uchger || !nb) {
		pr_err("Charger or nb can not be NULL.\n");
		return -EINVAL;
	}

	mutex_lock(&uchger->lock);
	ret = raw_notifier_chain_unregister(&uchger->uchger_nh, nb);
	mutex_unlock(&uchger->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(usb_charger_unregister_notify);

/*
 * usb_charger_notify_state() - It will notify other device registered
 * on usb charger when the usb charger state is changed.
 * @uchger - the usb charger instance.
 * @state - the state of the usb charger.
 *
 * Note: When notify the charger present state to power driver, the power driver
 * should get the current by usb_charger_get_current() API to set current.
 */
static void usb_charger_notify_state(struct usb_charger *uchger,
				     enum usb_charger_state state)
{
	char uchger_state[50] = { 0 };
	char *envp[] = { uchger_state, NULL };

	mutex_lock(&uchger->lock);
	if (uchger->state == state) {
		mutex_unlock(&uchger->lock);
		return;
	}

	uchger->state = state;

	switch (state) {
	case USB_CHARGER_PRESENT:
		raw_notifier_call_chain(&uchger->uchger_nh,
					usb_charger_get_cur_limit(uchger),
					uchger);
		snprintf(uchger_state, ARRAY_SIZE(uchger_state),
			 "USB_CHARGER_STATE=%s", "USB_CHARGER_PRESENT");
		break;
	case USB_CHARGER_ABSENT:
		uchger->type = UNKNOWN_TYPE;
		usb_charger_set_default_current(uchger);
		raw_notifier_call_chain(&uchger->uchger_nh, 0, uchger);
		snprintf(uchger_state, ARRAY_SIZE(uchger_state),
			 "USB_CHARGER_STATE=%s", "USB_CHARGER_ABSENT");
		break;
	default:
		pr_warn("Unknown USB charger state: %d\n", state);
		mutex_unlock(&uchger->lock);
		return;
	}

	kobject_uevent_env(&uchger->gadget->dev.kobj, KOBJ_CHANGE, envp);
	mutex_unlock(&uchger->lock);
}

/*
 * usb_charger_type_by_extcon() - The notifier call function which is registered
 * on the extcon device.
 * @nb - the notifier block that notified by extcon device.
 * @state - the extcon device state.
 * @data - here specify a extcon device.
 *
 * return the notify flag.
 */
static int usb_charger_type_by_extcon(struct notifier_block *nb,
				      unsigned long state, void *data)
{
	struct usb_charger_nb *extcon_nb =
		container_of(nb, struct usb_charger_nb, nb);
	struct usb_charger *uchger = extcon_nb->uchger;
	enum usb_charger_state uchger_state;
	enum usb_charger_type type;

	if (WARN(!uchger, "charger can not be NULL"))
		return NOTIFY_BAD;

	/* Determine charger type */
	if (extcon_get_cable_state_(uchger->extcon_dev,
				    EXTCON_CHG_USB_SDP) > 0) {
		type = SDP_TYPE;
		uchger_state = USB_CHARGER_PRESENT;
	} else if (extcon_get_cable_state_(uchger->extcon_dev,
					   EXTCON_CHG_USB_CDP) > 0) {
		type = CDP_TYPE;
		uchger_state = USB_CHARGER_PRESENT;
	} else if (extcon_get_cable_state_(uchger->extcon_dev,
					   EXTCON_CHG_USB_DCP) > 0) {
		type = DCP_TYPE;
		uchger_state = USB_CHARGER_PRESENT;
	} else if (extcon_get_cable_state_(uchger->extcon_dev,
					   EXTCON_CHG_USB_ACA) > 0) {
		type = ACA_TYPE;
		uchger_state = USB_CHARGER_PRESENT;
	} else {
		type = UNKNOWN_TYPE;
		uchger_state = USB_CHARGER_ABSENT;
	}

	mutex_lock(&uchger->lock);
	uchger->type = type;
	mutex_unlock(&uchger->lock);

	usb_charger_notify_state(uchger, uchger_state);

	return NOTIFY_OK;
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
static int usb_charger_plug_by_extcon(struct notifier_block *nb,
				      unsigned long state, void *data)
{
	struct usb_charger_nb *extcon_nb =
		container_of(nb, struct usb_charger_nb, nb);
	struct usb_charger *uchger = extcon_nb->uchger;
	enum usb_charger_state uchger_state;

	if (WARN(!uchger, "charger can not be NULL"))
		return NOTIFY_BAD;

	/*
	 * Report event to power users to setting the current limitation
	 * for this usb charger when one usb charger is added or removed
	 * with detecting by extcon device.
	 */
	if (state)
		uchger_state = USB_CHARGER_PRESENT;
	else
		uchger_state = USB_CHARGER_ABSENT;

	usb_charger_notify_state(uchger, uchger_state);

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
	return 0;
}
EXPORT_SYMBOL_GPL(usb_charger_plug_by_gadget);

/*
 * usb_charger_register() - Register a new usb charger.
 * @uchger - the new usb charger instance.
 */
static int usb_charger_register(struct usb_charger *uchger)
{
	int ret;

	ret = ida_simple_get(&usb_charger_ida, 0, 0, GFP_KERNEL);
	if (ret < 0) {
		pr_err("Failed to register usb charger: %d\n", ret);
		return ret;
	}

	uchger->id = ret;
	scnprintf(uchger->name, CHARGER_NAME_MAX, "usb-charger.%d", uchger->id);

	ret = sysfs_create_groups(&uchger->gadget->dev.kobj,
				  usb_charger_groups);
	if (ret) {
		pr_err("Failed to create usb charger attributes: %d\n", ret);
		ida_simple_remove(&usb_charger_ida, uchger->id);
		uchger->id = -1;
		return ret;
	}

	mutex_lock(&charger_lock);
	list_add_tail(&uchger->list, &charger_list);
	mutex_unlock(&charger_lock);

	return 0;
}

int usb_charger_init(struct usb_gadget *ugadget)
{
	struct usb_charger *uchger;
	struct extcon_dev *edev;
	int ret;

	uchger = kzalloc(sizeof(struct usb_charger), GFP_KERNEL);
	if (!uchger)
		return -ENOMEM;

	uchger->type = UNKNOWN_TYPE;
	uchger->state = USB_CHARGER_DEFAULT;
	uchger->id = -1;
	usb_charger_set_default_current(uchger);

	mutex_init(&uchger->lock);
	RAW_INIT_NOTIFIER_HEAD(&uchger->uchger_nh);
	INIT_WORK(&uchger->work, usb_charger_notify_work);

	/* Register a notifier on a extcon device if it is exsited */
	edev = extcon_get_edev_by_phandle(ugadget->dev.parent, 0);
	if (!IS_ERR_OR_NULL(edev)) {
		uchger->extcon_dev = edev;
		uchger->extcon_nb.nb.notifier_call = usb_charger_plug_by_extcon;
		uchger->extcon_nb.uchger = uchger;
		ret = extcon_register_notifier(edev, EXTCON_USB,
					       &uchger->extcon_nb.nb);
		if (ret) {
			pr_err("Failed to register extcon USB notifier.\n");
			goto fail_extcon;
		}

		uchger->extcon_type_nb.nb.notifier_call =
					usb_charger_type_by_extcon;
		uchger->extcon_type_nb.uchger = uchger;

		ret = extcon_register_notifier(edev, EXTCON_CHG_USB_SDP,
					       &uchger->extcon_type_nb.nb);
		if (ret) {
			pr_err("Failed to register extcon USB SDP notifier.\n");
			goto fail_extcon_sdp;
		}

		ret = extcon_register_notifier(edev, EXTCON_CHG_USB_CDP,
					       &uchger->extcon_type_nb.nb);
		if (ret) {
			pr_err("Failed to register extcon USB CDP notifier.\n");
			goto fail_extcon_cdp;
		}

		ret = extcon_register_notifier(edev, EXTCON_CHG_USB_DCP,
					       &uchger->extcon_type_nb.nb);
		if (ret) {
			pr_err("Failed to register extcon USB DCP notifier.\n");
			goto fail_extcon_dcp;
		}

		ret = extcon_register_notifier(edev, EXTCON_CHG_USB_ACA,
					       &uchger->extcon_type_nb.nb);
		if (ret) {
			pr_err("Failed to register extcon USB ACA notifier.\n");
			goto fail_extcon_aca;
		}
	}

	uchger->gadget = ugadget;
	uchger->old_gadget_state = USB_STATE_NOTATTACHED;

	/* Register a new usb charger */
	ret = usb_charger_register(uchger);
	if (ret)
		goto fail_register;

	return 0;

fail_register:
	extcon_unregister_notifier(uchger->extcon_dev,
				   EXTCON_CHG_USB_ACA,
				   &uchger->extcon_type_nb.nb);
fail_extcon_aca:
	extcon_unregister_notifier(uchger->extcon_dev,
				   EXTCON_CHG_USB_DCP,
				   &uchger->extcon_type_nb.nb);
fail_extcon_dcp:
	extcon_unregister_notifier(uchger->extcon_dev,
				   EXTCON_CHG_USB_CDP,
				   &uchger->extcon_type_nb.nb);
fail_extcon_cdp:
	extcon_unregister_notifier(uchger->extcon_dev,
				   EXTCON_CHG_USB_SDP,
				   &uchger->extcon_type_nb.nb);
fail_extcon_sdp:
	extcon_unregister_notifier(uchger->extcon_dev,
				   EXTCON_USB,
				   &uchger->extcon_nb.nb);
fail_extcon:
	kfree(uchger);

	return ret;
}

int usb_charger_exit(struct usb_gadget *ugadget)
{
	return 0;
}

MODULE_AUTHOR("Baolin Wang <baolin.wang@linaro.org>");
MODULE_DESCRIPTION("USB charger driver");
MODULE_LICENSE("GPL v2");
