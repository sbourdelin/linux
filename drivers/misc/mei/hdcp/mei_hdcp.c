/* SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause */
/*
 * Copyright © 2017-2018 Intel Corporation
 *
 * Mei_hdcp.c: HDCP client driver for mei bus
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *	Ramalingam C <ramalingam.c@intel.com>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uuid.h>
#include <linux/mei_cl_bus.h>
#include <linux/component.h>
#include <drm/i915_component.h>

bool mei_hdcp_component_registered;
static struct mei_cl_device *mei_cldev;

struct i915_hdcp_component_ops mei_hdcp_component_ops = {
	.owner					= THIS_MODULE,
	.initiate_hdcp2_session			= NULL,
	.verify_receiver_cert_prepare_km	= NULL,
	.verify_hprime				= NULL,
	.store_pairing_info			= NULL,
	.initiate_locality_check		= NULL,
	.verify_lprime				= NULL,
	.get_session_key			= NULL,
	.repeater_check_flow_prepare_ack	= NULL,
	.verify_mprime				= NULL,
	.enable_hdcp_authentication		= NULL,
	.close_hdcp_session			= NULL,
};

static int mei_hdcp_component_bind(struct device *mei_kdev,
				   struct device *i915_kdev, void *data)
{
	struct i915_hdcp_component *comp = data;

	WARN_ON(!mutex_is_locked(&comp->mutex));
	if (WARN_ON(comp->ops || comp->dev))
		return -EEXIST;

	dev_info(mei_kdev, "MEI HDCP comp bind\n");
	comp->ops = &mei_hdcp_component_ops;
	comp->i915_kdev = i915_kdev;
	comp->mei_cldev = mei_cldev;
	mei_cldev_set_drvdata(mei_cldev, (void *)comp);

	return 0;
}

static void mei_hdcp_component_unbind(struct device *mei_kdev,
				      struct device *i915_kdev, void *data)
{
	struct i915_hdcp_component *comp = data;

	WARN_ON(!mutex_is_locked(&comp->mutex));
	dev_info(mei_kdev, "MEI HDCP comp unbind\n");
	comp->ops = NULL;
	comp->dev = NULL;
	comp->mei_cldev = NULL;
}

static const struct component_ops mei_hdcp_component_bind_ops = {
	.bind	= mei_hdcp_component_bind,
	.unbind	= mei_hdcp_component_unbind,
};

void mei_hdcp_component_init(struct device *dev)
{
	int ret;

	ret = component_add(dev, &mei_hdcp_component_bind_ops);
	if (ret < 0) {
		dev_err(dev, "Failed to add MEI HDCP comp (%d)\n", ret);
		return;
	}

	mei_hdcp_component_registered = true;
}

void mei_hdcp_component_cleanup(struct device *dev)
{
	if (!mei_hdcp_component_registered)
		return;

	component_del(dev, &mei_hdcp_component_bind_ops);
	mei_hdcp_component_registered = false;
}

static int mei_hdcp_probe(struct mei_cl_device *cldev,
			  const struct mei_cl_device_id *id)
{
	int ret;

	ret = mei_cldev_enable(cldev);
	if (ret < 0) {
		dev_err(&cldev->dev, "mei_cldev_enable Failed. %d\n", ret);
		return ret;
	}

	mei_cldev = cldev;
	mei_hdcp_component_init(&cldev->dev);
	return 0;
}

static int mei_hdcp_remove(struct mei_cl_device *cldev)
{
	struct i915_hdcp_component *comp;

	comp = mei_cldev_get_drvdata(cldev);
	if (comp && comp->master_ops && comp->master_ops->pull_down_interface)
		comp->master_ops->pull_down_interface(comp->i915_kdev);

	mei_cldev = NULL;
	mei_cldev_set_drvdata(cldev, NULL);
	mei_hdcp_component_cleanup(&cldev->dev);

	return mei_cldev_disable(cldev);
}

#define MEI_UUID_HDCP		UUID_LE(0xB638AB7E, 0x94E2, 0x4EA2, 0xA5, \
					0x52, 0xD1, 0xC5, 0x4B, \
					0x62, 0x7F, 0x04)

static struct mei_cl_device_id mei_hdcp_tbl[] = {
	{ .uuid = MEI_UUID_HDCP, .version = MEI_CL_VERSION_ANY },
	{ }
};
MODULE_DEVICE_TABLE(mei, mei_hdcp_tbl);

static struct mei_cl_driver mei_hdcp_driver = {
	.id_table	= mei_hdcp_tbl,
	.name		= KBUILD_MODNAME,
	.probe		= mei_hdcp_probe,
	.remove		= mei_hdcp_remove,
};

module_mei_cl_driver(mei_hdcp_driver);

MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("MEI HDCP");
