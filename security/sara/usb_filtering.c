/*
 * S.A.R.A. Linux Security Module
 *
 * Copyright (C) 2017 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 */

#ifdef CONFIG_SECURITY_SARA_USB_FILTERING

#include <linux/kref.h>
#include <linux/lsm_hooks.h>
#include <linux/mm.h>
#include <linux/spinlock.h>

#include "include/sara.h"
#include "include/utils.h"
#include "include/securityfs.h"
#include "include/usb_filtering.h"

#define SARA_USB_FILTERING_CONFIG_VERSION 0

#define SARA_USB_FILTERING_ALLOW 0
#define SARA_USB_FILTERING_DENY 1

struct usb_filtering_rule {
	u16 product_id;
	u16 vendor_id;
	u8 product_id_shift;
	u8 vendor_id_shift;
	char *bus_name;
	bool exact_bus_name;
	u8 port;
	u8 action;
};

struct usb_config_container {
	u32 rules_size;
	struct usb_filtering_rule *rules;
	size_t buf_len;
	struct kref refcount;
	char hash[SARA_CONFIG_HASH_LEN];
};

static struct usb_config_container __rcu *usb_filtering_config;

static const int usb_filtering_version =
				SARA_USB_FILTERING_CONFIG_VERSION;
static bool usb_filtering_enabled __read_mostly = true;
static DEFINE_SPINLOCK(usb_config_lock);

#ifdef CONFIG_SECURITY_SARA_USB_FILTERING_DENY
static int default_action __ro_after_init = SARA_USB_FILTERING_DENY;
#else
static int default_action __ro_after_init = SARA_USB_FILTERING_ALLOW;
#endif

static int __init sara_usb_filtering_enabled_setup(char *str)
{
	if (str[0] == '1' && str[1] == '\0')
		usb_filtering_enabled = true;
	else
		usb_filtering_enabled = false;
	return 1;
}
__setup("sara_usb_filtering=", sara_usb_filtering_enabled_setup);

static int __init sara_usb_filtering_default_setup(char *str)
{
	if (str[0] == 'd' && str[1] == '\0')
		default_action = SARA_USB_FILTERING_DENY;
	else
		default_action = SARA_USB_FILTERING_ALLOW;
	return 1;
}
__setup("sara_usb_filtering_default=", sara_usb_filtering_default_setup);

static int sara_usb_device_auth(const struct usb_device *udev)
{
	int i;
	int ret;
	u16 vid = le16_to_cpu(udev->descriptor.idVendor);
	u16 pid = le16_to_cpu(udev->descriptor.idProduct);
	const char *bus_name = udev->bus->bus_name;
	struct usb_config_container *c;

	if (!sara_enabled || !usb_filtering_enabled)
		return SARA_USB_FILTERING_ALLOW;

	pr_debug("USB filtering: new usb device found \"%04x:%04x\" on \"%s\" port \"%d\".\n",
		 vid, pid, bus_name, udev->portnum);

	SARA_CONFIG_GET_RCU(c, usb_filtering_config);
	for (i = 0; i < c->rules_size; ++i) {
		if ((vid >> c->rules[i].vendor_id_shift) ==
		    c->rules[i].vendor_id &&
		    (pid >> c->rules[i].product_id_shift) ==
		    c->rules[i].product_id) {
			if (!c->rules[i].port ||
			    c->rules[i].port == udev->portnum) {
				if (c->rules[i].exact_bus_name) {
					if (strcmp(bus_name,
						   c->rules[i].bus_name) == 0)
						goto match;
				} else if (strncmp(bus_name,
						c->rules[i].bus_name,
						strlen(c->rules[i].bus_name))
						== 0)
					goto match;
			}
		}
	}
	SARA_CONFIG_PUT_RCU(c);

	ret = default_action;
	if (ret == SARA_USB_FILTERING_ALLOW)
		pr_info("USB filtering: no match found for \"%04x:%04x\" on \"%s\" port \"%d\". Default action is ALLOW.\n",
			vid, pid, bus_name, udev->portnum);
	else
		pr_info("USB filtering: no match found for \"%04x:%04x\" on \"%s\" port \"%d\". Default action is DENY.\n",
			vid, pid, bus_name, udev->portnum);
	goto out;

match:
	ret = c->rules[i].action;
	SARA_CONFIG_PUT_RCU(c);
	if (ret == SARA_USB_FILTERING_ALLOW)
		pr_info("USB filtering: match found for \"%04x:%04x\" on \"%s\" port \"%d\". Action is ALLOW.\n",
			vid, pid, bus_name, udev->portnum);
	else
		pr_notice("USB filtering: match found for \"%04x:%04x\" on \"%s\" port \"%d\". Action is DENY.\n",
			  vid, pid, bus_name, udev->portnum);
out:
	return ret;
}

static struct security_hook_list usb_hooks[] __ro_after_init  = {
	LSM_HOOK_INIT(usb_device_auth, sara_usb_device_auth),
};

struct binary_config_header {
	char magic[8];
	__le32 version;
	__le32 rules_size;
	char hash[SARA_CONFIG_HASH_LEN];
} __packed;

struct binary_config_rule {
	__le16 product_id;
	__le16 vendor_id;
	u8 product_id_shift;
	u8 vendor_id_shift;
	u8 exact_bus_name;
	u8 action;
	u8 port;
	u8 bus_name_len;
} __packed;

static void config_free(struct usb_config_container *data)
{
	int i;

	for (i = 0; i < data->rules_size; ++i)
		kfree(data->rules[i].bus_name);
	kvfree(data->rules);
	kfree(data);
}

static int config_load(const char *buf, size_t buf_len)
{
	int ret;
	int i;
	size_t inc;
	const char *pos;
	struct usb_config_container *new;
	struct binary_config_header *h;
	struct binary_config_rule *r;

	ret = -EINVAL;
	if (unlikely(buf_len < sizeof(*h)))
		goto out;

	h = (struct binary_config_header *) buf;
	pos = buf + sizeof(*h);

	ret = -EINVAL;
	if (unlikely(memcmp(h->magic, "SARAUSBF", 8) != 0))
		goto out;
	if (unlikely(le32_to_cpu(h->version) != usb_filtering_version))
		goto out;

	ret = -ENOMEM;
	new = kmalloc(sizeof(*new), GFP_KERNEL);
	if (unlikely(new == NULL))
		goto out;
	kref_init(&new->refcount);
	new->rules_size = le32_to_cpu(h->rules_size);
	BUILD_BUG_ON(sizeof(new->hash) != sizeof(h->hash));
	memcpy(new->hash, h->hash, sizeof(new->hash));
	if (unlikely(new->rules_size == 0)) {
		new->rules = NULL;
		goto replace;
	}

	ret = -ENOMEM;
	new->rules = sara_kvcalloc(new->rules_size,
				   sizeof(*new->rules));
	if (unlikely(new->rules == NULL))
		goto out_new;
	for (i = 0; i < new->rules_size; ++i) {
		r = (struct binary_config_rule *) pos;
		pos += sizeof(*r);
		inc = pos-buf;

		ret = -EINVAL;
		if (unlikely(inc + r->bus_name_len > buf_len))
			goto out_rules;

		new->rules[i].product_id = le16_to_cpu(r->product_id);
		new->rules[i].vendor_id = le16_to_cpu(r->vendor_id);
		new->rules[i].product_id_shift = r->product_id_shift;
		new->rules[i].vendor_id_shift = r->vendor_id_shift;
		new->rules[i].exact_bus_name = r->exact_bus_name;
		new->rules[i].action = r->action;
		new->rules[i].port = r->port;

		if (unlikely(new->rules[i].product_id_shift > 16))
			goto out_rules;
		if (unlikely(new->rules[i].vendor_id_shift > 16))
			goto out_rules;
		if (unlikely((int) new->rules[i].exact_bus_name != 0 &&
			     (int) new->rules[i].exact_bus_name != 1))
			goto out_rules;
		if (unlikely(new->rules[i].action != 0 &&
			     new->rules[i].action != 1))
			goto out_rules;

		ret = -ENOMEM;
		new->rules[i].bus_name = kmalloc(r->bus_name_len+1, GFP_KERNEL);
		if (unlikely(new->rules[i].bus_name == NULL))
			goto out_rules;

		memcpy(new->rules[i].bus_name, pos, r->bus_name_len);
		new->rules[i].bus_name[r->bus_name_len] = '\0';
		pos += r->bus_name_len;
	}
	new->buf_len = (size_t) (pos-buf);

replace:
	SARA_CONFIG_REPLACE(usb_filtering_config,
			    new,
			    config_free,
			    &usb_config_lock);
	pr_notice("USB filtering: new rules loaded.\n");
	return 0;

out_rules:
	for (i = 0; i < new->rules_size; ++i)
		kfree(new->rules[i].bus_name);
	kvfree(new->rules);
out_new:
	kfree(new);
out:
	pr_warn("USB filtering: failed to load rules.\n");
	return ret;
}

static ssize_t config_dump(char **buf)
{
	int i;
	ssize_t ret;
	size_t buf_len;
	char *pos;
	char *mybuf;
	int rulen;
	struct usb_config_container *c;
	struct usb_filtering_rule *rc;
	struct binary_config_header *h;
	struct binary_config_rule *r;

	ret = -ENOMEM;
	SARA_CONFIG_GET(c, usb_filtering_config);
	buf_len = c->buf_len;
	mybuf = sara_kvmalloc(buf_len);
	if (unlikely(mybuf == NULL))
		goto out;
	rulen = c->rules_size;
	h = (struct binary_config_header *) mybuf;
	memcpy(h->magic, "SARAUSBF", 8);
	h->version = cpu_to_le32(SARA_USB_FILTERING_CONFIG_VERSION);
	h->rules_size = cpu_to_le32(rulen);
	BUILD_BUG_ON(sizeof(c->hash) != sizeof(h->hash));
	memcpy(h->hash, c->hash, sizeof(h->hash));
	pos = mybuf + sizeof(*h);
	for (i = 0; i < rulen; ++i) {
		r = (struct binary_config_rule *) pos;
		pos += sizeof(*r);
		if (buf_len < (pos - mybuf))
			goto out;
		rc = &c->rules[i];
		r->product_id = cpu_to_le16(rc->product_id);
		r->vendor_id = cpu_to_le16(rc->vendor_id);
		r->product_id_shift = rc->product_id_shift;
		r->vendor_id_shift = rc->vendor_id_shift;
		r->exact_bus_name = (u8) rc->exact_bus_name;
		r->action = rc->action;
		r->port = rc->port;
		r->bus_name_len = strlen(rc->bus_name);
		if (buf_len < ((pos - mybuf) + r->bus_name_len))
			goto out;
		memcpy(pos, rc->bus_name, r->bus_name_len);
		pos += r->bus_name_len;
	}
	ret = (ssize_t) (pos - mybuf);
	*buf = mybuf;
out:
	SARA_CONFIG_PUT(c, config_free);
	return ret;
}

static int config_hash(char **buf)
{
	int ret;
	struct usb_config_container *config;

	ret = -ENOMEM;
	*buf = kzalloc(sizeof(config->hash), GFP_KERNEL);
	if (unlikely(*buf == NULL))
		goto out;

	SARA_CONFIG_GET_RCU(config, usb_filtering_config);
	memcpy(*buf, config->hash, sizeof(config->hash));
	SARA_CONFIG_PUT_RCU(config);

	ret = 0;
out:
	return ret;
}

static DEFINE_SARA_SECFS_BOOL_FLAG(usb_filtering_enabled_data,
				   usb_filtering_enabled);

static struct sara_secfs_fptrs fptrs __ro_after_init = {
	.load = config_load,
	.dump = config_dump,
	.hash = config_hash,
};

static const struct sara_secfs_node usb_filtering_fs[] __initconst = {
	{
		.name = "enabled",
		.type = SARA_SECFS_BOOL,
		.data = (void *) &usb_filtering_enabled_data,
	},
	{
		.name = "version",
		.type = SARA_SECFS_READONLY_INT,
		.data = (int *) &usb_filtering_version,
	},
	{
		.name = "default_action",
		.type = SARA_SECFS_READONLY_INT,
		.data = &default_action,
	},
	{
		.name = ".load",
		.type = SARA_SECFS_CONFIG_LOAD,
		.data = &fptrs,
	},
	{
		.name = ".dump",
		.type = SARA_SECFS_CONFIG_DUMP,
		.data = &fptrs,
	},
	{
		.name = "hash",
		.type = SARA_SECFS_CONFIG_HASH,
		.data = &fptrs,
	},
};

int __init sara_usb_filtering_init(void)
{
	int ret;
	struct usb_config_container *tmpc;

	ret = -ENOMEM;
	tmpc = kzalloc(sizeof(*tmpc), GFP_KERNEL);
	if (unlikely(tmpc == NULL))
		goto out_fail;
	tmpc->buf_len = sizeof(struct binary_config_header);
	kref_init(&tmpc->refcount);
	usb_filtering_config = (struct usb_config_container __rcu *) tmpc;
	ret = sara_secfs_subtree_register("usb_filtering",
					  usb_filtering_fs,
					  ARRAY_SIZE(usb_filtering_fs));
	if (unlikely(ret))
		goto out_fail;
	security_add_hooks(usb_hooks, ARRAY_SIZE(usb_hooks), "sara");
	return 0;

out_fail:
	kfree(tmpc);
	return ret;
}

#endif /* CONFIG_SECURITY_SARA_USB_FILTERING */
