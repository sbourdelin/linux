/*
 * FPGA Region - Device Tree support for FPGA programming under Linux
 *
 *  Copyright (C) 2013-2016 Altera Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/fpga/fpga-bridge.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include "fpga-region.h"

static DEFINE_IDA(fpga_region_ida);
struct class *fpga_region_class;

/* todo: prevent programming parent region */

struct fpga_image_info *fpga_region_alloc_image_info(struct fpga_region *region)
{
	struct device *dev = &region->dev;
	struct fpga_image_info *image_info;

	image_info = devm_kzalloc(dev, sizeof(*image_info), GFP_KERNEL);
	if (!image_info)
		return ERR_PTR(-ENOMEM);

	return image_info;
}
EXPORT_SYMBOL_GPL(fpga_region_alloc_image_info);

void fpga_region_free_image_info(struct fpga_region *region,
				 struct fpga_image_info *image_info)
{
	struct device *dev = &region->dev;

	if (!image_info)
		return;

	if (image_info->firmware_name)
		devm_kfree(dev, image_info->firmware_name);

	devm_kfree(dev, image_info);
}
EXPORT_SYMBOL_GPL(fpga_region_free_image_info);

#if IS_ENABLED(CONFIG_OF_FPGA_REGION)
static int fpga_region_of_node_match(struct device *dev, const void *data)
{
	return dev->of_node == data;
}

/**
 * of_fpga_region_find - find FPGA region
 * @np: device node of FPGA Region
 *
 * Caller will need to put_device(&region->dev) when done.
 *
 * Returns FPGA Region struct or NULL
 */
struct fpga_region *of_fpga_region_find(struct device_node *np)
{
	struct device *dev;

	dev = class_find_device(fpga_region_class, NULL, np,
				fpga_region_of_node_match);
	if (!dev)
		return NULL;

	return to_fpga_region(dev);
}
EXPORT_SYMBOL_GPL(of_fpga_region_find);

/*
 * If a region has overlays, only the first overlay can program the FPGA
 * so only the first overlay will have image info.
 */
struct fpga_image_info *fpga_region_ovl_image_info(struct fpga_region *region)
{
	struct region_overlay *reg_ovl;

	if (list_empty(&region->overlays))
		return NULL;

	reg_ovl = list_first_entry(&region->overlays, typeof(*reg_ovl), node);

	return reg_ovl->image_info;
}
EXPORT_SYMBOL_GPL(fpga_region_ovl_image_info);

#endif /* CONFIG_OF_FPGA_REGION */

/**
 * fpga_region_get - get an exclusive reference to a fpga region
 * @region: FPGA Region struct
 *
 * Caller should call fpga_region_put() when done with region.
 *
 * Return fpga_region struct if successful.
 * Return -EBUSY if someone already has a reference to the region.
 * Return -ENODEV if @np is not a FPGA Region.
 */
static struct fpga_region *fpga_region_get(struct fpga_region *region)
{
	struct device *dev = &region->dev;

	if (!mutex_trylock(&region->mutex)) {
		dev_dbg(dev, "%s: FPGA Region already in use\n", __func__);
		return ERR_PTR(-EBUSY);
	}

	get_device(dev);
	if (!try_module_get(dev->parent->driver->owner)) {
		put_device(dev);
		mutex_unlock(&region->mutex);
		return ERR_PTR(-ENODEV);
	}

	dev_dbg(&region->dev, "get\n");

	return region;
}

/**
 * fpga_region_put - release a reference to a region
 *
 * @region: FPGA region
 */
static void fpga_region_put(struct fpga_region *region)
{
	struct device *dev = &region->dev;

	dev_dbg(&region->dev, "put\n");

	module_put(dev->parent->driver->owner);
	put_device(dev);
	mutex_unlock(&region->mutex);
}

/**
 * fpga_region_program_fpga - program FPGA
 * @region: FPGA region that is receiving an overlay
 * @reg_ovl: region overlay with fpga_image_info parsed from overlay
 *
 * Program an FPGA using information in a device tree overlay.
 *
 * Return: 0 for success or negative error code.
 */
int fpga_region_program_fpga(struct fpga_region *region,
			     struct fpga_image_info *image_info)
{
	int ret;

	region = fpga_region_get(region);
	if (IS_ERR(region)) {
		pr_err("failed to get fpga region\n");
		return PTR_ERR(region);
	}

	ret = fpga_mgr_lock(region->mgr);
	if (ret < 0) {
		pr_err("fpga manager is busy\n");
		goto err_put_region;
	}

	/*
	 * In some cases, we already have a list of bridges in the
	 * fpga region struct.  Or we don't have any bridges.
	 */
	if (region->get_bridges) {
		ret = region->get_bridges(region, image_info);
		if (ret) {
			pr_err("failed to get fpga region bridges\n");
			goto err_unlock_mgr;
		}
	}

	ret = fpga_bridges_disable(&region->bridge_list);
	if (ret) {
		pr_err("failed to disable region bridges\n");
		goto err_put_br;
	}

	ret = fpga_mgr_load(region->mgr, image_info);
	if (ret) {
		pr_err("failed to load fpga image\n");
		goto err_put_br;
	}

	ret = fpga_bridges_enable(&region->bridge_list);
	if (ret) {
		pr_err("failed to enable region bridges\n");
		goto err_put_br;
	}

	region->image_info = image_info;

	fpga_mgr_unlock(region->mgr);
	fpga_region_put(region);

	return 0;

err_put_br:
	if (region->get_bridges)
		fpga_bridges_put(&region->bridge_list);
err_unlock_mgr:
	fpga_mgr_unlock(region->mgr);
err_put_region:
	fpga_region_put(region);

	return ret;
}
EXPORT_SYMBOL_GPL(fpga_region_program_fpga);

int fpga_region_register(struct device *dev, struct fpga_region *region)
{
	int id, ret = 0;

	id = ida_simple_get(&fpga_region_ida, 0, 0, GFP_KERNEL);
	if (id < 0)
		return id;

	mutex_init(&region->mutex);
	INIT_LIST_HEAD(&region->bridge_list);
	device_initialize(&region->dev);
	region->dev.class = fpga_region_class;
	region->dev.parent = dev;
	region->dev.of_node = dev->of_node;
	region->dev.id = id;
	dev_set_drvdata(dev, region);

	ret = dev_set_name(&region->dev, "region%d", id);
	if (ret)
		goto err_remove;

	ret = device_add(&region->dev);
	if (ret)
		goto err_remove;

	dev_info(dev, "FPGA Region probed\n");

	return 0;

err_remove:
	ida_simple_remove(&fpga_region_ida, id);

	return ret;
}
EXPORT_SYMBOL_GPL(fpga_region_register);

int fpga_region_unregister(struct fpga_region *region)
{
	device_unregister(&region->dev);

	return 0;
}
EXPORT_SYMBOL_GPL(fpga_region_unregister);

static void fpga_region_dev_release(struct device *dev)
{
	struct fpga_region *region = to_fpga_region(dev);

	ida_simple_remove(&fpga_region_ida, region->dev.id);
}

/**
 * fpga_region_init - init function for fpga_region class
 * Creates the fpga_region class.
 */
static int __init fpga_region_init(void)
{
	fpga_region_class = class_create(THIS_MODULE, "fpga_region");
	if (IS_ERR(fpga_region_class))
		return PTR_ERR(fpga_region_class);

	fpga_region_class->dev_release = fpga_region_dev_release;

	return 0;
}

static void __exit fpga_region_exit(void)
{
	class_destroy(fpga_region_class);
	ida_destroy(&fpga_region_ida);
}

subsys_initcall(fpga_region_init);
module_exit(fpga_region_exit);

MODULE_DESCRIPTION("FPGA Region");
MODULE_AUTHOR("Alan Tull <atull@opensource.altera.com>");
MODULE_LICENSE("GPL v2");
