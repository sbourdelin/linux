#ifndef __DRM_FB_GEM_HELPER_H__
#define __DRM_FB_GEM_HELPER_H__

#include <drm/drm_framebuffer.h>

struct drm_gem_shmem_object;
struct drm_mode_fb_cmd2;
struct drm_plane;
struct drm_plane_state;

/**
 * struct drm_fb_gem - GEM backed framebuffer
 */
struct drm_fb_gem {
	/**
	 * @base: Base DRM framebuffer
	 */
	struct drm_framebuffer base;
	/**
	 * @obj: GEM object array backing the framebuffer. One object per
	 * plane.
	 */
	struct drm_gem_object *obj[4];
};

static inline struct drm_fb_gem *to_fb_gem(struct drm_framebuffer *fb)
{
	return container_of(fb, struct drm_fb_gem, base);
}

struct drm_gem_object *drm_fb_gem_get_obj(struct drm_framebuffer *fb,
					  unsigned int plane);
struct drm_fb_gem *
drm_fb_gem_alloc(struct drm_device *dev,
		 const struct drm_mode_fb_cmd2 *mode_cmd,
		 struct drm_gem_object **obj, unsigned int num_planes,
		 const struct drm_framebuffer_funcs *funcs);
void drm_fb_gem_destroy(struct drm_framebuffer *fb);
int drm_fb_gem_create_handle(struct drm_framebuffer *fb, struct drm_file *file,
			     unsigned int *handle);

struct drm_framebuffer *
drm_fb_gem_create_with_funcs(struct drm_device *dev, struct drm_file *file,
			     const struct drm_mode_fb_cmd2 *mode_cmd,
			     const struct drm_framebuffer_funcs *funcs);
struct drm_framebuffer *
drm_fb_gem_create(struct drm_device *dev, struct drm_file *file,
		  const struct drm_mode_fb_cmd2 *mode_cmd);


int drm_fb_gem_prepare_fb(struct drm_plane *plane,
			  struct drm_plane_state *state);




#ifdef CONFIG_DEBUG_FS
struct seq_file;

int drm_fb_gem_debugfs_show(struct seq_file *m, void *arg);
#endif

#endif

