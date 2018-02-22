/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/mutex.h>

struct dma_buf;
struct drm_clip_rect;
struct drm_device;
struct drm_file;
struct drm_mode_modeinfo;

struct drm_client_dev;

/**
 * struct drm_client_funcs - DRM client  callbacks
 */
struct drm_client_funcs {
	/**
	 * @name:
	 *
	 * Name of the client.
	 */
	const char *name;

	/**
	 * @new:
	 *
	 * Called when a client or a &drm_device is registered.
	 * If the callback returns anything but zero, then this client instance
	 * is dropped.
	 *
	 * This callback is mandatory.
	 */
	int (*new)(struct drm_client_dev *client);

	/**
	 * @remove:
	 *
	 * Called when a &drm_device is unregistered or the client is
	 * unregistered. If zero is returned drm_client_free() is called
	 * automatically. If the client can't drop it's resources it should
	 * return non-zero and call drm_client_free() later.
	 *
	 * This callback is optional.
	 */
	int (*remove)(struct drm_client_dev *client);

	/**
	 * @lastclose:
	 *
	 * Called on drm_lastclose(). The first client instance in the list
	 * that returns zero gets the privilege to restore and no more clients
	 * are called.
	 *
	 * This callback is optional.
	 */
	int (*lastclose)(struct drm_client_dev *client);

	/**
	 * @hotplug:
	 *
	 * Called on drm_kms_helper_hotplug_event().
	 *
	 * This callback is optional.
	 */
	int (*hotplug)(struct drm_client_dev *client);

// TODO
//	void (*suspend)(struct drm_client_dev *client);
//	void (*resume)(struct drm_client_dev *client);
};

/**
 * struct drm_client_dev - DRM client instance
 */
struct drm_client_dev {
	struct list_head list;
	struct drm_device *dev;
	const struct drm_client_funcs *funcs;
	struct mutex lock;
	struct drm_file *file;
	unsigned int file_ref_count;
	u32 *crtcs;
	unsigned int num_crtcs;
	u32 min_width;
	u32 max_width;
	u32 min_height;
	u32 max_height;
	void *private;
};

void drm_client_free(struct drm_client_dev *client);
int drm_client_register(const struct drm_client_funcs *funcs);
void drm_client_unregister(const struct drm_client_funcs *funcs);

void drm_client_dev_register(struct drm_device *dev);
void drm_client_dev_unregister(struct drm_device *dev);
void drm_client_dev_hotplug(struct drm_device *dev);
void drm_client_dev_lastclose(struct drm_device *dev);

int drm_client_get_file(struct drm_client_dev *client);
void drm_client_put_file(struct drm_client_dev *client);
struct drm_event *
drm_client_read_event(struct drm_client_dev *client, bool block);

struct drm_client_connector {
	unsigned int conn_id;
	unsigned int status;
	unsigned int crtc_id;
	struct drm_mode_modeinfo *modes;
	unsigned int num_modes;
	bool has_tile;
	int tile_group;
	u8 tile_h_loc, tile_v_loc;
};

struct drm_client_display {
	struct drm_client_dev *client;

	struct drm_client_connector **connectors;
	unsigned int num_connectors;

	struct mutex modes_lock;
	struct drm_mode_modeinfo *modes;
	unsigned int num_modes;

	bool cloned;
	bool no_flushing;
};

void drm_client_display_free(struct drm_client_display *display);
struct drm_client_display *
drm_client_display_get_first_enabled(struct drm_client_dev *client, bool strict);

int drm_client_display_update_modes(struct drm_client_display *display,
				    bool *mode_changed);

static inline bool
drm_client_display_is_tiled(struct drm_client_display *display)
{
	return !display->cloned && display->num_connectors > 1;
}

int drm_client_display_dpms(struct drm_client_display *display, int mode);
int drm_client_display_wait_vblank(struct drm_client_display *display);

struct drm_mode_modeinfo *
drm_client_display_first_mode(struct drm_client_display *display);
struct drm_mode_modeinfo *
drm_client_display_next_mode(struct drm_client_display *display,
			     struct drm_mode_modeinfo *mode);

#define drm_client_display_for_each_mode(display, mode) \
	for (mode = drm_client_display_first_mode(display); mode; \
	     mode = drm_client_display_next_mode(display, mode))

unsigned int
drm_client_display_preferred_depth(struct drm_client_display *display);

int drm_client_display_commit_mode(struct drm_client_display *display,
				   u32 fb_id, struct drm_mode_modeinfo *mode);
unsigned int drm_client_display_current_fb(struct drm_client_display *display);
int drm_client_display_flush(struct drm_client_display *display, u32 fb_id,
			     struct drm_clip_rect *clips, unsigned int num_clips);
int drm_client_display_page_flip(struct drm_client_display *display, u32 fb_id,
				 bool event);

struct drm_client_buffer {
	struct drm_client_dev *client;
	u32 width;
	u32 height;
	u32 format;
	u32 handle;
	u32 pitch;
	u64 size;
	struct dma_buf *dma_buf;
	void *vaddr;

	unsigned int *fb_ids;
	unsigned int num_fbs;
};

struct drm_client_buffer *
drm_client_framebuffer_create(struct drm_client_dev *client,
			      struct drm_mode_modeinfo *mode, u32 format);
void drm_client_framebuffer_delete(struct drm_client_buffer *buffer);
struct drm_client_buffer *
drm_client_buffer_create(struct drm_client_dev *client, u32 width, u32 height,
			 u32 format);
void drm_client_buffer_delete(struct drm_client_buffer *buffer);
int drm_client_buffer_addfb(struct drm_client_buffer *buffer,
			    struct drm_mode_modeinfo *mode);
int drm_client_buffer_rmfb(struct drm_client_buffer *buffer);
