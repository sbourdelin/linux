#ifndef __DRM_FB_SHMEM_HELPER_H__
#define __DRM_FB_SHMEM_HELPER_H__

struct drm_fb_helper_surface_size;
struct drm_framebuffer_funcs;
struct drm_fb_helper;

int drm_fb_shmem_fbdev_probe(struct drm_fb_helper *helper,
			     struct drm_fb_helper_surface_size *sizes,
			     const struct drm_framebuffer_funcs *fb_funcs);

#ifdef CONFIG_DEBUG_FS
struct seq_file;

int drm_fb_shmem_debugfs_show(struct seq_file *m, void *arg);
#endif

#endif
