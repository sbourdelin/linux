// SPDX-License-Identifier: GPL-2.0
// Copyright 2018 Noralf Trønnes

#include <linux/console.h>
#include <linux/dma-buf.h>
#include <linux/fb.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <video/videomode.h>

#include <drm/drm_client.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modes.h>
#include <drm/drm_print.h>

struct drm_fbdev {
	struct mutex lock;

	struct drm_client_dev *client;
	struct drm_client_display *display;

	unsigned int open_count;
	struct drm_client_buffer *buffer;
	bool page_flip_sent;
	u32 curr_fb;

	struct fb_info *info;
	u32 pseudo_palette[17];

	bool flush;
	bool defio_no_flushing;
	struct drm_clip_rect dirty_clip;
	spinlock_t dirty_lock;
	struct work_struct dirty_work;
};

static int drm_fbdev_mode_to_fb_mode(struct drm_device *dev,
				     struct drm_mode_modeinfo *mode,
				     struct fb_videomode *fb_mode)
{
	struct drm_display_mode display_mode = { };
	struct videomode videomode = { };
	int ret;

	ret = drm_mode_convert_umode(dev, &display_mode, mode);
	if (ret)
		return ret;

	memset(fb_mode, 0, sizeof(*fb_mode));
	drm_display_mode_to_videomode(&display_mode, &videomode);
	fb_videomode_from_videomode(&videomode, fb_mode);

	return 0;
}

static void drm_fbdev_destroy_modelist(struct fb_info *info)
{
	struct fb_modelist *modelist, *tmp;

	list_for_each_entry_safe(modelist, tmp, &info->modelist, list) {
		kfree(modelist->mode.name);
		list_del(&modelist->list);
		kfree(modelist);
	}
}

static void drm_fbdev_use_first_mode(struct fb_info *info)
{
	struct fb_modelist *modelist;

	modelist = list_first_entry(&info->modelist, struct fb_modelist, list);
	fb_videomode_to_var(&info->var, &modelist->mode);
	info->mode = &modelist->mode;
}

static struct drm_mode_modeinfo *drm_fbdev_get_drm_mode(struct drm_fbdev *fbdev)
{
	struct drm_mode_modeinfo *mode_pos, *mode = NULL;
	struct fb_info *info = fbdev->info;
	struct fb_videomode tmp;

	mutex_lock(&fbdev->display->modes_lock);
	drm_client_display_for_each_mode(fbdev->display, mode_pos) {
		if (drm_fbdev_mode_to_fb_mode(fbdev->client->dev, mode_pos, &tmp))
			continue;
		if (fb_mode_is_equal(info->mode, &tmp)) {
			mode = mode_pos;
			break;
		}
	}
	mutex_unlock(&fbdev->display->modes_lock);

	return mode;
}

/* Return number of modes or negative error */
static int drm_fbdev_sync_modes(struct drm_fbdev *fbdev, bool force)
{
	struct fb_info *info = fbdev->info;
	struct drm_mode_modeinfo *mode;
	struct fb_videomode fb_mode;
	bool changed;

	struct fb_modelist *fbdev_modelist;
	int num_modes;

	num_modes = drm_client_display_update_modes(fbdev->display, &changed);
	if (num_modes <= 0)
		return num_modes;

	if (!info)
		return num_modes;

	if (!force && !changed)
		return num_modes;

	drm_fbdev_destroy_modelist(info);

	mutex_lock(&fbdev->display->modes_lock);
	drm_client_display_for_each_mode(fbdev->display, mode) {
		if (drm_fbdev_mode_to_fb_mode(fbdev->client->dev, mode, &fb_mode)) {
			num_modes--;
			continue;
		}

		fbdev_modelist = kzalloc(sizeof(*fbdev_modelist), GFP_KERNEL);
		if (!fbdev_modelist) {
			drm_fbdev_destroy_modelist(info);
			mutex_unlock(&fbdev->display->modes_lock);
			return -ENOMEM;
		}

		fbdev_modelist->mode = fb_mode;
		fbdev_modelist->mode.name = kstrndup(mode->name,
						     DRM_DISPLAY_MODE_LEN,
						     GFP_KERNEL);

		if (mode->type & DRM_MODE_TYPE_PREFERRED)
			fbdev_modelist->mode.flag |= FB_MODE_IS_FIRST;

		list_add_tail(&fbdev_modelist->list, &info->modelist);
	}
	mutex_unlock(&fbdev->display->modes_lock);

	if (!fbdev->open_count)
		drm_fbdev_use_first_mode(info);

	return num_modes;
}

static void drm_fbdev_format_fill_var(u32 format, struct fb_var_screeninfo *var)
{
	switch (format) {
	case DRM_FORMAT_XRGB1555:
		var->red.offset = 10;
		var->red.length = 5;
		var->green.offset = 5;
		var->green.length = 5;
		var->blue.offset = 0;
		var->blue.length = 5;
		var->transp.offset = 0;
		var->transp.length = 0;
		break;
	case DRM_FORMAT_ARGB1555:
		var->red.offset = 10;
		var->red.length = 5;
		var->green.offset = 5;
		var->green.length = 5;
		var->blue.offset = 0;
		var->blue.length = 5;
		var->transp.offset = 15;
		var->transp.length = 1;
		break;
	case DRM_FORMAT_RGB565:
		var->red.offset = 11;
		var->red.length = 5;
		var->green.offset = 5;
		var->green.length = 6;
		var->blue.offset = 0;
		var->blue.length = 5;
		var->transp.offset = 0;
		var->transp.length = 0;
		break;
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_XRGB8888:
		var->red.offset = 16;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;
		var->transp.offset = 0;
		var->transp.length = 0;
		break;
	case DRM_FORMAT_ARGB8888:
		var->red.offset = 16;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;
		var->transp.offset = 24;
		var->transp.length = 8;
		break;
	default:
		WARN_ON_ONCE(1);
		return;
	}

	var->colorspace = 0;
	var->grayscale = 0;
	var->nonstd = 0;
}

int drm_fbdev_var_to_format(struct fb_var_screeninfo *var, u32 *format)
{
	switch (var->bits_per_pixel) {
	case 15:
		*format = DRM_FORMAT_ARGB1555;
		break;
	case 16:
		if (var->green.length != 5)
			*format = DRM_FORMAT_RGB565;
		else if (var->transp.length > 0)
			*format = DRM_FORMAT_ARGB1555;
		else
			*format = DRM_FORMAT_XRGB1555;
		break;
	case 24:
		*format = DRM_FORMAT_RGB888;
		break;
	case 32:
		if (var->transp.length > 0)
			*format = DRM_FORMAT_ARGB8888;
		else
			*format = DRM_FORMAT_XRGB8888;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void drm_fbdev_dirty_work(struct work_struct *work)
{
	struct drm_fbdev *fbdev = container_of(work, struct drm_fbdev,
					       dirty_work);
	struct drm_clip_rect *clip = &fbdev->dirty_clip;
	struct drm_clip_rect clip_copy;
	unsigned long flags;

	spin_lock_irqsave(&fbdev->dirty_lock, flags);
	clip_copy = *clip;
	clip->x1 = clip->y1 = ~0;
	clip->x2 = clip->y2 = 0;
	spin_unlock_irqrestore(&fbdev->dirty_lock, flags);

	/* call dirty callback only when it has been really touched */
	if (clip_copy.x1 < clip_copy.x2 && clip_copy.y1 < clip_copy.y2)
		drm_client_display_flush(fbdev->display, fbdev->curr_fb,
					 &clip_copy, 1);
}

static void drm_fbdev_dirty(struct fb_info *info, u32 x, u32 y,
			    u32 width, u32 height)
{
	struct drm_fbdev *fbdev = info->par;
	struct drm_clip_rect *clip = &fbdev->dirty_clip;
	unsigned long flags;

	if (!fbdev->flush)
		return;

	spin_lock_irqsave(&fbdev->dirty_lock, flags);
	clip->x1 = min_t(u32, clip->x1, x);
	clip->y1 = min_t(u32, clip->y1, y);
	clip->x2 = max_t(u32, clip->x2, x + width);
	clip->y2 = max_t(u32, clip->y2, y + height);
	spin_unlock_irqrestore(&fbdev->dirty_lock, flags);

	schedule_work(&fbdev->dirty_work);
}

static void drm_fbdev_deferred_io(struct fb_info *info,
				  struct list_head *pagelist)
{
	struct drm_fbdev *fbdev = info->par;
	unsigned long start, end, min, max;
	struct page *page;
	u32 y1, y2;

	/* Is userspace doing explicit pageflip flushing? */
	if (fbdev->defio_no_flushing)
		return;

	min = ULONG_MAX;
	max = 0;
	list_for_each_entry(page, pagelist, lru) {
		start = page->index << PAGE_SHIFT;
		end = start + PAGE_SIZE;
		min = min(min, start);
		max = max(max, end);
	}

	if (min < max) {
		y1 = min / info->fix.line_length;
		y2 = DIV_ROUND_UP(max, info->fix.line_length);
		y2 = min(y2, info->var.yres);
		drm_fbdev_dirty(info, 0, y1, info->var.xres, y2 - y1);
	}
}

static struct fb_deferred_io drm_fbdev_fbdefio = {
	.delay		= HZ / 20,
	.deferred_io	= drm_fbdev_deferred_io,
};

static int
drm_fbdev_fb_mmap_notsupp(struct fb_info *info, struct vm_area_struct *vma)
{
	return -ENOTSUPP;
}

static void drm_fbdev_delete_buffer(struct drm_fbdev *fbdev)
{
	struct fb_info *info = fbdev->info;

	if (info->fbdefio) {
		/* Stop worker and clear page->mapping */
		fb_deferred_io_cleanup(info);
		info->fbdefio = NULL;
	}
	if (fbdev->flush) {
		fbdev->flush = false;
		cancel_work_sync(&fbdev->dirty_work);
	}

	drm_client_buffer_rmfb(fbdev->buffer);
	drm_client_buffer_delete(fbdev->buffer);

	fbdev->buffer = NULL;
	fbdev->curr_fb = 0;
	fbdev->page_flip_sent = false;
	info->screen_buffer = NULL;
	info->screen_size = 0;
	info->fix.smem_len = 0;
	info->fix.line_length = 0;
}

/* Temporary hack to make tinydrm work before converting to vmalloc buffers */
static int drm_fbdev_cma_deferred_io_mmap(struct fb_info *info,
					  struct vm_area_struct *vma)
{
	fb_deferred_io_mmap(info, vma);
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	return 0;
}

static int drm_fbdev_create_buffer(struct drm_fbdev *fbdev)
{
	struct drm_client_dev *client = fbdev->client;
	struct fb_info *info = fbdev->info;
	struct drm_client_buffer *buffer;
	struct drm_mode_modeinfo *mode;
	u32 format;
	int ret;

	ret = drm_fbdev_var_to_format(&info->var, &format);
	if (ret)
		return ret;

	buffer = drm_client_buffer_create(client, info->var.xres_virtual,
					  info->var.yres_virtual, format);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);

	mode = drm_fbdev_get_drm_mode(fbdev);
	if (!mode)
		return -EINVAL;

	ret = drm_client_buffer_addfb(buffer, mode);
	if (ret)
		goto err_free_buffer;

	fbdev->curr_fb = buffer->fb_ids[0];

	if (drm_mode_can_dirtyfb(client->dev, fbdev->curr_fb, client->file)) {
		fbdev->flush = true;
/*		if (is_vmalloc_addr(buffer->vaddr)) { */
/* Temporary hack for testing on tinydrm before it has moved to vmalloc */
		if (1) {
			fbdev->dirty_clip.x1 = fbdev->dirty_clip.y1 = ~0;
			fbdev->dirty_clip.x2 = fbdev->dirty_clip.y2 = 0;
			info->fbdefio = &drm_fbdev_fbdefio;

			/* tinydrm hack */
			info->fix.smem_start = page_to_phys(virt_to_page(buffer->vaddr));

			fb_deferred_io_init(info);
			/* tinydrm hack */
			info->fbops->fb_mmap = drm_fbdev_cma_deferred_io_mmap;
		} else {
			info->fbops->fb_mmap = drm_fbdev_fb_mmap_notsupp;
		}
	}

	fbdev->buffer = buffer;
	info->screen_buffer = buffer->vaddr;
	info->screen_size = buffer->size;
	info->fix.smem_len = buffer->size;
	info->fix.line_length = buffer->pitch;

	return 0;

err_free_buffer:
	drm_client_buffer_delete(buffer);

	return ret;
}

static int drm_fbdev_fb_open(struct fb_info *info, int user)
{
	struct drm_fbdev *fbdev = info->par;
	int ret = 0;

	DRM_DEV_DEBUG_KMS(fbdev->client->dev->dev, "\n");

	mutex_lock(&fbdev->lock);

	if (!fbdev->display) {
		ret = -ENODEV;
		goto out_unlock;
	}

	if (!fbdev->open_count) {
		/* Pipeline is disabled, make sure it's forced on */
		info->var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
		ret = drm_fbdev_create_buffer(fbdev);
		if (ret)
			goto out_unlock;
	}

	fbdev->open_count++;

out_unlock:
	mutex_unlock(&fbdev->lock);

	if (ret)
		DRM_DEV_ERROR(fbdev->client->dev->dev, "fb_open failed (%d)\n", ret);

	return ret;
}

static int drm_fbdev_fb_release(struct fb_info *info, int user)
{
	struct drm_fbdev *fbdev = info->par;

	DRM_DEV_DEBUG_KMS(fbdev->client->dev->dev, "\n");
	mutex_lock(&fbdev->lock);

	if (--fbdev->open_count == 0) {
		drm_client_display_dpms(fbdev->display, DRM_MODE_DPMS_OFF);
		drm_fbdev_delete_buffer(fbdev);
	}

	fbdev->defio_no_flushing = false;

	mutex_unlock(&fbdev->lock);

	return 0;
}

static ssize_t drm_fbdev_fb_write(struct fb_info *info, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	ssize_t ret;

	ret = fb_sys_write(info, buf, count, ppos);
	if (ret > 0)
		drm_fbdev_dirty(info, 0, 0, info->var.xres, info->var.yres);

	return ret;
}

static void
drm_fbdev_fb_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	sys_fillrect(info, rect);
	drm_fbdev_dirty(info, rect->dx, rect->dy, rect->width, rect->height);
}

static void
drm_fbdev_fb_copyarea(struct fb_info *info, const struct fb_copyarea *area)
{
	sys_copyarea(info, area);
	drm_fbdev_dirty(info, area->dx, area->dy, area->width, area->height);
}

static void
drm_fbdev_fb_imageblit(struct fb_info *info, const struct fb_image *image)
{
	sys_imageblit(info, image);
	drm_fbdev_dirty(info, image->dx, image->dy, image->width, image->height);
}

static int
drm_fbdev_fb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	u32 new_format, old_format, yres_virtual;
	struct drm_fbdev *fbdev = info->par;
	const struct fb_videomode *fb_mode;
	bool is_open;
	int ret;

	mutex_lock(&fbdev->lock);
	is_open = fbdev->open_count;
	mutex_unlock(&fbdev->lock);

	if (!is_open && in_dbg_master())
		return -EINVAL;

	/* Can be called from sysfs */
	if (is_open && (var->xres_virtual > fbdev->buffer->width ||
	    var->yres_virtual > fbdev->buffer->height)) {
		DRM_DEBUG_KMS("Cannot increase virtual resolution while open\n");
		return -EBUSY;
	}

	if (var->xres > var->xres_virtual || var->yres > var->yres_virtual) {
		DRM_DEBUG_KMS("Requested width/height to big: %dx%d > virtual %dx%d\n",
			      var->xres, var->yres, var->xres_virtual,
			      var->yres_virtual);
		return -EINVAL;
	}

	ret = drm_fbdev_var_to_format(var, &new_format);
	if (ret) {
		DRM_DEBUG_KMS("Unsupported format\n");
		return -EINVAL;
	}

	ret = drm_fbdev_var_to_format(&info->var, &old_format);
	if (ret)
		return ret;

	if (new_format != old_format && is_open) {
		DRM_DEBUG_KMS("Cannot change format while open\n");
		return -EBUSY;
	}

	drm_fbdev_format_fill_var(new_format, var);

	fb_mode = fb_find_best_mode(var, &info->modelist);
	if (!fb_mode)
		return -EINVAL;

	yres_virtual = var->yres_virtual;
	fb_videomode_to_var(var, fb_mode);
	var->yres_virtual = yres_virtual;

	return 0;
}

static int drm_fbdev_fb_set_par(struct fb_info *info)
{
	struct drm_fbdev *fbdev = info->par;
	const struct fb_videomode *fb_mode;
	struct drm_mode_modeinfo *mode;
	bool mode_changed;
	int ret;

	mutex_lock(&fbdev->lock);

	if (!fbdev->open_count) {
		ret = 0;
		goto out_unlock;
	}

	fb_mode = fb_match_mode(&info->var, &info->modelist);
	if (!fb_mode) {
		DRM_DEBUG_KMS("Couldn't find var mode\n");
		ret = -EINVAL;
		goto out_unlock;
	}

	mode_changed = !fb_mode_is_equal(info->mode, fb_mode);
	info->mode = (struct fb_videomode *)fb_mode;

	mode = drm_fbdev_get_drm_mode(fbdev);
	if (!mode) {
		DRM_DEBUG_KMS("Couldn't find the matching DRM mode\n");
		ret = -EINVAL;
		goto out_unlock;
	}

	if (mode_changed) {
		drm_client_buffer_rmfb(fbdev->buffer);
		fbdev->curr_fb = 0;
		ret = drm_client_buffer_addfb(fbdev->buffer, mode);
		if (ret)
			goto out_unlock;

		fbdev->curr_fb = fbdev->buffer->fb_ids[0];
		info->var.yoffset = 0;
	}

//	info->var.width = drm_mode->width_mm;
//	info->var.height = drm_mode->height_mm;

	/* Panning is only supported to do page flipping */
	info->fix.ypanstep = info->var.yres;

	ret = drm_client_display_commit_mode(fbdev->display, fbdev->curr_fb, mode);

out_unlock:
	mutex_unlock(&fbdev->lock);

	return ret;
}

/*
 * Do we need to support FB_VISUAL_PSEUDOCOLOR?

static int drm_fbdev_fb_setcolreg(unsigned regno, unsigned red, unsigned green,
		    unsigned blue, unsigned transp, struct fb_info *info)
{

}
*/

static int setcmap_pseudo_palette(struct fb_cmap *cmap, struct fb_info *info)
{
	u32 *palette = (u32 *)info->pseudo_palette;
	int i;

	if (cmap->start + cmap->len > 16)
		return -EINVAL;

	for (i = 0; i < cmap->len; ++i) {
		u16 red = cmap->red[i];
		u16 green = cmap->green[i];
		u16 blue = cmap->blue[i];
		u32 value;

		red >>= 16 - info->var.red.length;
		green >>= 16 - info->var.green.length;
		blue >>= 16 - info->var.blue.length;
		value = (red << info->var.red.offset) |
			(green << info->var.green.offset) |
			(blue << info->var.blue.offset);
		if (info->var.transp.length > 0) {
			u32 mask = (1 << info->var.transp.length) - 1;

			mask <<= info->var.transp.offset;
			value |= mask;
		}
		palette[cmap->start + i] = value;
	}

	return 0;
}

static int drm_fbdev_fb_setcmap(struct fb_cmap *cmap, struct fb_info *info)
{
	if (oops_in_progress)
		return -EBUSY;

	if (info->fix.visual == FB_VISUAL_TRUECOLOR)
		return setcmap_pseudo_palette(cmap, info);

	return -EINVAL;
}

static int drm_fbdev_fb_blank(int blank, struct fb_info *info)
{
	struct drm_fbdev *fbdev = info->par;
	bool is_open;
	int mode;

	if (oops_in_progress)
		return -EBUSY;

	mutex_lock(&fbdev->lock);
	is_open = fbdev->open_count;
	mutex_unlock(&fbdev->lock);

	if (!is_open)
		return -EINVAL;

	if (blank == FB_BLANK_UNBLANK)
		mode = DRM_MODE_DPMS_ON;
	else
		mode = DRM_MODE_DPMS_OFF;

	return drm_client_display_dpms(fbdev->display, mode);
}

static int
drm_fbdev_fb_pan_display(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct drm_fbdev *fbdev = info->par;
	struct drm_event *event;
	unsigned int fb_idx;
	int ret = 0;

	mutex_lock(&fbdev->lock);

	if (!fbdev->open_count)
		goto out_unlock;

	fb_idx = var->yoffset / info->var.yres;
	if (fb_idx >= fbdev->buffer->num_fbs) {
		ret = -EINVAL;
		goto out_unlock;
	}

	/* Drain previous flip event if userspace didn't care */
	if (fbdev->page_flip_sent) {
		event = drm_client_read_event(fbdev->client, false);
		if (!IS_ERR(event))
			kfree(event);
		fbdev->page_flip_sent = false;
	}

	if (fbdev->curr_fb == fbdev->buffer->fb_ids[fb_idx])
		goto out_unlock;

	fbdev->curr_fb = fbdev->buffer->fb_ids[fb_idx];
	fbdev->defio_no_flushing = true;

	ret = drm_client_display_page_flip(fbdev->display, fbdev->curr_fb, true);
	if (ret)
		goto out_unlock;

	fbdev->page_flip_sent = true;

out_unlock:
	mutex_unlock(&fbdev->lock);

	return ret;
}

static int drm_fbdev_fb_ioctl(struct fb_info *info, unsigned int cmd,
			      unsigned long arg)
{
	struct drm_fbdev *fbdev = info->par;
	struct drm_event *event;
	bool page_flip_sent;
	int ret = 0;

	switch (cmd) {
//	case FBIOGET_VBLANK:
//		break;
	case FBIO_WAITFORVSYNC:
		mutex_lock(&fbdev->lock);
		page_flip_sent = fbdev->page_flip_sent;
		fbdev->page_flip_sent = false;
		mutex_unlock(&fbdev->lock);

		if (page_flip_sent) {
			event = drm_client_read_event(fbdev->client, true);
			if (IS_ERR(event))
				ret = PTR_ERR(event);
			else
				kfree(event);
		} else {
			drm_client_display_wait_vblank(fbdev->display);
		}

		break;
	default:
		ret = -ENOTTY;
	}

	return ret;
}

static int drm_fbdev_fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct drm_fbdev *fbdev = info->par;

	return dma_buf_mmap(fbdev->buffer->dma_buf, vma, 0);
}

static void drm_fbdev_fb_destroy(struct fb_info *info)
{
	struct drm_fbdev *fbdev = info->par;

	DRM_DEV_DEBUG_KMS(fbdev->client->dev->dev, "\n");
	drm_client_display_free(fbdev->display);
	drm_client_free(fbdev->client);
	kfree(fbdev);
}

static struct fb_ops drm_fbdev_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_open	= drm_fbdev_fb_open,
	.fb_release	= drm_fbdev_fb_release,
	.fb_read	= fb_sys_read,
	.fb_write	= drm_fbdev_fb_write,
	.fb_check_var	= drm_fbdev_fb_check_var,
	.fb_set_par	= drm_fbdev_fb_set_par,
//	.fb_setcolreg	= drm_fbdev_fb_setcolreg,
	.fb_setcmap	= drm_fbdev_fb_setcmap,
	.fb_blank	= drm_fbdev_fb_blank,
	.fb_pan_display	= drm_fbdev_fb_pan_display,
	.fb_fillrect	= drm_fbdev_fb_fillrect,
	.fb_copyarea	= drm_fbdev_fb_copyarea,
	.fb_imageblit	= drm_fbdev_fb_imageblit,
	.fb_ioctl	= drm_fbdev_fb_ioctl,
	.fb_mmap	= drm_fbdev_fb_mmap,
	.fb_destroy	= drm_fbdev_fb_destroy,
};

static int drm_fbdev_register_framebuffer(struct drm_fbdev *fbdev)
{
	struct drm_client_display *display;
	struct fb_info *info;
	struct fb_ops *fbops;
	u32 format;
	int ret;

	display = drm_client_display_get_first_enabled(fbdev->client, false);
	if (IS_ERR_OR_NULL(display))
		return PTR_ERR_OR_ZERO(display);

	fbdev->display = display;

	/*
	 * fb_deferred_io_cleanup() clears &fbops->fb_mmap so a per instance
	 * version is necessary. We do it for all users since we don't know
	 * yet if the fb has a dirty callback.
	 */
	fbops = kzalloc(sizeof(*fbops), GFP_KERNEL);
	if (!fbops) {
		ret = -ENOMEM;
		goto err_free;
	}

	*fbops = drm_fbdev_fb_ops;

	info = framebuffer_alloc(0, fbdev->client->dev->dev);
	if (!info) {
		ret = -ENOMEM;
		goto err_free;
	}

	ret = fb_alloc_cmap(&info->cmap, 256, 0);
	if (ret)
		goto err_release;

	info->par = fbdev;
	info->fbops = fbops;
	INIT_LIST_HEAD(&info->modelist);
	info->pseudo_palette = fbdev->pseudo_palette;

	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.ypanstep = info->var.yres;

	strcpy(info->fix.id, "DRM emulated");

	fbdev->info = info;
	ret = drm_fbdev_sync_modes(fbdev, true);
	if (ret < 0)
		goto err_free_cmap;

	info->var.bits_per_pixel = drm_client_display_preferred_depth(fbdev->display);
	ret = drm_fbdev_var_to_format(&info->var, &format);
	if (ret) {
		DRM_WARN("Unsupported bpp, assuming x8r8g8b8 pixel format\n");
		format = DRM_FORMAT_XRGB8888;
	}
	drm_fbdev_format_fill_var(format, &info->var);

	info->var.xres_virtual = info->var.xres;
	info->var.yres_virtual = info->var.yres;

	info->var.yres_virtual *= CONFIG_DRM_FBDEV_OVERALLOC;
	info->var.yres_virtual /= 100;

	ret = register_framebuffer(info);
	if (ret)
		goto err_free_cmap;

	dev_info(fbdev->client->dev->dev, "fb%d: %s frame buffer device\n",
		 info->node, info->fix.id);

	return 0;

err_free_cmap:
	fb_dealloc_cmap(&info->cmap);
err_release:
	framebuffer_release(info);
err_free:
	kfree(fbops);
	fbdev->info = NULL;
	drm_client_display_free(fbdev->display);
	fbdev->display = NULL;

	return ret;
}

static int drm_fbdev_client_hotplug(struct drm_client_dev *client)
{
	struct drm_fbdev *fbdev = client->private;
	int ret;

	if (!fbdev->info)
		ret = drm_fbdev_register_framebuffer(fbdev);
	else
		ret = drm_fbdev_sync_modes(fbdev, false);

	return ret;
}

static int drm_fbdev_client_new(struct drm_client_dev *client)
{
	struct drm_fbdev *fbdev;

	fbdev = kzalloc(sizeof(*fbdev), GFP_KERNEL);
	if (!fbdev)
		return -ENOMEM;

	mutex_init(&fbdev->lock);
	spin_lock_init(&fbdev->dirty_lock);
	INIT_WORK(&fbdev->dirty_work, drm_fbdev_dirty_work);

	fbdev->client = client;
	client->private = fbdev;

	/*
	 * vc4 isn't done with it's setup when drm_dev_register() is called.
	 * It should have shouldn't it?
	 * So to keep it from crashing defer setup to hotplug...
	 */
	if (client->dev->mode_config.max_width)
		drm_fbdev_client_hotplug(client);

	return 0;
}

static int drm_fbdev_client_remove(struct drm_client_dev *client)
{
	struct drm_fbdev *fbdev = client->private;

	if (!fbdev->info) {
		kfree(fbdev);
		return 0;
	}

	unregister_framebuffer(fbdev->info);

	/* drm_fbdev_fb_destroy() frees the client */
	return 1;
}

static int drm_fbdev_client_lastclose(struct drm_client_dev *client)
{
	struct drm_fbdev *fbdev = client->private;
	int ret = -ENOENT;

	if (fbdev->info)
		ret = fbdev->info->fbops->fb_set_par(fbdev->info);

	return ret;
}

static const struct drm_client_funcs drm_fbdev_client_funcs = {
	.name		= "drm_fbdev",
	.new		= drm_fbdev_client_new,
	.remove		= drm_fbdev_client_remove,
	.lastclose	= drm_fbdev_client_lastclose,
	.hotplug	= drm_fbdev_client_hotplug,
};

static int __init drm_fbdev_init(void)
{
	return drm_client_register(&drm_fbdev_client_funcs);
}
module_init(drm_fbdev_init);

static void __exit drm_fbdev_exit(void)
{
	drm_client_unregister(&drm_fbdev_client_funcs);
}
module_exit(drm_fbdev_exit);

MODULE_DESCRIPTION("DRM Generic fbdev emulation");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
