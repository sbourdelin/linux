/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _DRM_CLIENT_H_
#define _DRM_CLIENT_H_

#include <linux/types.h>

struct drm_device;
struct drm_file;
struct drm_framebuffer;
struct drm_gem_object;

/**
 * struct drm_client_dev - DRM client instance
 */
struct drm_client_dev {
	/**
	 * @dev: DRM device
	 */
	struct drm_device *dev;

	/**
	 * @name: Name of the client.
	 */
	const char *name;

	/**
	 * @file: DRM file
	 */
	struct drm_file *file;
};

int drm_client_new(struct drm_device *dev, struct drm_client_dev *client,
		   const char *name);
void drm_client_release(struct drm_client_dev *client);

/**
 * struct drm_client_buffer - DRM client buffer
 */
struct drm_client_buffer {
	/**
	 * @client: DRM client
	 */
	struct drm_client_dev *client;

	/**
	 * @handle: Buffer handle
	 */
	u32 handle;

	/**
	 * @pitch: Buffer pitch
	 */
	u32 pitch;

	/**
	 * @gem: GEM object backing this buffer
	 */
	struct drm_gem_object *gem;

	/**
	 * @vaddr: Virtual address for the buffer
	 */
	void *vaddr;

	/**
	 * @fb: DRM framebuffer
	 */
	struct drm_framebuffer *fb;
};

struct drm_client_buffer *
drm_client_framebuffer_create(struct drm_client_dev *client, u32 width, u32 height, u32 format);
void drm_client_framebuffer_delete(struct drm_client_buffer *buffer);

#endif
