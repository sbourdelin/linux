/*
 * Copyright (C) 2016 - Antoine Tenart <antoine.tenart@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/firmware.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/overlay-manager.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

struct overlay_mgr_overlay {
	struct list_head list;
	char *name;
};

LIST_HEAD(overlay_mgr_overlays);
LIST_HEAD(overlay_mgr_formats);
DEFINE_SPINLOCK(overlay_mgr_lock);
DEFINE_SPINLOCK(overlay_mgr_format_lock);

/*
 * overlay_mgr_register_format()
 *
 * Adds a new format candidate to the list of supported formats. The registered
 * formats are used to parse the headers stored on the dips.
 */
int overlay_mgr_register_format(struct overlay_mgr_format *candidate)
{
	struct overlay_mgr_format *format;
	int err = 0;

	spin_lock(&overlay_mgr_format_lock);

	/* Check if the format is already registered */
	list_for_each_entry(format, &overlay_mgr_formats, list) {
		if (!strcpy(format->name, candidate->name)) {
			err = -EEXIST;
			goto err;
		}
	}

	list_add_tail(&candidate->list, &overlay_mgr_formats);

err:
	spin_unlock(&overlay_mgr_format_lock);
	return err;
}
EXPORT_SYMBOL_GPL(overlay_mgr_register_format);

/*
 * overlay_mgr_parse()
 *
 * Parse raw data with registered format parsers. Fills the candidate string if
 * one parser understood the raw data format.
 */
int overlay_mgr_parse(struct device *dev, void *data, char ***candidates,
		      unsigned *n)
{
	struct list_head *pos, *tmp;
	struct overlay_mgr_format *format;

	list_for_each_safe(pos, tmp, &overlay_mgr_formats) {
		format = list_entry(pos, struct overlay_mgr_format, list);

		format->parse(dev, data, candidates, n);
		if (n > 0)
			return 0;
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(overlay_mgr_parse);

static int overlay_mgr_check_overlay(struct device_node *node)
{
	struct property *p;
	const char *str = NULL;

	p = of_find_property(node, "compatible", NULL);
	if (!p)
		return -EINVAL;

	do {
		str = of_prop_next_string(p, str);
		if (of_machine_is_compatible(str))
			return 0;
	} while (str);

	return -EINVAL;
}

/*
 * _overlay_mgr_insert()
 *
 * Try to request and apply an overlay given a candidate name.
 */
static int _overlay_mgr_apply(struct device *dev, char *candidate)
{
	struct overlay_mgr_overlay *overlay;
	struct device_node *node;
	const struct firmware *firmware;
	char *firmware_name;
	int err = 0;

	spin_lock(&overlay_mgr_lock);

	list_for_each_entry(overlay, &overlay_mgr_overlays, list) {
		if (!strcmp(overlay->name, candidate)) {
			dev_err(dev, "overlay already loaded\n");
			err = -EEXIST;
			goto err_lock;
		}
	}

	overlay = devm_kzalloc(dev, sizeof(*overlay), GFP_KERNEL);
	if (!overlay) {
		err = -ENOMEM;
		goto err_lock;
	}

	overlay->name = candidate;

	firmware_name = kasprintf(GFP_KERNEL, "overlay-%s.dtbo", candidate);
	if (!firmware_name) {
		err = -ENOMEM;
		goto err_free;
	}

	dev_info(dev, "requesting firmware '%s'\n", firmware_name);

	err = request_firmware_direct(&firmware, firmware_name, dev);
	if (err) {
		dev_info(dev, "failed to request firmware '%s'\n",
			 firmware_name);
		goto err_free;
	}

	of_fdt_unflatten_tree((unsigned long *)firmware->data, NULL, &node);
	if (!node) {
		dev_err(dev, "failed to unflatted tree\n");
		err = -EINVAL;
		goto err_fw;
	}

	of_node_set_flag(node, OF_DETACHED);

	err = of_resolve_phandles(node);
	if (err) {
		dev_err(dev, "failed to resolve phandles: %d\n", err);
		goto err_fw;
	}

	err = overlay_mgr_check_overlay(node);
	if (err) {
		dev_err(dev, "overlay checks failed: %d\n", err);
		goto err_fw;
	}

	err = of_overlay_create(node);
	if (err < 0) {
		dev_err(dev, "failed to create overlay: %d\n", err);
		goto err_fw;
	}

	list_add_tail(&overlay->list, &overlay_mgr_overlays);

	dev_info(dev, "loaded firmware '%s'\n", firmware_name);

	spin_unlock(&overlay_mgr_lock);
	return 0;

err_fw:
	release_firmware(firmware);
err_free:
	devm_kfree(dev, overlay);
err_lock:
	spin_unlock(&overlay_mgr_lock);
	return err;
}

int overlay_mgr_apply(struct device *dev, char **candidates, unsigned n)
{
	int i, ret;

	for (i=0; i < n; i++) {
		ret = _overlay_mgr_apply(dev, candidates[i]);
		if (!ret)
			return 0;
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(overlay_mgr_apply);
