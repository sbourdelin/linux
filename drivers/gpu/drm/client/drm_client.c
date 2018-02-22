// SPDX-License-Identifier: GPL-2.0
// Copyright 2018 Noralf Trønnes

#include <linux/dma-buf.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <drm/drm_client.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drmP.h>

#include "drm_crtc_internal.h"
#include "drm_internal.h"

struct drm_client_funcs_entry {
	struct list_head list;
	const struct drm_client_funcs *funcs;
};

static LIST_HEAD(drm_client_list);
static LIST_HEAD(drm_client_funcs_list);
static DEFINE_MUTEX(drm_client_list_lock);

static void drm_client_new(struct drm_device *dev,
			   const struct drm_client_funcs *funcs)
{
	struct drm_client_dev *client;
	int ret;

	lockdep_assert_held(&drm_client_list_lock);

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return;

	mutex_init(&client->lock);
	client->dev = dev;
	client->funcs = funcs;

	ret = funcs->new(client);
	DRM_DEV_DEBUG_KMS(dev->dev, "%s: ret=%d\n", funcs->name, ret);
	if (ret) {
		drm_client_free(client);
		return;
	}

	list_add(&client->list, &drm_client_list);
}

/**
 * drm_client_free - Free DRM client resources
 * @client: DRM client
 *
 * This is called automatically on client removal unless the client returns
 * non-zero in the &drm_client_funcs->remove callback. The fbdev client does
 * this when it can't close &drm_file because userspace has an open fd.
 */
void drm_client_free(struct drm_client_dev *client)
{
	DRM_DEV_DEBUG_KMS(client->dev->dev, "%s\n", client->funcs->name);
	if (WARN_ON(client->file)) {
		client->file_ref_count = 1;
		drm_client_put_file(client);
	}
	mutex_destroy(&client->lock);
	kfree(client->crtcs);
	kfree(client);
}
EXPORT_SYMBOL(drm_client_free);

static void drm_client_remove(struct drm_client_dev *client)
{
	lockdep_assert_held(&drm_client_list_lock);

	list_del(&client->list);

	if (!client->funcs->remove || !client->funcs->remove(client))
		drm_client_free(client);
}

/**
 * drm_client_register - Register a DRM client
 * @funcs: Client callbacks
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int drm_client_register(const struct drm_client_funcs *funcs)
{
	struct drm_client_funcs_entry *funcs_entry;
	struct drm_device_list_iter iter;
	struct drm_device *dev;

	funcs_entry = kzalloc(sizeof(*funcs_entry), GFP_KERNEL);
	if (!funcs_entry)
		return -ENOMEM;

	funcs_entry->funcs = funcs;

	mutex_lock(&drm_global_mutex);
	mutex_lock(&drm_client_list_lock);

	drm_device_list_iter_begin(&iter);
	drm_for_each_device_iter(dev, &iter)
		if (drm_core_check_feature(dev, DRIVER_MODESET))
			drm_client_new(dev, funcs);
	drm_device_list_iter_end(&iter);

	list_add(&funcs_entry->list, &drm_client_funcs_list);

	mutex_unlock(&drm_client_list_lock);
	mutex_unlock(&drm_global_mutex);

	DRM_DEBUG_KMS("%s\n", funcs->name);

	return 0;
}
EXPORT_SYMBOL(drm_client_register);

/**
 * drm_client_unregister - Unregister a DRM client
 * @funcs: Client callbacks
 */
void drm_client_unregister(const struct drm_client_funcs *funcs)
{
	struct drm_client_funcs_entry *funcs_entry;
	struct drm_client_dev *client, *tmp;

	mutex_lock(&drm_client_list_lock);

	list_for_each_entry_safe(client, tmp, &drm_client_list, list) {
		if (client->funcs == funcs)
			drm_client_remove(client);
	}

	list_for_each_entry(funcs_entry, &drm_client_funcs_list, list) {
		if (funcs_entry->funcs == funcs) {
			list_del(&funcs_entry->list);
			kfree(funcs_entry);
			break;
		}
	}

	mutex_unlock(&drm_client_list_lock);

	DRM_DEBUG_KMS("%s\n", funcs->name);
}
EXPORT_SYMBOL(drm_client_unregister);

void drm_client_dev_register(struct drm_device *dev)
{
	struct drm_client_funcs_entry *funcs_entry;

	/*
	 * Minors are created at the beginning of drm_dev_register(), but can
	 * be removed again if the function fails. Since we iterate DRM devices
	 * by walking DRM minors, we need to stay under this lock.
	 */
	lockdep_assert_held(&drm_global_mutex);

	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		return;

	mutex_lock(&drm_client_list_lock);
	list_for_each_entry(funcs_entry, &drm_client_funcs_list, list)
		drm_client_new(dev, funcs_entry->funcs);
	mutex_unlock(&drm_client_list_lock);
}

void drm_client_dev_unregister(struct drm_device *dev)
{
	struct drm_client_dev *client, *tmp;

	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		return;

	mutex_lock(&drm_client_list_lock);
	list_for_each_entry_safe(client, tmp, &drm_client_list, list)
		if (client->dev == dev)
			drm_client_remove(client);
	mutex_unlock(&drm_client_list_lock);
}

void drm_client_dev_hotplug(struct drm_device *dev)
{
	struct drm_client_dev *client;
	int ret;

	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		return;

	mutex_lock(&drm_client_list_lock);
	list_for_each_entry(client, &drm_client_list, list)
		if (client->dev == dev && client->funcs->hotplug) {
			ret = client->funcs->hotplug(client);
			DRM_DEV_DEBUG_KMS(dev->dev, "%s: ret=%d\n",
					  client->funcs->name, ret);
		}
	mutex_unlock(&drm_client_list_lock);
}

void drm_client_dev_lastclose(struct drm_device *dev)
{
	struct drm_client_dev *client;
	int ret;

	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		return;

	mutex_lock(&drm_client_list_lock);
	list_for_each_entry(client, &drm_client_list, list)
		if (client->dev == dev && client->funcs->lastclose) {
			ret = client->funcs->lastclose(client);
			DRM_DEV_DEBUG_KMS(dev->dev, "%s: ret=%d\n",
					  client->funcs->name, ret);
		}
	mutex_unlock(&drm_client_list_lock);
}

/* Get static info */
static int drm_client_init(struct drm_client_dev *client, struct drm_file *file)
{
	struct drm_mode_card_res card_res = {};
	struct drm_device *dev = client->dev;
	u32 *crtcs;
	int ret;

	ret = drm_mode_getresources(dev, &card_res, file, false);
	if (ret)
		return ret;
	if (!card_res.count_crtcs)
		return -ENOENT;

	crtcs = kmalloc_array(card_res.count_crtcs, sizeof(*crtcs), GFP_KERNEL);
	if (!crtcs)
		return -ENOMEM;

	card_res.count_fbs = 0;
	card_res.count_connectors = 0;
	card_res.count_encoders = 0;
	card_res.crtc_id_ptr = (unsigned long)crtcs;

	ret = drm_mode_getresources(dev, &card_res, file, false);
	if (ret) {
		kfree(crtcs);
		return ret;
	}

	client->crtcs = crtcs;
	client->num_crtcs = card_res.count_crtcs;
	client->min_width = card_res.min_width;
	client->max_width = card_res.max_width;
	client->min_height = card_res.min_height;
	client->max_height = card_res.max_height;

	return 0;
}

/**
 * drm_client_get_file - Get a DRM file
 * @client: DRM client
 *
 * This function makes sure the client has a &drm_file available. The client
 * doesn't normally need to call this, since all client functions that depends
 * on a DRM file will call it. A matching call to drm_client_put_file() is
 * necessary.
 *
 * The reason for not opening a DRM file when a @client is created is because
 * we have to take a ref on the driver module due to &drm_driver->postclose
 * being called in drm_file_free(). Having a DRM file open for the lifetime of
 * the client instance would block driver module unload.
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int drm_client_get_file(struct drm_client_dev *client)
{
	struct drm_device *dev = client->dev;
	struct drm_file *file;
	int ret = 0;

	mutex_lock(&client->lock);

	if (client->file_ref_count++) {
		mutex_unlock(&client->lock);
		return 0;
	}

	if (!try_module_get(dev->driver->fops->owner)) {
		ret = -ENODEV;
		goto err_unlock;
	}

	drm_dev_get(dev);

	file = drm_file_alloc(dev->primary);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto err_put;
	}

	if (!client->crtcs) {
		ret = drm_client_init(client, file);
		if (ret)
			goto err_free;
	}

	mutex_lock(&dev->filelist_mutex);
	list_add(&file->lhead, &dev->filelist_internal);
	mutex_unlock(&dev->filelist_mutex);

	client->file = file;

	mutex_unlock(&client->lock);

	return 0;

err_free:
	drm_file_free(file);
err_put:
	drm_dev_put(dev);
	module_put(dev->driver->fops->owner);
err_unlock:
	client->file_ref_count = 0;
	mutex_unlock(&client->lock);

	return ret;
}
EXPORT_SYMBOL(drm_client_get_file);

void drm_client_put_file(struct drm_client_dev *client)
{
	struct drm_device *dev = client->dev;

	if (!client)
		return;

	mutex_lock(&client->lock);

	if (WARN_ON(!client->file_ref_count))
		goto out_unlock;

	if (--client->file_ref_count)
		goto out_unlock;

	mutex_lock(&dev->filelist_mutex);
	list_del(&client->file->lhead);
	mutex_unlock(&dev->filelist_mutex);

	drm_file_free(client->file);
	client->file = NULL;
	drm_dev_put(dev);
	module_put(dev->driver->fops->owner);
out_unlock:
	mutex_unlock(&client->lock);
}
EXPORT_SYMBOL(drm_client_put_file);

static struct drm_pending_event *
drm_client_read_get_pending_event(struct drm_device *dev, struct drm_file *file)
{
	struct drm_pending_event *e = NULL;
	int ret;

	ret = mutex_lock_interruptible(&file->event_read_lock);
	if (ret)
		return ERR_PTR(ret);

	spin_lock_irq(&dev->event_lock);
	if (!list_empty(&file->event_list)) {
		e = list_first_entry(&file->event_list,
				     struct drm_pending_event, link);
		file->event_space += e->event->length;
		list_del(&e->link);
	}
	spin_unlock_irq(&dev->event_lock);

	mutex_unlock(&file->event_read_lock);

	return e;
}

struct drm_event *
drm_client_read_event(struct drm_client_dev *client, bool block)
{
	struct drm_file *file = client->file;
	struct drm_device *dev = client->dev;
	struct drm_pending_event *e;
	struct drm_event *event;
	int ret;

	/* Allocate so it fits all events, there's a sanity check later */
	event = kzalloc(128, GFP_KERNEL);
	if (!event)
		return ERR_PTR(-ENOMEM);

	e = drm_client_read_get_pending_event(dev, file);
	if (IS_ERR(e)) {
		ret = PTR_ERR(e);
		goto err_free;
	}

	if (!e && !block) {
		ret = 0;
		goto err_free;
	}

	ret = wait_event_interruptible_timeout(file->event_wait,
					       !list_empty(&file->event_list),
					       5 * HZ);
	if (!ret)
		ret = -ETIMEDOUT;
	if (ret < 0)
		goto err_free;

	e = drm_client_read_get_pending_event(dev, file);
	if (IS_ERR_OR_NULL(e)) {
		ret = PTR_ERR_OR_ZERO(e);
		goto err_free;
	}

	if (WARN_ON(e->event->length > 128)) {
		/* Increase buffer if this happens */
		ret = -ENOMEM;
		goto err_free;
	}

	memcpy(event, e->event, e->event->length);
	kfree(e);

	return event;

err_free:
	kfree(event);

	return ret ? ERR_PTR(ret) : NULL;
}
EXPORT_SYMBOL(drm_client_read_event);

static void drm_client_connector_free(struct drm_client_connector *connector)
{
	if (!connector)
		return;
	kfree(connector->modes);
	kfree(connector);
}

static struct drm_client_connector *
drm_client_get_connector(struct drm_client_dev *client, unsigned int id)
{
	struct drm_mode_get_connector req = {
		.connector_id = id,
	};
	struct drm_client_connector *connector;
	struct drm_mode_modeinfo *modes = NULL;
	struct drm_device *dev = client->dev;
	struct drm_connector *conn;
	bool non_desktop;
	int ret;

	connector = kzalloc(sizeof(*connector), GFP_KERNEL);
	if (!connector)
		return ERR_PTR(-ENOMEM);

	ret = drm_mode_getconnector(dev, &req, client->file, false);
	if (ret)
		goto err_free;

	connector->conn_id = id;
	connector->status = req.connection;

	conn = drm_connector_lookup(dev, client->file, id);
	if (!conn) {
		ret = -ENOENT;
		goto err_free;
	}

	non_desktop = conn->display_info.non_desktop;

	connector->has_tile = conn->has_tile;
	connector->tile_h_loc = conn->tile_h_loc;
	connector->tile_v_loc = conn->tile_v_loc;
	if (conn->tile_group)
		connector->tile_group = conn->tile_group->id;

	drm_connector_put(conn);

	if (non_desktop) {
		kfree(connector);
		return NULL;
	}

	if (!req.count_modes)
		return connector;

	modes = kcalloc(req.count_modes, sizeof(*modes), GFP_KERNEL);
	if (!modes) {
		ret = -ENOMEM;
		goto err_free;
	}

	connector->modes = modes;
	connector->num_modes = req.count_modes;

	req.count_props = 0;
	req.count_encoders = 0;
	req.modes_ptr = (unsigned long)modes;

	ret = drm_mode_getconnector(dev, &req, client->file, false);
	if (ret)
		goto err_free;

	return connector;

err_free:
	kfree(modes);
	kfree(connector);

	return ERR_PTR(ret);
}

static int drm_client_get_connectors(struct drm_client_dev *client,
				     struct drm_client_connector ***connectors)
{
	struct drm_mode_card_res card_res = {};
	struct drm_device *dev = client->dev;
	int ret, num_connectors;
	u32 *connector_ids;
	unsigned int i;

	ret = drm_mode_getresources(dev, &card_res, client->file, false);
	if (ret)
		return ret;
	if (!card_res.count_connectors)
		return 0;

	num_connectors = card_res.count_connectors;
	connector_ids = kcalloc(num_connectors,
				sizeof(*connector_ids), GFP_KERNEL);
	if (!connector_ids)
		return -ENOMEM;

	card_res.count_fbs = 0;
	card_res.count_crtcs = 0;
	card_res.count_encoders = 0;
	card_res.connector_id_ptr = (unsigned long)connector_ids;

	ret = drm_mode_getresources(dev, &card_res, client->file, false);
	if (ret)
		goto err_free;

	*connectors = kcalloc(num_connectors, sizeof(**connectors), GFP_KERNEL);
	if (!(*connectors)) {
		ret = -ENOMEM;
		goto err_free;
	}

	for (i = 0; i < num_connectors; i++) {
		struct drm_client_connector *connector;

		connector = drm_client_get_connector(client, connector_ids[i]);
		if (IS_ERR(connector)) {
			ret = PTR_ERR(connector);
			goto err_free;
		}
		if (connector)
			(*connectors)[i] = connector;
		else
			num_connectors--;
	}

	if (!num_connectors) {
		ret = 0;
		goto err_free;
	}

	return num_connectors;

err_free:
	if (connectors)
		for (i = 0; i < num_connectors; i++)
			drm_client_connector_free((*connectors)[i]);

	kfree(connectors);
	kfree(connector_ids);

	return ret;
}

static bool
drm_client_connector_is_enabled(struct drm_client_connector *connector,
				bool strict)
{
	if (strict)
		return connector->status == connector_status_connected;
	else
		return connector->status != connector_status_disconnected;
}

struct drm_mode_modeinfo *
drm_client_display_first_mode(struct drm_client_display *display)
{
	if (!display->num_modes)
		return NULL;
	return display->modes;
}
EXPORT_SYMBOL(drm_client_display_first_mode);

struct drm_mode_modeinfo *
drm_client_display_next_mode(struct drm_client_display *display,
			     struct drm_mode_modeinfo *mode)
{
	struct drm_mode_modeinfo *modes = display->modes;

	if (++mode < &modes[display->num_modes])
		return mode;

	return NULL;
}
EXPORT_SYMBOL(drm_client_display_next_mode);

static void
drm_client_display_fill_tile_modes(struct drm_client_display *display,
				   struct drm_mode_modeinfo *tile_modes)
{
	unsigned int i, j, num_modes = display->connectors[0]->num_modes;
	struct drm_mode_modeinfo *tile_mode, *conn_mode;

	if (!num_modes) {
		kfree(tile_modes);
		kfree(display->modes);
		display->modes = NULL;
		display->num_modes = 0;
		return;
	}

	for (i = 0; i < num_modes; i++) {
		tile_mode = &tile_modes[i];

		conn_mode = &display->connectors[0]->modes[i];
		tile_mode->clock = conn_mode->clock;
		tile_mode->vscan = conn_mode->vscan;
		tile_mode->vrefresh = conn_mode->vrefresh;
		tile_mode->flags = conn_mode->flags;
		tile_mode->type = conn_mode->type;

		for (j = 0; j < display->num_connectors; j++) {
			conn_mode = &display->connectors[j]->modes[i];

			if (!display->connectors[j]->tile_h_loc) {
				tile_mode->hdisplay += conn_mode->hdisplay;
				tile_mode->hsync_start += conn_mode->hsync_start;
				tile_mode->hsync_end += conn_mode->hsync_end;
				tile_mode->htotal += conn_mode->htotal;
			}

			if (!display->connectors[j]->tile_v_loc) {
				tile_mode->vdisplay += conn_mode->vdisplay;
				tile_mode->vsync_start += conn_mode->vsync_start;
				tile_mode->vsync_end += conn_mode->vsync_end;
				tile_mode->vtotal += conn_mode->vtotal;
			}
		}
	}

	kfree(display->modes);
	display->modes = tile_modes;
	display->num_modes = num_modes;
}

/**
 * drm_client_display_update_modes - Fetch display modes
 * @display: Client display
 * @mode_changed: Optional pointer to boolen which return whether the modes
 *                have changed or not.
 *
 * This function can be used in the client hotplug callback to check if the
 * video modes have changed and get them up-to-date.
 *
 * Returns:
 * Number of modes on success, negative error code on failure.
 */
int drm_client_display_update_modes(struct drm_client_display *display,
				    bool *mode_changed)
{
	unsigned int num_connectors = display->num_connectors;
	struct drm_client_dev *client = display->client;
	struct drm_mode_modeinfo *display_tile_modes;
	struct drm_client_connector **connectors;
	unsigned int i, num_modes = 0;
	bool dummy_changed = false;
	int ret;

	if (mode_changed)
		*mode_changed = false;
	else
		mode_changed = &dummy_changed;

	if (display->cloned)
		return 2;

	ret = drm_client_get_file(client);
	if (ret)
		return ret;

	connectors = kcalloc(num_connectors, sizeof(*connectors), GFP_KERNEL);
	if (!connectors) {
		ret = -ENOMEM;
		goto out_put_file;
	}

	/* Get a new set for comparison */
	for (i = 0; i < num_connectors; i++) {
		connectors[i] = drm_client_get_connector(client, display->connectors[i]->conn_id);
		if (IS_ERR_OR_NULL(connectors[i])) {
			ret = PTR_ERR_OR_ZERO(connectors[i]);
			if (!ret)
				ret = -ENOENT;
			goto out_cleanup;
		}
	}

	/* All connectors should have the same number of modes */
	num_modes = connectors[0]->num_modes;
	for (i = 0; i < num_connectors; i++) {
		if (num_modes != connectors[i]->num_modes) {
			ret = -EINVAL;
			goto out_cleanup;
		}
	}

	if (num_connectors > 1) {
		display_tile_modes = kcalloc(num_modes, sizeof(*display_tile_modes), GFP_KERNEL);
		if (!display_tile_modes) {
			ret = -ENOMEM;
			goto out_cleanup;
		}
	}

	mutex_lock(&display->modes_lock);

	for (i = 0; i < num_connectors; i++) {
		display->connectors[i]->status = connectors[i]->status;
		if (display->connectors[i]->num_modes != connectors[i]->num_modes) {
			display->connectors[i]->num_modes = connectors[i]->num_modes;
			kfree(display->connectors[i]->modes);
			display->connectors[i]->modes = connectors[i]->modes;
			connectors[i]->modes = NULL;
			*mode_changed = true;
		}
	}

	if (num_connectors > 1)
		drm_client_display_fill_tile_modes(display, display_tile_modes);
	else
		display->modes = display->connectors[0]->modes;

	mutex_unlock(&display->modes_lock);

out_cleanup:
	for (i = 0; i < num_connectors; i++)
		drm_client_connector_free(connectors[i]);
	kfree(connectors);
out_put_file:
	drm_client_put_file(client);

	return ret ? ret : num_modes;
}
EXPORT_SYMBOL(drm_client_display_update_modes);

void drm_client_display_free(struct drm_client_display *display)
{
	unsigned int i;

	if (!display)
		return;

	/* tile modes? */
	if (display->modes != display->connectors[0]->modes)
		kfree(display->modes);

	for (i = 0; i < display->num_connectors; i++)
		drm_client_connector_free(display->connectors[i]);

	kfree(display->connectors);
	mutex_destroy(&display->modes_lock);
	kfree(display);
}
EXPORT_SYMBOL(drm_client_display_free);

static struct drm_client_display *
drm_client_display_alloc(struct drm_client_dev *client,
			 unsigned int num_connectors)
{
	struct drm_client_display *display;
	struct drm_client_connector **connectors;

	display = kzalloc(sizeof(*display), GFP_KERNEL);
	connectors = kcalloc(num_connectors, sizeof(*connectors), GFP_KERNEL);
	if (!display || !connectors) {
		kfree(display);
		kfree(connectors);
		return NULL;
	}

	mutex_init(&display->modes_lock);
	display->client = client;
	display->connectors = connectors;
	display->num_connectors = num_connectors;

	return display;
}

/* Logic is from drm_fb_helper */
static struct drm_client_display *
drm_client_connector_pick_cloned(struct drm_client_dev *client,
				 struct drm_client_connector **connectors,
				 unsigned int num_connectors)
{
	struct drm_mode_modeinfo modes[2], udmt_mode, *mode, *tmp;
	struct drm_display_mode *dmt_display_mode = NULL;
	unsigned int i, j, conns[2], num_conns = 0;
	struct drm_client_connector *connector;
	struct drm_device *dev = client->dev;
	struct drm_client_display *display;

	/* only contemplate cloning in the single crtc case */
	if (dev->mode_config.num_crtc > 1)
		return NULL;
retry:
	for (i = 0; i < num_connectors; i++) {
		connector = connectors[i];
		if (!connector || connector->has_tile || !connector->num_modes)
			continue;

		for (j = 0; j < connector->num_modes; j++) {
			mode = &connector->modes[j];
			if (dmt_display_mode) {
				if (drm_umode_equal(&udmt_mode, mode)) {
					conns[num_conns] = i;
					modes[num_conns++] = *mode;
					break;
				}
			} else {
				if (mode->type & DRM_MODE_TYPE_USERDEF) {
					conns[num_conns] = i;
					modes[num_conns++] = *mode;
					break;
				}
			}
		}
		if (num_conns == 2)
			break;
	}

	if (num_conns == 2)
		goto found;

	if (dmt_display_mode)
		return NULL;

	dmt_display_mode = drm_mode_find_dmt(dev, 1024, 768, 60, false);
	drm_mode_convert_to_umode(&udmt_mode, dmt_display_mode);
	drm_mode_destroy(dev, dmt_display_mode);

	goto retry;

found:
	tmp = kcalloc(2, sizeof(*tmp), GFP_KERNEL);
	if (!tmp)
		return ERR_PTR(-ENOMEM);

	display = drm_client_display_alloc(client, 2);
	if (!display) {
		kfree(tmp);
		return ERR_PTR(-ENOMEM);
	}

	for (i = 0; i < 2; i++) {
		connector = connectors[conns[i]];
		display->connectors[i] = connector;
		connectors[conns[i]] = NULL;
		kfree(connector->modes);
		tmp[i] = modes[i];
		connector->modes = &tmp[i];
		connector->num_modes = 1;
	}

	display->cloned = true;
	display->modes = &tmp[0];
	display->num_modes = 1;

	return display;
}

static struct drm_client_display *
drm_client_connector_pick_tile(struct drm_client_dev *client,
			       struct drm_client_connector **connectors,
			       unsigned int num_connectors)
{
	unsigned int i, num_conns, num_modes, tile_group = 0;
	struct drm_mode_modeinfo *tile_modes = NULL;
	struct drm_client_connector *connector;
	struct drm_client_display *display;
	u16 conns[32];

	for (i = 0; i < num_connectors; i++) {
		connector = connectors[i];
		if (!connector || !connector->tile_group)
			continue;

		if (!tile_group) {
			tile_group = connector->tile_group;
			num_modes = connector->num_modes;
		}

		if (connector->tile_group != tile_group)
			continue;

		if (num_modes != connector->num_modes) {
			DRM_ERROR("Tile connectors must have the same number of modes\n");
			return ERR_PTR(-EINVAL);
		}

		conns[num_conns++] = i;
		if (WARN_ON(num_conns == 33))
			return ERR_PTR(-EINVAL);
	}

	if (!num_conns)
		return NULL;

	if (num_modes) {
		tile_modes = kcalloc(num_modes, sizeof(*tile_modes), GFP_KERNEL);
		if (!tile_modes)
			return ERR_PTR(-ENOMEM);
	}

	display = drm_client_display_alloc(client, num_conns);
	if (!display) {
		kfree(tile_modes);
		return ERR_PTR(-ENOMEM);
	}

	if (num_modes)
		drm_client_display_fill_tile_modes(display, tile_modes);

	return display;
}

static struct drm_client_display *
drm_client_connector_pick_not_tile(struct drm_client_dev *client,
				   struct drm_client_connector **connectors,
				   unsigned int num_connectors)
{
	struct drm_client_display *display;
	unsigned int i;

	for (i = 0; i < num_connectors; i++) {
		if (!connectors[i] || connectors[i]->has_tile)
			continue;
		break;
	}

	if (i == num_connectors)
		return NULL;

	display = drm_client_display_alloc(client, 1);
	if (!display)
		return ERR_PTR(-ENOMEM);

	display->connectors[0] = connectors[i];
	connectors[i] = NULL;
	display->modes = display->connectors[0]->modes;
	display->num_modes = display->connectors[0]->num_modes;

	return display;
}

/* Get connectors and bundle them up into displays */
static int drm_client_get_displays(struct drm_client_dev *client,
				   struct drm_client_display ***displays)
{
	int ret, num_connectors, num_displays = 0;
	struct drm_client_connector **connectors;
	struct drm_client_display *display;
	unsigned int i;

	ret = drm_client_get_file(client);
	if (ret)
		return ret;

	num_connectors = drm_client_get_connectors(client, &connectors);
	if (num_connectors <= 0) {
		ret = num_connectors;
		goto err_put_file;
	}

	*displays = kcalloc(num_connectors, sizeof(*displays), GFP_KERNEL);
	if (!(*displays)) {
		ret = -ENOMEM;
		goto err_free;
	}

	display = drm_client_connector_pick_cloned(client, connectors,
						   num_connectors);
	if (IS_ERR(display)) {
		ret = PTR_ERR(display);
		goto err_free;
	}
	if (display)
		(*displays)[num_displays++] = display;

	for (i = 0; i < num_connectors; i++) {
		display = drm_client_connector_pick_tile(client, connectors,
							 num_connectors);
		if (IS_ERR(display)) {
			ret = PTR_ERR(display);
			goto err_free;
		}
		if (!display)
			break;
		(*displays)[num_displays++] = display;
	}

	for (i = 0; i < num_connectors; i++) {
		display = drm_client_connector_pick_not_tile(client, connectors,
							     num_connectors);
		if (IS_ERR(display)) {
			ret = PTR_ERR(display);
			goto err_free;
		}
		if (!display)
			break;
		(*displays)[num_displays++] = display;
	}

	for (i = 0; i < num_connectors; i++) {
		if (connectors[i]) {
			DRM_INFO("Connector %u fell through the cracks.\n",
				 connectors[i]->conn_id);
			drm_client_connector_free(connectors[i]);
		}
	}

	drm_client_put_file(client);
	kfree(connectors);

	return num_displays;

err_free:
	for (i = 0; i < num_displays; i++)
		drm_client_display_free((*displays)[i]);
	kfree(*displays);
	*displays = NULL;
	for (i = 0; i < num_connectors; i++)
		drm_client_connector_free(connectors[i]);
	kfree(connectors);
err_put_file:
	drm_client_put_file(client);

	return ret;
}

static bool
drm_client_display_is_enabled(struct drm_client_display *display, bool strict)
{
	unsigned int i;

	if (!display->num_modes)
		return false;

	for (i = 0; i < display->num_connectors; i++)
		if (!drm_client_connector_is_enabled(display->connectors[i], strict))
			return false;

	return true;
}

/**
 * drm_client_display_get_first_enabled - Get first enabled display
 * @client: DRM client
 * @strict: If true the connector(s) have to be connected, if false they can
 *          also have unknown status.
 *
 * This function gets all connectors and bundles them into displays
 * (tiled/cloned). It then picks the first one with connectors that is enabled
 * according to @strict.
 *
 * Returns:
 * Pointer to a client display if such a display was found, NULL if not found
 * or an error pointer on failure.
 */
struct drm_client_display *
drm_client_display_get_first_enabled(struct drm_client_dev *client, bool strict)
{
	struct drm_client_display **displays, *display = NULL;
	int num_displays;
	unsigned int i;

	num_displays = drm_client_get_displays(client, &displays);
	if (num_displays < 0)
		return ERR_PTR(num_displays);
	if (!num_displays)
		return NULL;

	for (i = 0; i < num_displays; i++) {
		if (!display &&
		    drm_client_display_is_enabled(displays[i], strict)) {
			display = displays[i];
			continue;
		}
		drm_client_display_free(displays[i]);
	}

	kfree(displays);

	return display;
}
EXPORT_SYMBOL(drm_client_display_get_first_enabled);

unsigned int
drm_client_display_preferred_depth(struct drm_client_display *display)
{
	struct drm_connector *conn;
	unsigned int ret;

	conn = drm_connector_lookup(display->client->dev, NULL,
				    display->connectors[0]->conn_id);
	if (!conn)
		return 0;

	if (conn->cmdline_mode.bpp_specified)
		ret = conn->cmdline_mode.bpp;
	else
		ret = display->client->dev->mode_config.preferred_depth;

	drm_connector_put(conn);

	return ret;
}
EXPORT_SYMBOL(drm_client_display_preferred_depth);

int drm_client_display_dpms(struct drm_client_display *display, int mode)
{
	struct drm_mode_obj_set_property prop;

	prop.value = mode;
	prop.prop_id = display->client->dev->mode_config.dpms_property->base.id;
	prop.obj_id = display->connectors[0]->conn_id;
	prop.obj_type = DRM_MODE_OBJECT_CONNECTOR;

	return drm_mode_obj_set_property(display->client->dev, &prop,
					 display->client->file);
}
EXPORT_SYMBOL(drm_client_display_dpms);

int drm_client_display_wait_vblank(struct drm_client_display *display)
{
	struct drm_crtc *crtc;
	union drm_wait_vblank vblank_req = {
		.request = {
			.type = _DRM_VBLANK_RELATIVE,
			.sequence = 1,
		},
	};

	crtc = drm_crtc_find(display->client->dev, display->client->file,
			     display->connectors[0]->crtc_id);
	if (!crtc)
		return -ENOENT;

	vblank_req.request.type |= drm_crtc_index(crtc) << _DRM_VBLANK_HIGH_CRTC_SHIFT;

	return drm_wait_vblank(display->client->dev, &vblank_req,
			       display->client->file);
}
EXPORT_SYMBOL(drm_client_display_wait_vblank);

static int drm_client_get_crtc_index(struct drm_client_dev *client, u32 id)
{
	int i;

	for (i = 0; i < client->num_crtcs; i++)
		if (client->crtcs[i] == id)
			return i;

	return -ENOENT;
}

static int drm_client_display_find_crtcs(struct drm_client_display *display)
{
	struct drm_client_dev *client = display->client;
	struct drm_device *dev = client->dev;
	struct drm_file *file = client->file;
	u32 encoder_ids[DRM_CONNECTOR_MAX_ENCODER];
	unsigned int i, j, available_crtcs = ~0;
	struct drm_mode_get_connector conn_req;
	struct drm_mode_get_encoder enc_req;
	int ret;

	/* Already done? */
	if (display->connectors[0]->crtc_id)
		return 0;

	for (i = 0; i < display->num_connectors; i++) {
		u32 active_crtcs = 0, crtcs_for_connector = 0;
		int crtc_idx;

		memset(&conn_req, 0, sizeof(conn_req));
		conn_req.connector_id = display->connectors[i]->conn_id;
		conn_req.encoders_ptr = (unsigned long)(encoder_ids);
		conn_req.count_encoders = DRM_CONNECTOR_MAX_ENCODER;
		ret = drm_mode_getconnector(dev, &conn_req, file, false);
		if (ret)
			return ret;

		if (conn_req.encoder_id) {
			memset(&enc_req, 0, sizeof(enc_req));
			enc_req.encoder_id = conn_req.encoder_id;
			ret = drm_mode_getencoder(dev, &enc_req, file);
			if (ret)
				return ret;
			crtcs_for_connector |= enc_req.possible_crtcs;
			if (crtcs_for_connector & available_crtcs)
				goto found;
		}

		for (j = 0; j < conn_req.count_encoders; j++) {
			memset(&enc_req, 0, sizeof(enc_req));
			enc_req.encoder_id = encoder_ids[j];
			ret = drm_mode_getencoder(dev, &enc_req, file);
			if (ret)
				return ret;

			crtcs_for_connector |= enc_req.possible_crtcs;

			if (enc_req.crtc_id) {
				crtc_idx = drm_client_get_crtc_index(client, enc_req.crtc_id);
				if (crtc_idx >= 0)
					active_crtcs |= 1 << crtc_idx;
			}
		}

found:
		crtcs_for_connector &= available_crtcs;
		active_crtcs &= available_crtcs;

		if (!crtcs_for_connector)
			return -ENOENT;

		if (active_crtcs)
			crtc_idx = ffs(active_crtcs) - 1;
		else
			crtc_idx = ffs(crtcs_for_connector) - 1;

		if (crtc_idx >= client->num_crtcs)
			return -EINVAL;

		display->connectors[i]->crtc_id = client->crtcs[crtc_idx];
		available_crtcs &= ~BIT(crtc_idx);
	}

	return 0;
}

/**
 * drm_client_display_commit_mode - Commit a mode to the crtc(s)
 * @display: Client display
 * @fb_id: Framebuffer id
 * @mode: Video mode
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int drm_client_display_commit_mode(struct drm_client_display *display,
				   u32 fb_id, struct drm_mode_modeinfo *mode)
{
	struct drm_client_dev *client = display->client;
	struct drm_device *dev = client->dev;
	unsigned int num_crtcs = client->num_crtcs;
	struct drm_file *file = client->file;
	unsigned int *xoffsets = NULL, *yoffsets = NULL;
	struct drm_mode_crtc *crtc_reqs, *req;
	u32 cloned_conn_ids[2];
	unsigned int i;
	int idx, ret;

	ret = drm_client_display_find_crtcs(display);
	if (ret)
		return ret;

	crtc_reqs = kcalloc(num_crtcs, sizeof(*crtc_reqs), GFP_KERNEL);
	if (!crtc_reqs)
		return -ENOMEM;

	for (i = 0; i < num_crtcs; i++)
		crtc_reqs[i].crtc_id = client->crtcs[i];

	if (drm_client_display_is_tiled(display)) {
		/* TODO calculate tile crtc offsets */
	}

	for (i = 0; i < display->num_connectors; i++) {
		idx = drm_client_get_crtc_index(client, display->connectors[i]->crtc_id);
		if (idx < 0)
			return -ENOENT;

		req = &crtc_reqs[idx];

		req->fb_id = fb_id;
		if (xoffsets) {
			req->x = xoffsets[i];
			req->y = yoffsets[i];
		}
		req->mode_valid = 1;
		req->mode = *mode;

		if (display->cloned) {
			cloned_conn_ids[0] = display->connectors[0]->conn_id;
			cloned_conn_ids[1] = display->connectors[1]->conn_id;
			req->set_connectors_ptr = (unsigned long)(cloned_conn_ids);
			req->count_connectors = 2;
			break;
		}

		req->set_connectors_ptr = (unsigned long)(&display->connectors[i]->conn_id);
		req->count_connectors = 1;
	}

	for (i = 0; i < num_crtcs; i++) {
		ret = drm_mode_setcrtc(dev, &crtc_reqs[i], file, false);
		if (ret)
			break;
	}

	kfree(xoffsets);
	kfree(yoffsets);
	kfree(crtc_reqs);

	return ret;
}
EXPORT_SYMBOL(drm_client_display_commit_mode);

unsigned int drm_client_display_current_fb(struct drm_client_display *display)
{
	struct drm_client_dev *client = display->client;
	int ret;
	struct drm_mode_crtc crtc_req = {
		.crtc_id = display->connectors[0]->crtc_id,
	};

	ret = drm_mode_getcrtc(client->dev, &crtc_req, client->file);
	if (ret)
		return 0;

	return crtc_req.fb_id;
}
EXPORT_SYMBOL(drm_client_display_current_fb);

int drm_client_display_flush(struct drm_client_display *display, u32 fb_id,
			     struct drm_clip_rect *clips, unsigned int num_clips)
{
	struct drm_client_dev *client = display->client;
	struct drm_mode_fb_dirty_cmd dirty_req = {
		.fb_id = fb_id,
		.clips_ptr = (unsigned long)clips,
		.num_clips = num_clips,
	};
	int ret;

	if (display->no_flushing)
		return 0;

	ret = drm_mode_dirtyfb(client->dev, &dirty_req, client->file, false);
	if (ret == -ENOSYS) {
		ret = 0;
		display->no_flushing = true;
	}

	return ret;
}
EXPORT_SYMBOL(drm_client_display_flush);

int drm_client_display_page_flip(struct drm_client_display *display, u32 fb_id,
				 bool event)
{
	struct drm_client_dev *client = display->client;
	struct drm_mode_crtc_page_flip_target page_flip_req = {
		.crtc_id = display->connectors[0]->crtc_id,
		.fb_id = fb_id,
	};

	if (event)
		page_flip_req.flags = DRM_MODE_PAGE_FLIP_EVENT;

	return drm_mode_page_flip(client->dev, &page_flip_req, client->file);
	/*
	 * TODO:
	 * Where do we flush on page flip? Should the driver handle that?
	 */
}
EXPORT_SYMBOL(drm_client_display_page_flip);

/**
 * drm_client_framebuffer_create - Create a client framebuffer
 * @client: DRM client
 * @mode: Display mode to create a buffer for
 * @format: Buffer format
 *
 * This function creates a &drm_client_buffer which consists of a
 * &drm_framebuffer backed by a dumb buffer. The dumb buffer is &dma_buf
 * exported to aquire a virtual address which is stored in
 * &drm_client_buffer->vaddr.
 * Call drm_client_framebuffer_delete() to free the buffer.
 *
 * Returns:
 * Pointer to a client buffer or an error pointer on failure.
 */
struct drm_client_buffer *
drm_client_framebuffer_create(struct drm_client_dev *client,
			      struct drm_mode_modeinfo *mode, u32 format)
{
	struct drm_client_buffer *buffer;
	int ret;

	buffer = drm_client_buffer_create(client, mode->hdisplay,
					  mode->vdisplay, format);
	if (IS_ERR(buffer))
		return buffer;

	ret = drm_client_buffer_addfb(buffer, mode);
	if (ret) {
		drm_client_buffer_delete(buffer);
		return ERR_PTR(ret);
	}

	return buffer;
}
EXPORT_SYMBOL(drm_client_framebuffer_create);

void drm_client_framebuffer_delete(struct drm_client_buffer *buffer)
{
	drm_client_buffer_rmfb(buffer);
	drm_client_buffer_delete(buffer);
}
EXPORT_SYMBOL(drm_client_framebuffer_delete);

struct drm_client_buffer *
drm_client_buffer_create(struct drm_client_dev *client, u32 width, u32 height,
			 u32 format)
{
	struct drm_mode_create_dumb dumb_args = { 0 };
	struct drm_prime_handle prime_args = { 0 };
	struct drm_client_buffer *buffer;
	struct dma_buf *dma_buf;
	void *vaddr;
	int ret;

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	ret = drm_client_get_file(client);
	if (ret)
		goto err_free;

	buffer->client = client;
	buffer->width = width;
	buffer->height = height;
	buffer->format = format;

	dumb_args.width = buffer->width;
	dumb_args.height = buffer->height;
	dumb_args.bpp = drm_format_plane_cpp(format, 0) * 8;
	ret = drm_mode_create_dumb(client->dev, &dumb_args, client->file);
	if (ret)
		goto err_put_file;

	buffer->handle = dumb_args.handle;
	buffer->pitch = dumb_args.pitch;
	buffer->size = dumb_args.size;

	prime_args.handle = dumb_args.handle;
	ret = drm_prime_handle_to_fd(client->dev, &prime_args, client->file);
	if (ret)
		goto err_delete;

	dma_buf = dma_buf_get(prime_args.fd);
	if (IS_ERR(dma_buf)) {
		ret = PTR_ERR(dma_buf);
		goto err_delete;
	}

	buffer->dma_buf = dma_buf;

	vaddr = dma_buf_vmap(dma_buf);
	if (!vaddr) {
		ret = -ENOMEM;
		goto err_delete;
	}

	buffer->vaddr = vaddr;

	return buffer;

err_delete:
	drm_client_buffer_delete(buffer);
err_put_file:
	drm_client_put_file(client);
err_free:
	kfree(buffer);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL(drm_client_buffer_create);

void drm_client_buffer_delete(struct drm_client_buffer *buffer)
{
	if (!buffer)
		return;

	if (buffer->vaddr)
		dma_buf_vunmap(buffer->dma_buf, buffer->vaddr);

	if (buffer->dma_buf)
		dma_buf_put(buffer->dma_buf);

	drm_mode_destroy_dumb(buffer->client->dev, buffer->handle,
			      buffer->client->file);
	drm_client_put_file(buffer->client);
	kfree(buffer);
}
EXPORT_SYMBOL(drm_client_buffer_delete);

int drm_client_buffer_addfb(struct drm_client_buffer *buffer,
			    struct drm_mode_modeinfo *mode)
{
	struct drm_client_dev *client = buffer->client;
	struct drm_mode_fb_cmd2 fb_req = { };
	unsigned int num_fbs, *fb_ids;
	int i, ret;

	if (buffer->num_fbs)
		return -EINVAL;

	if (mode->hdisplay > buffer->width || mode->vdisplay > buffer->height)
		return -EINVAL;

	num_fbs = buffer->height / mode->vdisplay;
	fb_ids = kcalloc(num_fbs, sizeof(*fb_ids), GFP_KERNEL);
	if (!fb_ids)
		return -ENOMEM;

	fb_req.width = mode->hdisplay;
	fb_req.height = mode->vdisplay;
	fb_req.pixel_format = buffer->format;
	fb_req.handles[0] = buffer->handle;
	fb_req.pitches[0] = buffer->pitch;

	for (i = 0; i < num_fbs; i++) {
		fb_req.offsets[0] = i * mode->vdisplay * buffer->pitch;
		ret = drm_mode_addfb2(client->dev, &fb_req, client->file,
				      client->funcs->name);
		if (ret)
			goto err_remove;
		fb_ids[i] = fb_req.fb_id;
	}

	buffer->fb_ids = fb_ids;
	buffer->num_fbs = num_fbs;

	return 0;

err_remove:
	for (i--; i >= 0; i--)
		drm_mode_rmfb(client->dev, fb_ids[i], client->file);
	kfree(fb_ids);

	return ret;
}
EXPORT_SYMBOL(drm_client_buffer_addfb);

int drm_client_buffer_rmfb(struct drm_client_buffer *buffer)
{
	unsigned int i;
	int ret;

	if (!buffer || !buffer->num_fbs)
		return 0;

	for (i = 0; i < buffer->num_fbs; i++) {
		ret = drm_mode_rmfb(buffer->client->dev, buffer->fb_ids[i],
				    buffer->client->file);
		if (ret)
			DRM_DEV_ERROR(buffer->client->dev->dev,
				      "Error removing FB:%u (%d)\n",
				      buffer->fb_ids[i], ret);
	}

	kfree(buffer->fb_ids);
	buffer->fb_ids = NULL;
	buffer->num_fbs = 0;

	return 0;
}
EXPORT_SYMBOL(drm_client_buffer_rmfb);
