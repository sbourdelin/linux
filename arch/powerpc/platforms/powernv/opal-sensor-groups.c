/*
 * PowerNV OPAL Sensor-groups interface
 *
 * Copyright 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#define pr_fmt(fmt)     "opal-sensor-groups: " fmt

#include <linux/of.h>
#include <linux/kobject.h>
#include <linux/slab.h>

#include <asm/opal.h>

DEFINE_MUTEX(sg_mutex);

static struct kobject *sg_kobj;

struct sg_attr {
	u32 handle;
	struct kobj_attribute attr;
	u32 opal_no;
	int enable;
};

static struct sensor_group {
	char name[20];
	struct attribute_group sg;
	struct sg_attr *sgattrs;
} *sgs;

static int sensor_group_clear(u32 handle)
{
	struct opal_msg msg;
	int token, ret;

	token = opal_async_get_token_interruptible();
	if (token < 0)
		return token;

	ret = opal_sensor_group_clear(handle, token);
	if (ret == OPAL_ASYNC_COMPLETION) {
		ret = opal_async_wait_response(token, &msg);
		if (ret) {
			pr_devel("Failed to wait for the async response\n");
			ret = -EIO;
			goto out;
		}
		ret = opal_error_code(opal_get_async_rc(msg));
	} else {
		ret = opal_error_code(ret);
	}

out:
	opal_async_release_token(token);
	return ret;
}

static int sensor_group_enable(u32 handle, int enable)
{
	struct opal_msg msg;
	int token, ret;

	token = opal_async_get_token_interruptible();
	if (token < 0)
		return token;

	ret = opal_sensor_group_enable(handle, token, enable);
	if (ret == OPAL_ASYNC_COMPLETION) {
		ret = opal_async_wait_response(token, &msg);
		if (ret) {
			pr_devel("Failed to wait for the async response\n");
			ret = -EIO;
			goto out;
		}
		ret = opal_error_code(opal_get_async_rc(msg));
	} else {
		ret = opal_error_code(ret);
	}

out:
	opal_async_release_token(token);
	return ret;
}

static ssize_t sg_store(struct kobject *kobj, struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	struct sg_attr *sattr = container_of(attr, struct sg_attr, attr);
	u32 data;
	int ret;

	ret = kstrtoint(buf, 0, &data);
	if (ret)
		return ret;

	ret = mutex_lock_interruptible(&sg_mutex);
	if (ret)
		return ret;

	ret = -EINVAL;
	switch (sattr->opal_no) {
	case OPAL_SENSOR_GROUP_CLEAR:
		if (data == 1)
			ret = sensor_group_clear(sattr->handle);
		break;
	case OPAL_SENSOR_GROUP_ENABLE:
		if (data == 0 || data == 1) {
			if (data != sattr->enable) {
				ret = sensor_group_enable(sattr->handle, data);
				if (!ret)
					sattr->enable = data;
			} else {
				ret = 0;
			}
		}
		break;
	default:
		break;
	}

	if (!ret)
		ret = count;

	mutex_unlock(&sg_mutex);
	return ret;
}

static ssize_t sg_show(struct kobject *kobj, struct kobj_attribute *attr,
		       char *buf)
{
	struct sg_attr *sattr = container_of(attr, struct sg_attr, attr);

	return sprintf(buf, "%d\n", sattr->enable);
}

static struct sg_ops_info {
	int opal_no;
	const char *attr_name;
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr,
			const char *buf, size_t count);
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf);
	umode_t mode;
} ops_info[] = {
	{ OPAL_SENSOR_GROUP_CLEAR, "clear", sg_store, NULL, 0220 },
	{ OPAL_SENSOR_GROUP_ENABLE, "enable", sg_store, sg_show, 0660 },
};

static void add_attr(int handle, struct sg_attr *attr, int index)
{
	attr->handle = handle;
	attr->opal_no = ops_info[index].opal_no;
	sysfs_attr_init(&attr->attr.attr);
	attr->attr.attr.name = ops_info[index].attr_name;
	attr->attr.attr.mode = ops_info[index].mode;
	attr->attr.store = ops_info[index].store;
	attr->attr.show = ops_info[index].show;
	if (attr->opal_no == OPAL_SENSOR_GROUP_ENABLE)
		attr->enable = 1;
}

static int add_attr_group(const __be32 *ops, int len, struct sensor_group *sg,
			   u32 handle)
{
	int i, j;
	int count = 0;

	for (i = 0; i < len; i++)
		for (j = 0; j < ARRAY_SIZE(ops_info); j++)
			if (be32_to_cpu(ops[i]) == ops_info[j].opal_no) {
				add_attr(handle, &sg->sgattrs[count], j);
				sg->sg.attrs[count] =
					&sg->sgattrs[count].attr.attr;
				count++;
			}

	return sysfs_create_group(sg_kobj, &sg->sg);
}

static int get_nr_attrs(const __be32 *ops, int len)
{
	int i, j;
	int nr_attrs = 0;

	for (i = 0; i < len; i++)
		for (j = 0; j < ARRAY_SIZE(ops_info); j++)
			if (be32_to_cpu(ops[i]) == ops_info[j].opal_no)
				nr_attrs++;

	return nr_attrs;
}

void __init opal_sensor_groups_init(void)
{
	struct device_node *sg, *node;
	int i = 0;

	sg = of_find_compatible_node(NULL, NULL, "ibm,opal-sensor-group");
	if (!sg) {
		pr_devel("Sensor groups node not found\n");
		return;
	}

	sgs = kcalloc(of_get_child_count(sg), sizeof(*sgs), GFP_KERNEL);
	if (!sgs)
		return;

	sg_kobj = kobject_create_and_add("sensor_groups", opal_kobj);
	if (!sg_kobj) {
		pr_warn("Failed to create sensor group kobject\n");
		goto out_sgs;
	}

	for_each_child_of_node(sg, node) {
		const __be32 *ops;
		u32 sgid, len, nr_attrs, chipid;

		ops = of_get_property(node, "ops", &len);
		if (!ops)
			continue;

		nr_attrs = get_nr_attrs(ops, len);
		if (!nr_attrs)
			continue;

		sgs[i].sgattrs = kcalloc(nr_attrs, sizeof(struct sg_attr),
					 GFP_KERNEL);
		if (!sgs[i].sgattrs)
			goto out_sgs_sgattrs;

		sgs[i].sg.attrs = kcalloc(nr_attrs + 1,
					  sizeof(struct attribute *),
					  GFP_KERNEL);

		if (!sgs[i].sg.attrs) {
			kfree(sgs[i].sgattrs);
			goto out_sgs_sgattrs;
		}

		if (of_property_read_u32(node, "sensor-group-id", &sgid)) {
			pr_warn("sensor-group-id property not found\n");
			goto out_sgs_sgattrs;
		}

		if (!of_property_read_u32(node, "ibm,chip-id", &chipid))
			sprintf(sgs[i].name, "%s%d", node->name, chipid);
		else
			sprintf(sgs[i].name, "%s", node->name);

		sgs[i].sg.name = sgs[i].name;
		if (add_attr_group(ops, len, &sgs[i], sgid)) {
			pr_warn("Failed to create sensor attribute group %s\n",
				sgs[i].sg.name);
			goto out_sgs_sgattrs;
		}
		i++;
	}

	return;

out_sgs_sgattrs:
	while (--i >= 0) {
		kfree(sgs[i].sgattrs);
		kfree(sgs[i].sg.attrs);
	}
	kobject_put(sg_kobj);
out_sgs:
	kfree(sgs);
}
