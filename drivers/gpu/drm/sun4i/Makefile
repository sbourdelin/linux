sun4i-drm-y += sun4i_drv.o
sun4i-drm-y += sun4i_framebuffer.o
sun4i-drm-y += sun4i_backend.o
sun4i-drm-$(CONFIG_DRM_SUN4I_DE2) += sun8i_mixer.o

sun4i-tcon-y += sun4i_tcon.o
sun4i-tcon-y += sun4i_rgb.o
sun4i-tcon-y += sun4i_dotclock.o
sun4i-tcon-y += sun4i_crtc.o
sun4i-tcon-y += sun4i_layer.o

obj-$(CONFIG_DRM_SUN4I)		+= sun4i-drm.o sun4i-tcon.o
obj-$(CONFIG_DRM_SUN4I)		+= sun6i_drc.o
obj-$(CONFIG_DRM_SUN4I)		+= sun4i_tv.o
