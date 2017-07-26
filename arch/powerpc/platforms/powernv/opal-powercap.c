/*
 * PowerNV OPAL Powercap interface
 *
 * Copyright 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#define pr_fmt(fmt)     "opal-powercap: " fmt

#include <linux/of.h>
#include <linux/kobject.h>
#include <linux/slab.h>

#include <asm/opal.h>

DEFINE_MUTEX(powercap_mutex);

static struct kobject *powercap_kobj;

struct powercap_attr {
	u32 handle;
	struct kobj_attribute attr;
};

static struct attribute_group *pattr_groups;
static struct powercap_attr *pcap_attrs;

static ssize_t powercap_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	struct powercap_attr *pcap_attr = container_of(attr,
						struct powercap_attr, attr);
	struct opal_msg msg;
	u32 pcap;
	int ret, token;

	token = opal_async_get_token_interruptible();
	if (token < 0) {
		pr_devel("Failed to get token\n");
		return token;
	}

	mutex_lock(&powercap_mutex);
	ret = opal_get_powercap(pcap_attr->handle, token, &pcap);
	switch (ret) {
	case OPAL_ASYNC_COMPLETION:
		ret = opal_async_wait_response(token, &msg);
		if (ret) {
			pr_devel("Failed to wait for the async response %d\n",
				 ret);
			goto out;
		}
		ret = opal_error_code(opal_get_async_rc(msg));
		if (!ret)
			ret = sprintf(buf, "%u\n", be32_to_cpu(pcap));
		break;
	case OPAL_SUCCESS:
		ret = sprintf(buf, "%u\n", be32_to_cpu(pcap));
		break;
	default:
		ret = opal_error_code(ret);
	}

out:
	mutex_unlock(&powercap_mutex);
	opal_async_release_token(token);
	return ret;
}

static ssize_t powercap_store(struct kobject *kobj,
			      struct kobj_attribute *attr, const char *buf,
			      size_t count)
{
	struct powercap_attr *pcap_attr = container_of(attr,
						struct powercap_attr, attr);
	struct opal_msg msg;
	u32 pcap;
	int ret, token;

	ret = kstrtoint(buf, 0, &pcap);
	if (ret)
		return ret;

	token = opal_async_get_token_interruptible();
	if (token < 0) {
		pr_devel("Failed to get token\n");
		return token;
	}

	mutex_lock(&powercap_mutex);
	ret = opal_set_powercap(pcap_attr->handle, token, pcap);
	switch (ret) {
	case OPAL_ASYNC_COMPLETION:
		ret = opal_async_wait_response(token, &msg);
		if (ret) {
			pr_devel("Failed to wait for the async response %d\n",
				 ret);
			goto out;
		}
		ret = opal_error_code(opal_get_async_rc(msg));
		if (!ret)
			ret = count;
		break;
	case OPAL_SUCCESS:
		ret = count;
		break;
	default:
		ret = opal_error_code(ret);
	}

out:
	mutex_unlock(&powercap_mutex);
	opal_async_release_token(token);

	return ret;
}

static void powercap_add_attr(int handle, const char *name,
			      struct powercap_attr *attr)
{
	attr->handle = handle;
	sysfs_attr_init(&attr->attr.attr);
	attr->attr.attr.name = name;
	attr->attr.attr.mode = 0444;
	attr->attr.show = powercap_show;
}

void __init opal_powercap_init(void)
{
	struct device_node *powercap, *node;
	int pattr_group_count = 0, total_attr_count = 0;
	int i, count;

	powercap = of_find_node_by_path("/ibm,opal/power-mgt/powercap");
	if (!powercap) {
		pr_devel("/ibm,opal/power-mgt/powercap node not found\n");
		return;
	}

	for_each_child_of_node(powercap, node)
		pattr_group_count++;

	pattr_groups = kcalloc(pattr_group_count,
			       sizeof(struct attribute_group), GFP_KERNEL);
	if (!pattr_groups)
		return;

	i = 0;
	for_each_child_of_node(powercap, node) {
		int attr_count = 0;

		if (of_find_property(node, "powercap-min", NULL))
			attr_count++;
		if (of_find_property(node, "powercap-max", NULL))
			attr_count++;
		if (of_find_property(node, "powercap-cur", NULL))
			attr_count++;

		total_attr_count += attr_count;
		pattr_groups[i].attrs = kcalloc(attr_count + 1,
						sizeof(struct attribute *),
						GFP_KERNEL);
		if (!pattr_groups[i].attrs) {
			while (--i >= 0)
				kfree(pattr_groups[i].attrs);
			goto out_pattr_groups;
		}
		i++;
	}

	pcap_attrs = kcalloc(total_attr_count, sizeof(struct powercap_attr),
			     GFP_KERNEL);
	if (!pcap_attrs)
		goto out_pattr_groups_attrs;

	count = 0;
	i = 0;
	for_each_child_of_node(powercap, node) {
		u32 handle;
		int j = 0;

		pattr_groups[i].name = node->name;

		if (!of_property_read_u32(node, "powercap-min", &handle)) {
			powercap_add_attr(handle, "powercap-min",
					  &pcap_attrs[count]);
			pattr_groups[i].attrs[j++] =
				&pcap_attrs[count++].attr.attr;
		}

		if (!of_property_read_u32(node, "powercap-max", &handle)) {
			powercap_add_attr(handle, "powercap-max",
					  &pcap_attrs[count]);
			pattr_groups[i].attrs[j++] =
				&pcap_attrs[count++].attr.attr;
		}

		if (!of_property_read_u32(node, "powercap-cur", &handle)) {
			powercap_add_attr(handle, "powercap-cur",
					  &pcap_attrs[count]);
			pcap_attrs[count].attr.attr.mode |= 0220;
			pcap_attrs[count].attr.store = powercap_store;
			pattr_groups[i].attrs[j++] =
				&pcap_attrs[count++].attr.attr;
		}
		i++;
	}

	powercap_kobj = kobject_create_and_add("powercap", opal_kobj);
	if (!powercap_kobj) {
		pr_warn("Failed to create powercap kobject\n");
		goto out_pcap_attrs;
	}

	for (i = 0; i < pattr_group_count; i++) {
		if (sysfs_create_group(powercap_kobj, &pattr_groups[i])) {
			pr_warn("Failed to create powercap attribute group %s\n",
				pattr_groups[i].name);
			goto out;
		}
	}

	return;
out:
	kobject_put(powercap_kobj);
out_pcap_attrs:
	kfree(pcap_attrs);
out_pattr_groups_attrs:
	while (--pattr_group_count >= 0)
		kfree(pattr_groups[i].attrs);
out_pattr_groups:
	kfree(pattr_groups);
}
