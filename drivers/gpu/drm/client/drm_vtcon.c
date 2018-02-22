// SPDX-License-Identifier: GPL-2.0
// Copyright 2018 Noralf Trønnes

#include <linux/console.h>
#include <linux/font.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vt_buffer.h>
#include <linux/vt_kern.h>
#include <linux/workqueue.h>

#include <drm/drm_client.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_print.h>

/* TODO Need a way to unbind */

/* TODO: Scrolling */

/*
 * The code consists of 3 parts:
 *
 * 1. The DRM client
 *    Gets a display, uses the first mode to find a font,
 *    sets the max cols/rows and a matching text buffer.
 *
 * 2. The VT console
 *    Writes to the text buffer which consists of CGA colored characters.
 *    Schedules the worker when it needs rendering or blanking.
 *
 * 3. Worker
 *    Does modesetting, blanking and rendering.
 *    It takes a snapshot of the VT text buffer and renders the changes since
 *    last.
 */

struct drm_vtcon_vc {
	struct mutex lock;

	u16 *text_buf;
	size_t buf_len;

	unsigned int rows;
	unsigned int cols;
	unsigned int max_rows;
	unsigned int max_cols;
	const struct font_desc *font;
	bool blank;
	unsigned long cursor_blink_jiffies;
};

static struct drm_vtcon_vc *drm_vtcon_vc;

struct drm_vtcon {
	struct drm_client_dev *client;
	struct drm_client_display *display;
	struct drm_client_buffer *buffer;

	unsigned int rows;
	unsigned int cols;

	u16 *text_buf[2];
	size_t buf_len;
	unsigned int buf_idx;
	bool blank;
};

static struct drm_vtcon *vtcon_instance;

#define drm_vtcon_debug(vc, fmt, ...) \
	printk(KERN_DEBUG "%s[%u]: " fmt, __func__, vc->vc_num, ##__VA_ARGS__)

/* CGA color palette: 4-bit RGBI: intense red green blue */
static const u32 drm_vtcon_paletteX888[16] = {
	0x00000000, /*  0 black */
	0x000000aa, /*  1 blue */
	0x0000aa00, /*  2 green */
	0x0000aaaa, /*  3 cyan */
	0x00aa0000, /*  4 red */
	0x00aa00aa, /*  5 magenta */
	0x00aa5500, /*  6 brown */
	0x00aaaaaa, /*  7 light gray */
	0x00555555, /*  8 dark gray */
	0x005555ff, /*  9 bright blue */
	0x0055ff55, /* 10 bright green */
	0x0055ffff, /* 11 bright cyan */
	0x00ff5555, /* 12 bright red */
	0x00ff55ff, /* 13 bright magenta */
	0x00ffff55, /* 14 yellow */
	0x00ffffff  /* 15 white */
};

static void
drm_vtcon_render_char(struct drm_vtcon *vtcon, unsigned int x, unsigned int y,
		      u16 cc, const struct font_desc *font)
{
	struct drm_client_buffer *buffer = vtcon->buffer;
	unsigned int h, w;
	const u8 *src;
	void *dst;
	u32 *pix;
	u32 fg_col = drm_vtcon_paletteX888[(cc & 0x0f00) >> 8];
	u32 bg_col = drm_vtcon_paletteX888[cc >> 12];

	src = font->data + (cc & 0xff) * font->height;
	dst = vtcon->buffer->vaddr + y * buffer->pitch + x * sizeof(u32);

	for (h = 0; h < font->height; h++) {
		u8 fontline = *(src + h);

		pix = dst;
		for (w = 0; w < font->width; w++)
			pix[w] = fontline & BIT(7 - w) ? fg_col : bg_col;
		dst += buffer->pitch;
	}
}

static int drm_vtcon_modeset(struct drm_vtcon *vtcon, unsigned int cols,
			     unsigned int rows, const struct font_desc *font)
{
	struct drm_client_display *display = vtcon->display;
	struct drm_client_dev *client = vtcon->client;
	struct drm_mode_modeinfo use_mode, *mode, *best_mode = NULL;
	unsigned int best_cols = ~0, best_rows = ~0;
	struct drm_client_buffer *buffer;
	int ret;

	DRM_DEV_INFO(client->dev->dev, "IN %ux%u\n", cols, rows);

	/* Find the smallest mode that fits */
	mutex_lock(&display->modes_lock);
	drm_client_display_for_each_mode(display, mode) {
		unsigned int mode_cols = mode->hdisplay / font->width;
		unsigned int mode_rows = mode->vdisplay / font->height;

		DRM_DEV_INFO(client->dev->dev,
			     "try: %ux%u\n", mode_cols, mode_rows);

		if (mode_cols < cols || mode_rows < rows)
			break;

		if (mode_cols >= best_cols || mode_rows >= best_rows)
			continue;

		best_cols = mode_cols;
		best_rows = mode_rows;
		best_mode = mode;
	}
	if (best_mode)
		use_mode = *best_mode;
	mutex_unlock(&display->modes_lock);

	if (!best_mode) {
		DRM_DEV_ERROR(client->dev->dev,
			      "Couldn't find mode for %ux%u\n", cols, rows);
		return -EINVAL;
	}

	DRM_DEV_INFO(client->dev->dev,
		     "Chosen: %ux%u\n", best_cols, best_rows);

	buffer = drm_client_framebuffer_create(client, &use_mode,
					       DRM_FORMAT_XRGB8888);
	DRM_DEV_INFO(client->dev->dev, "buffer=%p\n", buffer);
	if (IS_ERR(buffer)) {
		DRM_DEV_ERROR(client->dev->dev,
			      "Failed to create framebuffer: %d\n", ret);
		return PTR_ERR(buffer);
	}

	ret = drm_client_display_commit_mode(display, buffer->fb_ids[0],
					     &use_mode);
	DRM_DEV_INFO(client->dev->dev, "commit ret=%d\n", ret);
	if (ret) {
		DRM_DEV_ERROR(client->dev->dev,
			      "Failed to commit mode: %d\n", ret);
		goto err_free_buffer;
	}

	drm_client_framebuffer_delete(vtcon->buffer);

	vtcon->buffer = buffer;
	vtcon->cols = cols;
	vtcon->rows = rows;

	DRM_DEV_INFO(client->dev->dev, "OUT\n");
	return 0;

err_free_buffer:
	drm_client_framebuffer_delete(buffer);

	DRM_DEV_INFO(client->dev->dev, "OUT ret=%d\n", ret);
	return ret;
}

static void drm_vtcon_blank(struct drm_vtcon *vtcon, bool blank)
{
	int ret;

	if (blank)
		ret = drm_client_display_dpms(vtcon->display, DRM_MODE_DPMS_OFF);
	else
		ret = drm_client_display_dpms(vtcon->display, DRM_MODE_DPMS_ON);
	if (ret)
		DRM_DEBUG_KMS("Error %sblanking display: %d\n",
			      blank ? "" : "un", ret);

	vtcon->blank = blank;
}

static int drm_vtcon_resize_buf(struct drm_vtcon *vtcon, size_t len)
{
	u16 *text_buf[2];

	text_buf[0] = kzalloc(len, GFP_KERNEL);
	text_buf[1] = kzalloc(len, GFP_KERNEL);
	if (!text_buf[0] || !text_buf[1]) {
		kfree(text_buf[0]);
		kfree(text_buf[1]);
		return -ENOMEM;
	}

	kfree(vtcon->text_buf[0]);
	kfree(vtcon->text_buf[1]);

	vtcon->text_buf[0] = text_buf[0];
	vtcon->text_buf[1] = text_buf[1];
	vtcon->buf_len = len;

	return 0;
}

static void drm_vtcon_work_fn(struct work_struct *work)
{
	struct drm_vtcon *vtcon = vtcon_instance;
	unsigned int vc_cols, vc_rows, col, row, x, y;
	const struct font_desc *font;
	u16 prev, curr;
	bool blank, render_all = false;
	struct drm_clip_rect clip = {
		.x1 = ~0,
		.y1 = ~0,
		.x2 = 0,
		.y2 = 0,
	};
	char *str;
	int ret;

	if (!vtcon->display)
		return;

	mutex_lock(&drm_vtcon_vc->lock);

	vc_cols = drm_vtcon_vc->cols;
	vc_rows = drm_vtcon_vc->rows;
	font = drm_vtcon_vc->font;
	blank = drm_vtcon_vc->blank;

	if (vtcon->buf_len != drm_vtcon_vc->buf_len) {
		ret = drm_vtcon_resize_buf(vtcon, drm_vtcon_vc->buf_len);
		if (ret) {
			mutex_unlock(&drm_vtcon_vc->lock);
			return;
		}
		render_all = true;
	}

	vtcon->buf_idx = !vtcon->buf_idx;
	memcpy(vtcon->text_buf[vtcon->buf_idx], drm_vtcon_vc->text_buf,
	       vc_cols * vc_rows * sizeof(u16));

	mutex_unlock(&drm_vtcon_vc->lock);

	if (vtcon->cols != vc_cols || vtcon->rows != vc_rows) {
		ret = drm_vtcon_modeset(vtcon, vc_cols, vc_rows, font);
		if (ret)
			return;
		render_all = true;
	} else if (vtcon->blank != blank) {
		drm_vtcon_blank(vtcon, blank);
	}

	str = kmalloc(vc_cols + 1, GFP_KERNEL);
	if (!str)
		return;

	for (row = 0; row < vc_rows; row++) {
		for (col = 0; col < vc_cols; col++) {
			prev = vtcon->text_buf[!vtcon->buf_idx][col + (row * vc_cols)];
			curr = vtcon->text_buf[vtcon->buf_idx][col + (row * vc_cols)];

			if (render_all || prev != curr) {
				str[col] = curr;
				x = col * font->width;
				y = row * font->height;

				clip.x1 = min_t(u32, clip.x1, x);
				clip.y1 = min_t(u32, clip.y1, y);
				clip.x2 = max_t(u32, clip.x2, x + font->width);
				clip.y2 = max_t(u32, clip.y2, y + font->height);

				drm_vtcon_render_char(vtcon, x, y, curr, font);
			} else {
				str[col] = ' ';
			}
		}
		str[vc_cols] = '\0';
//		pr_info("%02u|%s\n", row, str);
	}

	kfree(str);

	if (clip.x1 < clip.x2)
		drm_client_display_flush(vtcon->display, vtcon->buffer->fb_ids[0],
					 &clip, 1);
}

static DECLARE_WORK(drm_vtcon_work, drm_vtcon_work_fn);

static const char *drm_vtcon_con_startup(void)
{
	return "drm-vt";
}

static void drm_vtcon_con_init(struct vc_data *vc, int init)
{
	drm_vtcon_debug(vc, "(init=%d) drm_vtcon_vc=%p\n", init, drm_vtcon_vc);

	vc->vc_can_do_color = 1;

	if (init) {
		vc->vc_cols = drm_vtcon_vc->cols;
		vc->vc_rows = drm_vtcon_vc->rows;
	} else {
		vc_resize(vc, drm_vtcon_vc->cols, drm_vtcon_vc->rows);
	}
}

static void drm_vtcon_con_deinit(struct vc_data *vc)
{
	drm_vtcon_debug(vc, "\n");
}

static void drm_vtcon_con_putcs(struct vc_data *vc, const unsigned short *s,
				int count, int y, int x)
{
	u16 *dest;

	dest = &drm_vtcon_vc->text_buf[x + (y * drm_vtcon_vc->cols)];

	for (; count > 0; count--)
		scr_writew(scr_readw(s++), dest++);

	schedule_work(&drm_vtcon_work);
}

static void drm_vtcon_con_putc(struct vc_data *vc, int ch, int y, int x)
{
	unsigned short chr;

	scr_writew(ch, &chr);
	drm_vtcon_con_putcs(vc, &chr, 1, y, x);
}

/* TODO: How do I actually test this? */
static void drm_vtcon_con_clear(struct vc_data *vc, int y, int x,
				int height, int width)
{
	drm_vtcon_debug(vc, "\n");

	scr_memcpyw(drm_vtcon_vc->text_buf, (unsigned short *)vc->vc_pos,
		    vc->vc_cols * vc->vc_rows);
	schedule_work(&drm_vtcon_work);
}

static int drm_vtcon_con_switch(struct vc_data *vc)
{
	drm_vtcon_debug(vc, "%ux%u\n", vc->vc_cols, vc->vc_rows);

	mutex_lock(&drm_vtcon_vc->lock);
	drm_vtcon_vc->cols = vc->vc_cols;
	drm_vtcon_vc->rows = vc->vc_rows;
	mutex_unlock(&drm_vtcon_vc->lock);

	return 1; /* redraw */
}

static int drm_vtcon_con_resize(struct vc_data *vc, unsigned int width,
				unsigned int height, unsigned int user)
{
	int ret = 0;

	drm_vtcon_debug(vc, "width=%u, height=%u, user=%u\n",
			width, height, user);

	mutex_lock(&drm_vtcon_vc->lock);

	if (width > drm_vtcon_vc->max_cols || height > drm_vtcon_vc->max_rows)
		ret = -EINVAL;

	mutex_unlock(&drm_vtcon_vc->lock);

	drm_vtcon_debug(vc, "ret=%d\n", ret);

	return ret;
}

static void drm_vtcon_con_set_palette(struct vc_data *vc,
				      const unsigned char *table)
{
	drm_vtcon_debug(vc, "\n");
}

static int drm_vtcon_con_blank(struct vc_data *vc, int blank, int mode_switch)
{
	drm_vtcon_debug(vc, "(blank=%d, mode_switch=%d)\n", blank, mode_switch);

	mutex_lock(&drm_vtcon_vc->lock);
	drm_vtcon_vc->blank = blank;
	mutex_unlock(&drm_vtcon_vc->lock);

	schedule_work(&drm_vtcon_work);

	return 0;
}

static void drm_vtcon_con_scrolldelta(struct vc_data *vc, int lines)
{
	drm_vtcon_debug(vc, "(lines=%d)\n", lines);
}

static void drm_vtcon_con_cursor_draw(bool show)
{
	static unsigned short set_chr, saved_chr;
	static int prev_x, prev_y;
	struct vc_data *vc = vc_cons[fg_console].d;
	unsigned short *pos;

	if (saved_chr) {
		pos = &drm_vtcon_vc->text_buf[prev_x + (prev_y * drm_vtcon_vc->cols)];
		if (*pos == set_chr)
			*pos = saved_chr;
		saved_chr = 0;
	}

	if (show) {
		pos = &drm_vtcon_vc->text_buf[vc->vc_x + (vc->vc_y * drm_vtcon_vc->cols)];
		*pos = scr_readw((u16 *)vc->vc_pos);
		saved_chr = *pos;
		set_chr = *pos & 0xff00;
		set_chr |= '_';
		*pos = set_chr;
		prev_x = vc->vc_x;
		prev_y = vc->vc_y;
	}

	schedule_work(&drm_vtcon_work);
}

static void drm_vtcon_con_cursor_timer_handler(struct timer_list *t)
{
	static bool show;

	show = !show;
	drm_vtcon_con_cursor_draw(show);
	mod_timer(t, jiffies + drm_vtcon_vc->cursor_blink_jiffies);
}
DEFINE_TIMER(drm_vtcon_con_cursor_timer, drm_vtcon_con_cursor_timer_handler);

static void drm_vtcon_con_cursor(struct vc_data *vc, int mode)
{
	//drm_vtcon_debug(vc, "(mode=%d)\n", mode);

	switch (mode) {
	case CM_ERASE:
		drm_vtcon_con_cursor_draw(false);
		del_timer_sync(&drm_vtcon_con_cursor_timer);
		break;
	case CM_MOVE:
	case CM_DRAW:
		drm_vtcon_vc->cursor_blink_jiffies = msecs_to_jiffies(vc->vc_cur_blink_ms);
		mod_timer(&drm_vtcon_con_cursor_timer,
			  jiffies + drm_vtcon_vc->cursor_blink_jiffies);
		break;
	}
}

static bool drm_vtcon_con_scroll(struct vc_data *vc, unsigned int top,
				 unsigned int bottom, enum con_scroll dir,
				 unsigned int lines)
{
	size_t count;

	switch (dir) {
	case SM_UP:
		count = vc->vc_cols * (vc->vc_rows - lines);
		memmove(drm_vtcon_vc->text_buf, drm_vtcon_vc->text_buf + vc->vc_cols, count * sizeof(u16));
		memset(drm_vtcon_vc->text_buf + count, 0, vc->vc_cols * lines * sizeof(u16));
		break;
	case SM_DOWN:
		drm_vtcon_debug(vc, "TODO\n");
		break;
	}

	return false;
}

static const struct consw drm_vtcon_consw = {
	.owner			= THIS_MODULE,
	.con_startup		= drm_vtcon_con_startup,
	.con_init		= drm_vtcon_con_init,
	.con_deinit		= drm_vtcon_con_deinit,
	.con_clear		= drm_vtcon_con_clear,
	.con_putc		= drm_vtcon_con_putc,
	.con_putcs		= drm_vtcon_con_putcs,
	.con_cursor		= drm_vtcon_con_cursor,
	.con_scroll		= drm_vtcon_con_scroll,
	.con_switch		= drm_vtcon_con_switch,
	.con_blank		= drm_vtcon_con_blank,
	.con_resize		= drm_vtcon_con_resize,
	.con_set_palette	= drm_vtcon_con_set_palette,
	.con_scrolldelta	= drm_vtcon_con_scrolldelta,
};

static int drm_vtcon_vc_set_max(struct drm_mode_modeinfo *mode)
{
	u16 *new_buf = NULL, *old_buf = NULL;
	const struct font_desc *font;
	unsigned int cols, rows, i;
	size_t buf_len;

	/* only 8 bit wide and 8 or 16-bit high */
	font = get_default_font(mode->hdisplay, mode->vdisplay,
				BIT(8 - 1), BIT(8 - 1) | BIT(16 - 1));
	if (!font)
		return -ENODEV;

	DRM_INFO("font: %s\n", font->name);

	cols = mode->hdisplay / font->width;
	rows = mode->vdisplay / font->height;

	DRM_INFO("ASKED: cols=%zu, rows=%zu\n", cols, rows);

	if (drm_vtcon_vc->max_cols == cols && drm_vtcon_vc->max_rows == rows &&
	    drm_vtcon_vc->font == font)
		return 0;

	buf_len = cols * rows * sizeof(u16);
	if (buf_len > drm_vtcon_vc->buf_len) {
		new_buf = kzalloc(buf_len, GFP_KERNEL);
		if (!new_buf)
			return -ENOMEM;
	}

	mutex_lock(&drm_vtcon_vc->lock);

	if (new_buf) {
		DRM_INFO("Allocated new buf: buf_len=%zu\n", buf_len);
		old_buf = drm_vtcon_vc->text_buf;
		drm_vtcon_vc->text_buf = new_buf;
		drm_vtcon_vc->buf_len = buf_len;
	}

	drm_vtcon_vc->max_cols = cols;
	drm_vtcon_vc->max_rows = rows;
	drm_vtcon_vc->font = font;

	mutex_unlock(&drm_vtcon_vc->lock);

	DRM_INFO("max_cols=%u, max_rows=%u\n", drm_vtcon_vc->max_cols,
		 drm_vtcon_vc->max_rows);

	console_lock();
	for (i = 0; i < 2; i++)
		vc_resize(vc_cons[i].d, drm_vtcon_vc->max_cols,
			  drm_vtcon_vc->max_rows);
	console_unlock();

	kfree(old_buf);

	return 0;
}

static int drm_vtcon_setup_dev(struct drm_vtcon *vtcon)
{
	struct drm_client_dev *client = vtcon->client;
	struct drm_client_display *display;
	struct drm_mode_modeinfo *mode;
	int ret;

	display = drm_client_display_get_first_enabled(client, false);
	if (IS_ERR(display))
		return PTR_ERR(display);
	if (!display)
		return -ENOENT;

	mode = drm_client_display_first_mode(display);
	if (!mode) {
		ret = -EINVAL;
		goto err_free_display;
	}

	ret = drm_vtcon_vc_set_max(mode);
	if (ret)
		goto err_free_display;

	DRM_INFO("cols=%zu, rows=%zu\n", drm_vtcon_vc->cols, drm_vtcon_vc->rows);

	vtcon->display = display;

	schedule_work(&drm_vtcon_work);

	return 0;

err_free_display:
	drm_client_display_free(display);

	return ret;
}

static int drm_vtcon_client_hotplug(struct drm_client_dev *client)
{
	struct drm_vtcon *vtcon = client->private;
	int ret = 0;

	if (!vtcon->display)
		ret = drm_vtcon_setup_dev(vtcon);

	return ret;
}

static int drm_vtcon_client_new(struct drm_client_dev *client)
{
	struct drm_vtcon *vtcon = vtcon_instance;

	if (vtcon->client) {
		DRM_DEV_INFO(client->dev->dev, "Console is taken\n");
		return -EBUSY;
	}

	vtcon->client = client;
	client->private = vtcon;

	/*
	 * vc4 isn't done with it's setup when drm_dev_register() is called.
	 * It should have shouldn't it?
	 * So to keep it from crashing defer setup to hotplug...
	 */
	if (client->dev->mode_config.max_width)
		drm_vtcon_client_hotplug(client);

	return 0;
}

static int drm_vtcon_client_remove(struct drm_client_dev *client)
{
	struct drm_vtcon *vtcon = client->private;

	if (vtcon->display) {
		flush_work(&drm_vtcon_work);
		kfree(vtcon->text_buf[0]);
		kfree(vtcon->text_buf[1]);
		drm_client_framebuffer_delete(vtcon->buffer);
		drm_client_display_free(vtcon->display);
	}

	return 0;
}

static const struct drm_client_funcs drm_vtcon_client_funcs = {
	.name		= "drm_vtcon",
	.new		= drm_vtcon_client_new,
	.remove		= drm_vtcon_client_remove,
	.hotplug	= drm_vtcon_client_hotplug,
};

static void drm_vtcon_teardown(void)
{
	struct drm_vtcon *vtcon = vtcon_instance;

	if (!drm_vtcon_vc)
		return;

	kfree(drm_vtcon_vc->text_buf);
	mutex_destroy(&drm_vtcon_vc->lock);
	kfree(drm_vtcon_vc);
	kfree(vtcon);
}

static int __init drm_vtcon_setup(void)
{
	struct drm_vtcon *vtcon;

	drm_vtcon_vc = kzalloc(sizeof(*drm_vtcon_vc), GFP_KERNEL);
	vtcon = kzalloc(sizeof(*vtcon), GFP_KERNEL);
	if (!drm_vtcon_vc || !vtcon)
		goto err_free;

	drm_vtcon_vc->cols = drm_vtcon_vc->max_cols = 80;
	drm_vtcon_vc->rows = drm_vtcon_vc->max_rows = 25;
	DRM_INFO("cols=%zu, rows=%zu\n", drm_vtcon_vc->cols, drm_vtcon_vc->rows);

	drm_vtcon_vc->buf_len = drm_vtcon_vc->max_cols * drm_vtcon_vc->max_rows * sizeof(u16);
	drm_vtcon_vc->text_buf = kzalloc(drm_vtcon_vc->buf_len, GFP_KERNEL);
	if (!drm_vtcon_vc->text_buf)
		goto err_free;

	mutex_init(&drm_vtcon_vc->lock);
	vtcon_instance = vtcon;

	return 0;

err_free:
	kfree(drm_vtcon_vc);
	kfree(vtcon);

	return -ENOMEM;
}

static int __init drm_vtcon_module_init(void)
{
	int ret;

	ret = drm_vtcon_setup();
	if (ret)
		return ret;

	console_lock();
	ret = do_take_over_console(&drm_vtcon_consw, 0, 1, 0);
	console_unlock();
	if (ret)
		goto err_free;

	ret = drm_client_register(&drm_vtcon_client_funcs);
	if (ret)
		goto err_give_up;

	return 0;

err_give_up:
	give_up_console(&drm_vtcon_consw);
err_free:
	drm_vtcon_teardown();

	return ret;
}
module_init(drm_vtcon_module_init);

static void __exit drm_vtcon_module_exit(void)
{
	drm_client_unregister(&drm_vtcon_client_funcs);
	give_up_console(&drm_vtcon_consw);
	drm_vtcon_teardown();
}
module_exit(drm_vtcon_module_exit);

MODULE_LICENSE("GPL");
