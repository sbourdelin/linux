#include <linux/backlight.h>
#include <linux/dma-buf.h>
#include <linux/pm.h>
#include <linux/swab.h>

#include <drm/tinydrm/tinydrm.h>
#include <drm/tinydrm/tinydrm-backlight.h>

/**
 * tinydrm_of_find_backlight - Find backlight device in device-tree
 * @dev: Device
 *
 * This function looks for a DT node pointed to by a property named 'backlight'
 * and uses of_find_backlight_by_node() to get the backlight device.
 * Additionally if the brightness property is zero, it is set to
 * max_brightness.
 *
 * Returns:
 * NULL if there's no backlight property.
 * Error pointer -EPROBE_DEFER if the DT node is found, but no backlight device
 * is found.
 * If the backlight device is found, a pointer to the structure is returned.
 */

struct backlight_device *tinydrm_of_find_backlight(struct device *dev)
{
	struct backlight_device *backlight;
	struct device_node *np;

	np = of_parse_phandle(dev->of_node, "backlight", 0);
	if (!np)
		return NULL;

	backlight = of_find_backlight_by_node(np);
	of_node_put(np);

	if (!backlight)
		return ERR_PTR(-EPROBE_DEFER);

	if (!backlight->props.brightness) {
		backlight->props.brightness = backlight->props.max_brightness;
		DRM_DEBUG_KMS("Backlight brightness set to %d\n",
			      backlight->props.brightness);
	}

	return backlight;
}
EXPORT_SYMBOL(tinydrm_of_find_backlight);

/**
 * tinydrm_enable_backlight - Enable backlight helper
 * @backlight: Backlight device
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_enable_backlight(struct backlight_device *backlight)
{
	unsigned int old_state;
	int ret;

	if (!backlight)
		return 0;

	old_state = backlight->props.state;
	backlight->props.state &= ~BL_CORE_FBBLANK;
	DRM_DEBUG_KMS("Backlight state: 0x%x -> 0x%x\n", old_state,
		      backlight->props.state);

	ret = backlight_update_status(backlight);
	if (ret)
		DRM_ERROR("Failed to enable backlight %d\n", ret);

	return ret;
}
EXPORT_SYMBOL(tinydrm_enable_backlight);

/**
 * tinydrm_disable_backlight - Disable backlight helper
 * @backlight: Backlight device
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_disable_backlight(struct backlight_device *backlight)
{
	unsigned int old_state;
	int ret;

	if (!backlight)
		return 0;

	old_state = backlight->props.state;
	backlight->props.state |= BL_CORE_FBBLANK;
	DRM_DEBUG_KMS("Backlight state: 0x%x -> 0x%x\n", old_state,
		      backlight->props.state);
	ret = backlight_update_status(backlight);
	if (ret)
		DRM_ERROR("Failed to disable backlight %d\n", ret);

	return ret;
}
EXPORT_SYMBOL(tinydrm_disable_backlight);
