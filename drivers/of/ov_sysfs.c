/*
 * Sysfs ABI for device tree overlays
 *
 * Copyright (C) 2016  Heinrich Schuchardt <xypron.glpk@gmx.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2 as published by the Free Software Foundation.
 */

#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/libfdt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>

static int of_create_overlay_from_file(const char *path)
{
	struct file *filp = NULL;
	mm_segment_t fs;
	int ret = 0;
	loff_t size;
	char *buffer = NULL;
	ssize_t bytes_read;
	loff_t offset = 0;
	struct device_node *overlay = NULL;

	fs = get_fs();
	set_fs(get_ds());
	filp = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
	if (IS_ERR(filp)) {
		ret = PTR_ERR(filp);
		goto err_file_open;
	}

	if (!S_ISREG(filp->f_inode->i_mode)) {
		ret = -EISDIR;
		goto err_file_read;
	}
	size = i_size_read(filp->f_inode);
	buffer = vmalloc(size);
	if (buffer == NULL) {
		ret = -ENOMEM;
		goto err_malloc;
	}
	for (; size > 0; ) {
		bytes_read = vfs_read(filp, buffer, size, &offset);
		if (bytes_read == 0)
			break;
		if (bytes_read < 0) {
			ret = bytes_read;
			goto err_file_read;
		}
		size -= bytes_read;
	}
	if (offset < sizeof(struct fdt_header) ||
	    offset < fdt_totalsize(buffer)) {
		pr_err("OF: Size of %s does not match header information\n",
		       path);
		ret = -EINVAL;
		goto err_file_read;
	}
	overlay = of_fdt_unflatten_tree((unsigned long *) buffer, NULL, NULL);
	if (overlay == NULL) {
		pr_err("OF: Cannot unflatten %s\n", path);
		ret = -EINVAL;
		goto err_file_read;
	}
	of_node_set_flag(overlay, OF_DETACHED);
	ret = of_resolve_phandles(overlay);
	if (ret < 0) {
		pr_err("OF: Failed to resolve phandles for %s\n", path);
		goto err_overlay;
	}
	ret = of_overlay_create(overlay);
	if (ret < 0) {
		pr_err("OF: Cannot create overlay from %s\n", path);
	} else {
		pr_info("OF: Overlay %d created from %s\n", ret, path);
		ret = 0;
	}
err_overlay:
	of_node_put(overlay);
err_file_read:
	vfree(buffer);
err_malloc:
	fput(filp);
err_file_open:
	set_fs(fs);
	return ret;
}

static ssize_t attribute_read(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      char *buf)
{
	int ret;

	if (strcmp(attr->attr.name, "loaded") == 0)
		ret = sprintf(buf, "%d\n", of_overlay_count());
	else
		ret = -ENOENT;

	return ret;
}

static ssize_t attribute_write(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t size)
{
	char *parameter;
	int ret;
	long count;

	if (size > PATH_MAX)
		return -ENAMETOOLONG;

	/* The parameter has to be terminated either by LF or \0. */

	switch (buf[size - 1]) {
	case 0x00:
	case 0x0a:
		break;
	default:
		return -ENOENT;
	}
	parameter = vmalloc(size);
	if (!parameter)
		return -ENOMEM;
	memcpy(parameter, buf, size);
	parameter[size - 1] = 0x00;

	if (strcmp(attr->attr.name, "load") == 0) {
		ret = of_create_overlay_from_file(parameter);
		if (!ret)
			ret = size;
	} else if (strcmp(attr->attr.name, "unload") == 0) {
		ret = kstrtol(parameter, 0, &count);
		if (ret)
			goto out;
		if (count < 0)
			ret = of_overlay_destroy_all();
		else
			for (; count > 0; --count) {
				ret = of_overlay_destroy_last();
				if (ret)
					goto out;
			}
		ret = size;
	} else
		ret = -ENOENT;
out:
	vfree(parameter);

	return ret;
}

static struct kobject *kobj;

static struct kobj_attribute load_attribute =
	__ATTR(load, 0200, NULL, attribute_write);
static struct kobj_attribute loaded_attribute =
	__ATTR(loaded, 0444, attribute_read, NULL);
static struct kobj_attribute unload_attribute =
	__ATTR(unload, 0200, NULL, attribute_write);
static struct attribute *attrs[] = {
	&load_attribute.attr,
	&loaded_attribute.attr,
	&unload_attribute.attr,
	NULL
};
static struct attribute_group attr_group = {
	.attrs = attrs,
};

static int __init ov_sysfs_init(void)
{
	int ret;

	kobj = kobject_create_and_add("devicetree-overlays", firmware_kobj);
	if (kobj == 0)
		return -ENOMEM;
	ret = sysfs_create_group(kobj, &attr_group);
	if (ret) {
		kobject_put(kobj);
		return ret;
	}

	/*
	 * It is not possible to ensure that no sysfs io is started while
	 * module_exit is called. So disable unloading.
	 */
	__module_get(THIS_MODULE);

	return 0;
}

static void __exit ov_sysfs_exit(void)
{
	kobject_put(kobj);
}

module_init(ov_sysfs_init);
module_exit(ov_sysfs_exit);

MODULE_AUTHOR("Heinrich Schuchardt <xypron.glpk@gmx.de>");
MODULE_DESCRIPTION("Sysfs ABI for device tree overlays");
MODULE_LICENSE("GPL");
