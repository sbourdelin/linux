
/*
 * Copyright 2012 Red Hat
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License version 2. See the file COPYING in the main
 * directory of this archive for more details.
 *
 * Authors: Matthew Garrett
 *          Dave Airlie
 *
 * Portions of this code derived from cirrusfb.c:
 * drivers/video/cirrusfb.c - driver for Cirrus Logic chipsets
 *
 * Copyright 1999-2001 Jeff Garzik <jgarzik@pobox.com>
 */
#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_plane_helper.h>

#include <video/cirrus.h>

#include "cirrus_drv.h"

#define CIRRUS_LUT_SIZE 256

#define PALETTE_INDEX 0x8
#define PALETTE_DATA 0x9

/*
 * This file contains setup code for the CRTC.
 */

/*
 * The DRM core requires DPMS functions, but they make little sense in our
 * case and so are just stubs
 */

static void cirrus_crtc_dpms(struct drm_crtc *crtc, int mode)
{
	struct drm_device *dev = crtc->dev;
	struct cirrus_device *cdev = dev->dev_private;
	u8 sr01, gr0e;

	switch (mode) {
	case DRM_MODE_DPMS_ON:
		sr01 = 0x00;
		gr0e = 0x00;
		break;
	case DRM_MODE_DPMS_STANDBY:
		sr01 = 0x20;
		gr0e = 0x02;
		break;
	case DRM_MODE_DPMS_SUSPEND:
		sr01 = 0x20;
		gr0e = 0x04;
		break;
	case DRM_MODE_DPMS_OFF:
		sr01 = 0x20;
		gr0e = 0x06;
		break;
	default:
		return;
	}

	WREG8(SEQ_INDEX, 0x1);
	sr01 |= RREG8(SEQ_DATA) & ~0x20;
	WREG_SEQ(0x1, sr01);

	WREG8(GFX_INDEX, 0xe);
	gr0e |= RREG8(GFX_DATA) & ~0x06;
	WREG_GFX(0xe, gr0e);
}

/*
 * The core passes us a mode and we have to program it. The modesetting here
 * is the bare minimum required to satisfy the qemu emulation of this
 * hardware, and running this against a real device is likely to result in
 * an inadequately programmed mode.
 */
static void cirrus_mode_set_nofb(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct cirrus_device *cdev = dev->dev_private;
	struct drm_display_mode *mode = &crtc->mode;
	int hsyncstart, hsyncend, htotal, hdispend;
	int vtotal, vdispend;
	int tmp;

	htotal = mode->htotal / 8;
	hsyncend = mode->hsync_end / 8;
	hsyncstart = mode->hsync_start / 8;
	hdispend = mode->hdisplay / 8;

	vtotal = mode->vtotal;
	vdispend = mode->vdisplay;

	vdispend -= 1;
	vtotal -= 2;

	htotal -= 5;
	hdispend -= 1;
	hsyncstart += 1;
	hsyncend += 1;

	WREG_CRT(VGA_CRTC_V_SYNC_END, 0x20);
	WREG_CRT(VGA_CRTC_H_TOTAL, htotal);
	WREG_CRT(VGA_CRTC_H_DISP, hdispend);
	WREG_CRT(VGA_CRTC_H_SYNC_START, hsyncstart);
	WREG_CRT(VGA_CRTC_H_SYNC_END, hsyncend);
	WREG_CRT(VGA_CRTC_V_TOTAL, vtotal & 0xff);
	WREG_CRT(VGA_CRTC_V_DISP_END, vdispend & 0xff);

	tmp = 0x40;
	if ((vdispend + 1) & 512)
		tmp |= 0x20;
	WREG_CRT(VGA_CRTC_MAX_SCAN, tmp);

	/*
	 * Overflow bits for values that don't fit in the standard registers
	 */
	tmp = 16;
	if (vtotal & 256)
		tmp |= 1;
	if (vdispend & 256)
		tmp |= 2;
	if ((vdispend + 1) & 256)
		tmp |= 8;
	if (vtotal & 512)
		tmp |= 32;
	if (vdispend & 512)
		tmp |= 64;
	WREG_CRT(VGA_CRTC_OVERFLOW, tmp);

	tmp = 0;

	/* More overflow bits */

	if ((htotal + 5) & 64)
		tmp |= 16;
	if ((htotal + 5) & 128)
		tmp |= 32;
	if (vtotal & 256)
		tmp |= 64;
	if (vtotal & 512)
		tmp |= 128;

	WREG_CRT(CL_CRT1A, tmp);

	/* Disable Hercules/CGA compatibility */
	WREG_CRT(VGA_CRTC_MODE, 0x03);

	/* Enable high-colour modes */
	WREG_GFX(VGA_GFX_MODE, 0x40);

	/* And set graphics mode */
	WREG_GFX(VGA_GFX_MISC, 0x01);
}

/*
 * This is called after a mode is programmed. It should reverse anything done
 * by the prepare function
 */
static void cirrus_crtc_commit(struct drm_crtc *crtc)
{
}

/*
 * The core can pass us a set of gamma values to program. We actually only
 * use this for 8-bit mode so can't perform smooth fades on deeper modes,
 * but it's a requirement that we provide the function
 */
static int cirrus_crtc_gamma_set(struct drm_crtc *crtc, u16 *red, u16 *green,
				 u16 *blue, uint32_t size,
				 struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_device *dev = crtc->dev;
	struct cirrus_device *cdev = dev->dev_private;
	u16 *r, *g, *b;
	int i;

	if (!crtc->enabled)
		return 0;

	r = crtc->gamma_store;
	g = r + crtc->gamma_size;
	b = g + crtc->gamma_size;

	for (i = 0; i < CIRRUS_LUT_SIZE; i++) {
		/* VGA registers */
		WREG8(PALETTE_INDEX, i);
		WREG8(PALETTE_DATA, *r++ >> 8);
		WREG8(PALETTE_DATA, *g++ >> 8);
		WREG8(PALETTE_DATA, *b++ >> 8);
	}

	return 0;
}

/* Simple cleanup function */
static void cirrus_crtc_destroy(struct drm_crtc *crtc)
{
	struct cirrus_crtc *cirrus_crtc = to_cirrus_crtc(crtc);

	drm_crtc_cleanup(crtc);
	kfree(cirrus_crtc);
}

static void cirrus_crtc_atomic_flush(struct drm_crtc *crtc,
				     struct drm_crtc_state *old_crtc_state)
{
	struct drm_device *dev = crtc->dev;
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	if (crtc->state && crtc->state->event) {
		event = crtc->state->event;
		crtc->state->event = NULL;

		spin_lock_irqsave(&dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irqrestore(&dev->event_lock, flags);
	}
}

/* These provide the minimum set of functions required to handle a CRTC */
static const struct drm_crtc_funcs cirrus_crtc_funcs = {
	.gamma_set = cirrus_crtc_gamma_set,
	.set_config = drm_atomic_helper_set_config,
	.destroy = cirrus_crtc_destroy,
	.page_flip = drm_atomic_helper_page_flip,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_crtc_helper_funcs cirrus_helper_funcs = {
	.dpms = cirrus_crtc_dpms,
	.mode_set_nofb = cirrus_mode_set_nofb,
	.commit = cirrus_crtc_commit,
	.atomic_flush = cirrus_crtc_atomic_flush,
};

static void cirrus_argb_to_cursor(void *src , void __iomem *dst,
				  uint32_t cursor_size)
{
	uint8_t *pixel = (uint8_t *)src;
	const uint32_t row_size = cursor_size / 8;
	const uint32_t plane_size = row_size * cursor_size;
	uint32_t row_skip;
	void __iomem *plane_0 = dst;
	void __iomem *plane_1;
	uint32_t x;
	uint32_t y;

	switch (cursor_size) {
	case 32:
		row_skip = 0;
		plane_1 = plane_0 + plane_size;
		break;
	case 64:
		row_skip = row_size;
		plane_1 = plane_0 + row_size;
		break;
	default:
		DRM_DEBUG("Cursor plane format is undefined for given size");
		return;
	}

	for (y = 0; y < cursor_size; y++) {
		uint8_t bits_0 = 0;
		uint8_t bits_1 = 0;

		for (x = 0; x < cursor_size; x++) {
			uint8_t alpha = pixel[3];
			int intensity = pixel[0] + pixel[1] + pixel[2];

			intensity /= 3;
			bits_0 <<= 1;
			bits_1 <<= 1;
			if (alpha > 0x7f) {
				bits_1 |= 1;
				if (intensity > 0x7f)
					bits_0 |= 1;
			}
			if ((x % 8) == 7) {
				iowrite8(bits_0, plane_0);
				iowrite8(bits_1, plane_1);
				plane_0++;
				plane_1++;
				bits_0 = 0;
				bits_1 = 0;
			}
			pixel += 4;
		}
		plane_0 += row_skip;
		plane_1 += row_skip;
	}
}

static int cirrus_bo_to_cursor(struct cirrus_device *cdev,
			       struct drm_framebuffer *fb,
			       uint32_t cursor_size, uint32_t cursor_index)
{
	const uint32_t pixel_count = cursor_size * cursor_size;
	const uint32_t plane_size = pixel_count / 8;
	const uint32_t cursor_offset = cursor_index * plane_size * 2;
	int ret = 0;
	struct drm_device *dev = cdev->dev;
	struct drm_gem_object *obj;
	struct cirrus_bo *bo;
	struct ttm_bo_kmap_obj bo_kmap;
	bool is_iomem;
	struct ttm_tt *ttm;
	void *bo_ptr;

	if ((cursor_size == 32 && cursor_index >= 64) ||
	    (cursor_size == 64 && cursor_index >= 16)) {
		DRM_ERROR("Cursor index is out of bounds\n");
		return -EINVAL;
	}

	mutex_lock(&dev->struct_mutex);
	obj = to_cirrus_framebuffer(fb)->obj;
	if (obj == NULL) {
		ret = -ENOENT;
		DRM_ERROR("Buffer handle for cursor is invalid\n");
		goto out_unlock;
	}

	bo = gem_to_cirrus_bo(obj);
	ttm = bo->bo.ttm;

	ret = ttm_bo_kmap(&bo->bo, 0, bo->bo.num_pages, &bo_kmap);
	if (ret) {
		DRM_ERROR("Cursor failed kmap of buffer object\n");
		goto out_unlock;
	}

	bo_ptr = ttm_kmap_obj_virtual(&bo_kmap, &is_iomem);

	cirrus_argb_to_cursor(bo_ptr, cdev->cursor_iomem + cursor_offset,
			      cursor_size);

	ttm_bo_kunmap(&bo_kmap);
out_unlock:
	mutex_unlock(&dev->struct_mutex);
	return ret;
}


int cirrus_cursor_atomic_check(struct drm_plane *plane,
			   struct drm_plane_state *state)
{
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_object *obj;
	struct cirrus_bo *bo;
	uint32_t pixel_count;
	uint32_t expected_pages;

	if (!fb)
		return 0;
	if (fb->width != fb->height) {
		DRM_DEBUG("Cursors are expected to have square dimensions\n");
		return -EINVAL;
	}

	if (!(fb->width == 32 || fb->width == 64)) {
		DRM_ERROR("Cursor dimension are expected to be 32 or 64\n");
		return -EINVAL;
	}

	obj = to_cirrus_framebuffer(fb)->obj;
	if (obj == NULL) {
		DRM_ERROR("Buffer handle for cursor is invalid\n");
		return -ENOENT;
	}
	bo = gem_to_cirrus_bo(obj);
	pixel_count = fb->width * fb->width;
	expected_pages = DIV_ROUND_UP(pixel_count * 4, PAGE_SIZE);
	if (bo->bo.num_pages < expected_pages) {
		DRM_ERROR("Buffer object for cursor is too small\n");
		return -EINVAL;
	}

	return 0;
}

static void cirrus_cursor_atomic_update(struct drm_plane *plane,
				    struct drm_plane_state *old_state)
{
	int ret;
	struct drm_device *dev = plane->state->crtc->dev;
	struct cirrus_device *cdev = dev->dev_private;
	struct drm_framebuffer *fb = plane->state->fb;
	uint8_t cursor_index = 0;
	int width, x, y;
	int sr10, sr10_index;
	int sr11, sr11_index;
	int sr12, sr13;

	width = fb->width;
	if (fb != old_state->fb) {
		WREG8(SEQ_INDEX, 0x12);
		sr12 = RREG8(SEQ_DATA);
		sr12 &= 0xfe;
		WREG_SEQ(0x12, sr12);

		/* This may still fail if the bo reservation fails. */
		ret = cirrus_bo_to_cursor(cdev, fb, width, cursor_index);
		if (ret)
			return;

		WREG8(SEQ_INDEX, 0x12);
		sr12 = RREG8(SEQ_DATA);
		sr12 &= 0xfa;
		sr12 |= 0x03; /* enables cursor and write to extra DAC LUT */
		if (width == 64)
			sr12 |= 0x04;
		WREG_SEQ(0x12, sr12);

		/* Background set to black, foreground set to white */
		WREG_PAL(0x00, 0, 0, 0);
		WREG_PAL(0x0f, 255, 255, 255);

		sr12 &= ~0x2; /* Disables writes to the extra LUT */
		WREG_SEQ(0x12, sr12);

		sr13 = 0;
		if (width == 64)
			sr13 |= (cursor_index & 0x0f) << 2;
		else
			sr13 |= cursor_index & 0x3f;
		WREG_SEQ(0x13, sr13);
	}

	x = plane->state->crtc_x + fb->hot_x;
	y = plane->state->crtc_y + fb->hot_y;
	if (x < 0)
		x = 0;
	if (x > 0x7ff)
		x = 0x7ff;
	if (y < 0)
		y = 0;
	if (y > 0x7ff)
		y = 0x7ff;

	sr10 = (x >> 3) & 0xff;
	sr10_index = 0x10;
	sr10_index |= (x & 0x07) << 5;
	WREG_SEQ(sr10_index, sr10);
	sr11 = (y >> 3) & 0xff;
	sr11_index = 0x11;
	sr11_index |= (y & 0x07) << 5;
	WREG_SEQ(sr11_index, sr11);
}

void cirrus_cursor_atomic_disable(struct drm_plane *plane,
			       struct drm_plane_state *old_state)
{
	struct cirrus_device *cdev = plane->dev->dev_private;
	int sr12;

	WREG8(SEQ_INDEX, 0x12);
	sr12 = (RREG8(SEQ_DATA) | 0x04) & 0xfe;
	WREG8(SEQ_DATA, sr12);
}

static const uint32_t cirrus_plane_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_RGB565,
};

static const struct drm_plane_funcs cirrus_plane_funcs = {
	.update_plane	= drm_atomic_helper_update_plane,
	.disable_plane	= drm_primary_helper_disable,
	.destroy	= drm_primary_helper_destroy,
	.reset	= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};

static int cirrus_plane_prepare_fb(struct drm_plane *plane,
				   struct drm_plane_state *new_state)
{
	struct cirrus_device *cdev = plane->dev->dev_private;
	struct drm_gem_object *obj;
	struct cirrus_framebuffer *cirrus_fb;
	struct cirrus_bo *bo;
	int ret;

	if (!new_state->fb)
		return 0;

	if (plane->old_fb) {
		cirrus_fb = to_cirrus_framebuffer(plane->old_fb);
		obj = cirrus_fb->obj;
		bo = gem_to_cirrus_bo(obj);
		cirrus_bo_push_sysram(bo);
	}

	cirrus_fb = to_cirrus_framebuffer(new_state->fb);
	obj = cirrus_fb->obj;
	bo = gem_to_cirrus_bo(obj);

	ret = cirrus_bo_pin(bo, TTM_PL_FLAG_VRAM, &bo->gpu_addr);
	if (ret)
		return ret;

	if (&cdev->mode_info.gfbdev->gfb == cirrus_fb) {
		/* if pushing console in kmap it */
		ret = ttm_bo_kmap(&bo->bo, 0, bo->bo.num_pages, &bo->kmap);
		if (ret)
			return ret;
	}

	return 0;
}

static void cirrus_plane_cleanup_fb(struct drm_plane *plane,
				    struct drm_plane_state *old_state)
{
	struct drm_gem_object *obj;
	struct cirrus_bo *bo;

	if (!plane->state->fb) {
		/* we never executed prepare_fb, so there's nothing to
		 * unpin.
		 */
		return;
	}

	obj = to_cirrus_framebuffer(plane->state->fb)->obj;
	bo = gem_to_cirrus_bo(obj);

	cirrus_bo_unpin(bo);
}

static int cirrus_plane_atomic_check(struct drm_plane *plane,
				     struct drm_plane_state *state)
{
	struct cirrus_device *cdev = plane->dev->dev_private;
	struct drm_framebuffer *fb = state->fb;
	struct drm_crtc *crtc = state->crtc ? state->crtc : plane->crtc;
	struct drm_crtc_state *crtc_state;
	struct drm_rect clip = { 0 };
	int ret;

	if (!crtc || !fb)
		return 0;

	if (!cirrus_check_framebuffer(cdev, fb->width, fb->height,
				      fb->format->cpp[0], fb->pitches[0]))
		return -EINVAL;

	crtc_state = drm_atomic_get_existing_crtc_state(state->state, crtc);
	if (!crtc_state)
		return -EINVAL;

	clip.x2 = crtc_state->adjusted_mode.hdisplay;
	clip.y2 = crtc_state->adjusted_mode.vdisplay;

	ret = drm_plane_helper_check_state(state, &clip,
					   DRM_PLANE_HELPER_NO_SCALING,
					   DRM_PLANE_HELPER_NO_SCALING,
					   false, true);
	if (ret)
		return ret;

	return 0;
}

static void cirrus_plane_atomic_disable(struct drm_plane *plane,
					struct drm_plane_state *old_state)
{
	return;
}

static void cirrus_set_framebuffer_regs(struct drm_plane *plane)
{
	struct cirrus_device *cdev = plane->dev->dev_private;
	struct drm_framebuffer *fb = plane->state->fb;
	int sr07 = 0, hdr = 0, tmp;

	WREG8(SEQ_INDEX, 0x7);
	sr07 = RREG8(SEQ_DATA);
	sr07 &= 0xe0;
	switch (fb->format->cpp[0] * 8) {
	case 8:
		sr07 |= 0x11;
		break;
	case 16:
		sr07 |= 0x17;
		hdr = 0xc1;
		break;
	case 24:
		sr07 |= 0x15;
		hdr = 0xc5;
		break;
	case 32:
		sr07 |= 0x19;
		hdr = 0xc5;
		break;
	default:
		/* Should never reach here. */
		break;
	}

	WREG_SEQ(0x7, sr07);

	/* Program the pitch */
	tmp = fb->pitches[0] / 8;
	WREG_CRT(VGA_CRTC_OFFSET, tmp);

	/* Enable extended blanking and pitch bits, and enable full memory */
	tmp = 0x22;
	tmp |= (fb->pitches[0] >> 7) & 0x10;
	tmp |= (fb->pitches[0] >> 6) & 0x40;
	WREG_CRT(0x1b, tmp);

	WREG_HDR(hdr);
}

static void cirrus_set_start_address(struct drm_crtc *crtc, unsigned offset)
{
	struct cirrus_device *cdev = crtc->dev->dev_private;
	u32 addr;
	u8 tmp;

	addr = offset >> 2;
	WREG_CRT(0x0c, (u8)((addr >> 8) & 0xff));
	WREG_CRT(0x0d, (u8)(addr & 0xff));

	WREG8(CRT_INDEX, 0x1b);
	tmp = RREG8(CRT_DATA);
	tmp &= 0xf2;
	tmp |= (addr >> 16) & 0x01;
	tmp |= (addr >> 15) & 0x0c;
	WREG_CRT(0x1b, tmp);
	WREG8(CRT_INDEX, 0x1d);
	tmp = RREG8(CRT_DATA);
	tmp &= 0x7f;
	tmp |= (addr >> 12) & 0x80;
	WREG_CRT(0x1d, tmp);
}

static void cirrus_plane_atomic_update(struct drm_plane *plane,
				       struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = plane->state;
	struct drm_gem_object *obj;
	struct cirrus_bo *bo;

	cirrus_set_framebuffer_regs(plane);

	obj = to_cirrus_framebuffer(state->fb)->obj;
	bo = gem_to_cirrus_bo(obj);
	cirrus_set_start_address(state->crtc, (u32)bo->gpu_addr);

	/* Unblank (needed on S3 resume, vgabios doesn't do it then) */
	outb(0x20, 0x3c0);
}

static const uint32_t cirrus_cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
};

static const struct drm_plane_funcs cirrus_cursor_plane_funcs = {
	.update_plane	= drm_atomic_helper_update_plane,
	.disable_plane	= drm_atomic_helper_disable_plane,
	.destroy	= drm_primary_helper_destroy,
	.reset		= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static const struct drm_plane_helper_funcs cirrus_cursor_helper_funcs = {
	.atomic_check = cirrus_cursor_atomic_check,
	.atomic_update = cirrus_cursor_atomic_update,
	.atomic_disable = cirrus_cursor_atomic_disable,
	.prepare_fb = cirrus_plane_prepare_fb,
	.cleanup_fb = cirrus_plane_cleanup_fb,
};

static const struct drm_plane_helper_funcs cirrus_plane_helper_funcs = {
	.prepare_fb = cirrus_plane_prepare_fb,
	.cleanup_fb = cirrus_plane_cleanup_fb,
	.atomic_check = cirrus_plane_atomic_check,
	.atomic_disable = cirrus_plane_atomic_disable,
	.atomic_update = cirrus_plane_atomic_update,
};

/* CRTC setup */
static void cirrus_crtc_init(struct drm_device *dev)
{
	struct cirrus_device *cdev = dev->dev_private;
	struct cirrus_crtc *cirrus_crtc;
	struct drm_plane *primary, *cursor;
	int ret;

	cirrus_crtc = kzalloc(sizeof(struct cirrus_crtc) +
			      (CIRRUSFB_CONN_LIMIT * sizeof(struct drm_connector *)),
			      GFP_KERNEL);

	if (cirrus_crtc == NULL)
		return;

	primary = kzalloc(sizeof(*primary), GFP_KERNEL);
	if (primary == NULL)
		goto cleanup_crtc;

	drm_plane_helper_add(primary, &cirrus_plane_helper_funcs);
	ret = drm_universal_plane_init(dev, primary, 1,
				       &cirrus_plane_funcs,
				       cirrus_plane_formats,
				       ARRAY_SIZE(cirrus_plane_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret) {
		kfree(primary);
		goto cleanup_crtc;
	}

	cursor = kzalloc(sizeof(*cursor), GFP_KERNEL);
	if (cursor == NULL)
		goto cleanup_primary;

	drm_plane_helper_add(cursor, &cirrus_cursor_helper_funcs);
	ret = drm_universal_plane_init(dev, cursor, 1,
				       &cirrus_cursor_plane_funcs,
				       cirrus_cursor_formats,
				       ARRAY_SIZE(cirrus_cursor_formats),
				       NULL, DRM_PLANE_TYPE_CURSOR, NULL);
	if (ret) {
		kfree(cursor);
		goto cleanup_primary;
	}

	ret = drm_crtc_init_with_planes(dev, &cirrus_crtc->base, primary, cursor,
				      &cirrus_crtc_funcs, NULL);
	if (ret)
		goto cleanup_cursor;
	drm_mode_crtc_set_gamma_size(&cirrus_crtc->base, CIRRUS_LUT_SIZE);
	cdev->mode_info.crtc = cirrus_crtc;

	drm_crtc_helper_add(&cirrus_crtc->base, &cirrus_helper_funcs);
	return;

cleanup_cursor:
	drm_plane_cleanup(cursor);
	kfree(cursor);
cleanup_primary:
	drm_plane_cleanup(primary);
	kfree(primary);
cleanup_crtc:
	kfree(cirrus_crtc);
	return;
}

static void cirrus_encoder_mode_set(struct drm_encoder *encoder,
				struct drm_display_mode *mode,
				struct drm_display_mode *adjusted_mode)
{
}

static void cirrus_encoder_dpms(struct drm_encoder *encoder, int state)
{
	return;
}

static void cirrus_encoder_prepare(struct drm_encoder *encoder)
{
}

static void cirrus_encoder_commit(struct drm_encoder *encoder)
{
}

static void cirrus_encoder_destroy(struct drm_encoder *encoder)
{
	struct cirrus_encoder *cirrus_encoder = to_cirrus_encoder(encoder);
	drm_encoder_cleanup(encoder);
	kfree(cirrus_encoder);
}

static const struct drm_encoder_helper_funcs cirrus_encoder_helper_funcs = {
	.dpms = cirrus_encoder_dpms,
	.mode_set = cirrus_encoder_mode_set,
	.prepare = cirrus_encoder_prepare,
	.commit = cirrus_encoder_commit,
};

static const struct drm_encoder_funcs cirrus_encoder_encoder_funcs = {
	.destroy = cirrus_encoder_destroy,
};

static struct drm_encoder *cirrus_encoder_init(struct drm_device *dev)
{
	struct drm_encoder *encoder;
	struct cirrus_encoder *cirrus_encoder;

	cirrus_encoder = kzalloc(sizeof(struct cirrus_encoder), GFP_KERNEL);
	if (!cirrus_encoder)
		return NULL;

	encoder = &cirrus_encoder->base;
	encoder->possible_crtcs = 0x1;

	drm_encoder_init(dev, encoder, &cirrus_encoder_encoder_funcs,
			 DRM_MODE_ENCODER_DAC, NULL);
	drm_encoder_helper_add(encoder, &cirrus_encoder_helper_funcs);

	return encoder;
}


static int cirrus_vga_get_modes(struct drm_connector *connector)
{
	int count;

	/* Just add a static list of modes */
	if (cirrus_bpp <= 24) {
		count = drm_add_modes_noedid(connector, 1280, 1024);
		drm_set_preferred_mode(connector, 1024, 768);
	} else {
		count = drm_add_modes_noedid(connector, 800, 600);
		drm_set_preferred_mode(connector, 800, 600);
	}
	return count;
}

static struct drm_encoder *cirrus_connector_best_encoder(struct drm_connector
						  *connector)
{
	int enc_id = connector->encoder_ids[0];
	/* pick the encoder ids */
	if (enc_id)
		return drm_encoder_find(connector->dev, enc_id);
	return NULL;
}

static void cirrus_connector_destroy(struct drm_connector *connector)
{
	drm_connector_cleanup(connector);
	kfree(connector);
}

static const struct drm_connector_helper_funcs cirrus_vga_connector_helper_funcs = {
	.get_modes = cirrus_vga_get_modes,
	.best_encoder = cirrus_connector_best_encoder,
};

static const struct drm_connector_funcs cirrus_vga_connector_funcs = {
	.dpms = drm_helper_connector_dpms,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = cirrus_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static struct drm_connector *cirrus_vga_init(struct drm_device *dev)
{
	struct drm_connector *connector;
	struct cirrus_connector *cirrus_connector;

	cirrus_connector = kzalloc(sizeof(struct cirrus_connector), GFP_KERNEL);
	if (!cirrus_connector)
		return NULL;

	connector = &cirrus_connector->base;

	drm_connector_init(dev, connector,
			   &cirrus_vga_connector_funcs, DRM_MODE_CONNECTOR_VGA);

	drm_connector_helper_add(connector, &cirrus_vga_connector_helper_funcs);

	drm_connector_register(connector);
	return connector;
}


int cirrus_modeset_init(struct cirrus_device *cdev)
{
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	int ret;

	drm_mode_config_init(cdev->dev);
	cdev->mode_info.mode_config_initialized = true;

	cdev->dev->mode_config.max_width = CIRRUS_MAX_FB_WIDTH;
	cdev->dev->mode_config.max_height = CIRRUS_MAX_FB_HEIGHT;

	cdev->dev->mode_config.fb_base = cdev->mc.vram_base;
	cdev->dev->mode_config.preferred_depth = 24;
	/* don't prefer a shadow on virt GPU */
	cdev->dev->mode_config.prefer_shadow = 0;

	cirrus_crtc_init(cdev->dev);

	encoder = cirrus_encoder_init(cdev->dev);
	if (!encoder) {
		DRM_ERROR("cirrus_encoder_init failed\n");
		return -1;
	}

	connector = cirrus_vga_init(cdev->dev);
	if (!connector) {
		DRM_ERROR("cirrus_vga_init failed\n");
		return -1;
	}

	drm_mode_connector_attach_encoder(connector, encoder);

	drm_mode_config_reset(cdev->dev);

	ret = cirrus_fbdev_init(cdev);
	if (ret) {
		DRM_ERROR("cirrus_fbdev_init failed\n");
		return ret;
	}

	return 0;
}

void cirrus_modeset_fini(struct cirrus_device *cdev)
{
	cirrus_fbdev_fini(cdev);

	if (cdev->mode_info.mode_config_initialized) {
		drm_mode_config_cleanup(cdev->dev);
		cdev->mode_info.mode_config_initialized = false;
	}
}
