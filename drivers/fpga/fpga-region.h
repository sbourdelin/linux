#include <linux/device.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/fpga/fpga-bridge.h>

#ifndef _FPGA_REGION_H
#define _FPGA_REGION_H

/**
 * struct fpga_region - FPGA Region structure
 * @dev: FPGA Region device
 * @mutex: enforces exclusive reference to region
 * @bridge_list: list of FPGA bridges specified in region
 * @overlays: list of struct region_overlay_info
 * @mgr_dev: device of fpga manager
 * @priv: private data
 */
struct fpga_region {
	struct device dev;
	struct mutex mutex; /* for exclusive reference to region */
	struct list_head bridge_list;
	struct fpga_manager *mgr;
	struct fpga_image_info *image_info;
	void *priv;
	int (*get_bridges)(struct fpga_region *region,
			   struct fpga_image_info *image_info);
#if IS_ENABLED(CONFIG_OF_FPGA_REGION)
	struct list_head overlays;
#endif
};

#if IS_ENABLED(CONFIG_OF_FPGA_REGION)
/**
 * struct region_overlay: info regarding overlays applied to the region
 * @node: list node
 * @overlay: pointer to overlay
 * @image_info: fpga image specific information parsed from overlay.  Is NULL if
 *        overlay doesn't program FPGA.
 */
struct region_overlay {
	struct list_head node;
	struct device_node *overlay;
	struct fpga_image_info *image_info;
};
#endif /* CONFIG_OF_FPGA_REGION */

#define to_fpga_region(d) container_of(d, struct fpga_region, dev)

#ifdef CONFIG_OF
struct fpga_region *of_fpga_region_find(struct device_node *np);
#else
struct fpga_region *of_fpga_region_find(struct device_node *np)
{
	return NULL;
}
#endif /* CONFIG_OF */

struct fpga_image_info *fpga_region_alloc_image_info(
				struct fpga_region *region);
void fpga_region_free_image_info(struct fpga_region *region,
				 struct fpga_image_info *image_info);

int fpga_region_program_fpga(struct fpga_region *region,
			     struct fpga_image_info *image_info);

int fpga_region_register(struct device *dev, struct fpga_region *region);
int fpga_region_unregister(struct fpga_region *region);

#endif /* _FPGA_REGION_H */
