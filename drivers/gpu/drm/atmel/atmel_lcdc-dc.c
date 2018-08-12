// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Sam Ravnborg
 *
 * The driver is based on atmel_lcdfb which is:
 * Copyright (C) 2007 Atmel Corporation
 *
 * Atmel LCD Controller Display Controller.
 * A sub-device of the Atmel LCDC IP.
 *
 * The Atmel LCD Controller supports in the following configuration:
 * - TFT only, with BGR565, 8 bits/pixel
 * - Resolution up to 2048x2048
 * - Single plane, crtc, one fixed output
 *
 * Features not (yet) ported from atmel_lcdfb:
 * - Support for extra modes (and configurable intensify bit)
 * - Check modesetting support - lcdc_dc_display_check()
 * - set color / palette handling
 * - support for STN displays (partly implemented)
 * - AVR32 support (relevant?)
 */

#include <linux/regulator/consumer.h>
#include <linux/mfd/atmel-lcdc.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/irqreturn.h>
#include <linux/device.h>
#include <linux/regmap.h>

#include <video/videomode.h>

#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_vblank.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>
#include <drm/drm_drv.h>
#include <drm/drm_of.h>

/* Parameters */
#define ATMEL_LCDC_DMA_BURST_LEN	8	/* words */

/**
 * struct atmel_lcdc_dc_desc - CPU specific configuration properties
 */
struct atmel_lcdc_dc_desc {
	int guard_time;
	int fifo_size;
	int min_width;
	int min_height;
	int max_width;
	int max_height;
	bool have_hozval;
	bool have_alt_pixclock;
};

/* private data */
struct lcdc_dc {
	const struct atmel_lcdc_dc_desc *desc;
	struct atmel_mfd_lcdc *mfd_lcdc;
	struct regulator *lcd_supply;
	struct drm_fbdev_cma *fbdev;
	struct drm_bridge *bridge;
	struct drm_panel *panel;
	struct regmap *regmap;
	struct device *dev;

	struct drm_simple_display_pipe pipe;
	struct work_struct reset_lcdc_work;
	struct drm_connector connector;
};

/* Configuration of individual CPU's */
static const struct atmel_lcdc_dc_desc atmel_lcdc_dc_at91sam9261 = {
	.guard_time = 1,
	.fifo_size = 512,
	.min_width = 0,
	.min_height = 0,
	.max_width = 2048,
	.max_height = 2048,
	.have_hozval = true,
	.have_alt_pixclock = false,
};

static const struct atmel_lcdc_dc_desc atmel_lcdc_dc_at91sam9263 = {
	.guard_time = 1,
	.fifo_size = 2048,
	.min_width = 0,
	.min_height = 0,
	.max_width = 2048,
	.max_height = 2048,
	.have_hozval = false,
	.have_alt_pixclock = false,
};

static const struct atmel_lcdc_dc_desc atmel_lcdc_dc_at91sam9g10 = {
	.guard_time = 1,
	.fifo_size = 512,
	.min_width = 0,
	.min_height = 0,
	.max_width = 2048,
	.max_height = 2048,
	.have_hozval = true,
	.have_alt_pixclock = false,
};

static const struct atmel_lcdc_dc_desc atmel_lcdc_dc_at91sam9g45 = {
	.guard_time = 1,
	.fifo_size = 512,
	.min_width = 0,
	.min_height = 0,
	.max_width = 2048,
	.max_height = 2048,
	.have_hozval = false,
	.have_alt_pixclock = true,
};

static const struct atmel_lcdc_dc_desc atmel_lcdc_dc_at91sam9g46 = {
	.guard_time = 1,
	.fifo_size = 512,
	.min_width = 0,
	.min_height = 0,
	.max_width = 2048,
	.max_height = 2048,
	.have_hozval = false,
	.have_alt_pixclock = false,
};

static const struct atmel_lcdc_dc_desc atmel_lcdc_dc_at91sam9m10 = {
	.guard_time = 1,
	.fifo_size = 512,
	.min_width = 0,
	.min_height = 0,
	.max_width = 2048,
	.max_height = 2048,
	.have_hozval = false,
	.have_alt_pixclock = false,
};

static const struct atmel_lcdc_dc_desc atmel_lcdc_dc_at91sam9m11 = {
	.guard_time = 1,
	.fifo_size = 512,
	.min_width = 0,
	.min_height = 0,
	.max_width = 2048,
	.max_height = 2048,
	.have_hozval = false,
	.have_alt_pixclock = false,
};

static const struct atmel_lcdc_dc_desc atmel_lcdc_dc_at91sam9rl = {
	.guard_time = 1,
	.fifo_size = 512,
	.min_width = 0,
	.min_height = 0,
	.max_width = 2048,
	.max_height = 2048,
	.have_hozval = false,
	.have_alt_pixclock = false,
};

static const struct of_device_id atmel_lcdc_of_match[] = {
	{
		.compatible = "atmel,at91sam9261-lcdc-mfd",
		.data = &atmel_lcdc_dc_at91sam9261,
	}, {
		.compatible = "atmel,at91sam9263-lcdc-mfd",
		.data = &atmel_lcdc_dc_at91sam9263,
	}, {
		.compatible = "atmel,at91sam9g10-lcdc-mfd",
		.data = &atmel_lcdc_dc_at91sam9g10,
	}, {
		.compatible = "atmel,at91sam9g45-lcdc-mfd",
		.data = &atmel_lcdc_dc_at91sam9g45,
	}, {
		.compatible = "atmel,at91sam9g46-lcdc-mfd",
		.data = &atmel_lcdc_dc_at91sam9g46,
	}, {
		.compatible = "atmel,at91sam9m10-lcdc-mfd",
		.data = &atmel_lcdc_dc_at91sam9m10,
	}, {
		.compatible = "atmel,at91sam9m11-lcdc-mfd",
		.data = &atmel_lcdc_dc_at91sam9m11,
	}, {
		.compatible = "atmel,at91sam9rl-lcdc-mfd",
		.data = &atmel_lcdc_dc_at91sam9rl,
	},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, atmel_lcdc_of_match);

/*
 * The Atmel LCD controller display-controller supports several formats but
 * this driver supports only a small subset.
 * TODO: atmel_lcdfb supports more - port it over
 * Maybe actual wiring will impact mode support?
 */
static const u32 lcdc_dc_formats[] = {
	DRM_FORMAT_BGR565,
};

/* Start LCD Controller (DMA + PWR) */
static void lcdc_dc_start(struct lcdc_dc *lcdc_dc)
{
	// Enable DMA
	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_DMACON, ATMEL_LCDC_DMAEN);
	// Enable LCD
	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_PWRCON,
		     (lcdc_dc->desc->guard_time << ATMEL_LCDC_GUARDT_OFFSET)
		     | ATMEL_LCDC_PWR);
}

/* Stop LCD Controller (PWR + DMA) */
static void lcdc_dc_stop(struct lcdc_dc *lcdc_dc)
{
	unsigned int pwrcon;

	might_sleep();

	/* Turn off the LCD controller and the DMA controller */
	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_PWRCON,
			lcdc_dc->desc->guard_time << ATMEL_LCDC_GUARDT_OFFSET);

	/* Wait for the LCDC core to become idle */
	regmap_read_poll_timeout(lcdc_dc->regmap, ATMEL_LCDC_PWRCON, pwrcon,
				 !(pwrcon & ATMEL_LCDC_BUSY), 100, 10000);
	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_DMACON, !(ATMEL_LCDC_DMAEN));
}

static void lcdc_dc_start_clock(struct lcdc_dc *lcdc_dc)
{
	clk_prepare_enable(lcdc_dc->mfd_lcdc->bus_clk);
	clk_prepare_enable(lcdc_dc->mfd_lcdc->lcdc_clk);
}

static void lcdc_dc_stop_clock(struct lcdc_dc *lcdc_dc)
{
	clk_disable_unprepare(lcdc_dc->mfd_lcdc->bus_clk);
	clk_disable_unprepare(lcdc_dc->mfd_lcdc->lcdc_clk);
}

static int lcdc_dc_display_check(struct drm_simple_display_pipe *pipe,
				 struct drm_plane_state *pstate,
				 struct drm_crtc_state *cstate)
{
	const struct drm_display_mode *dmode;
	struct drm_framebuffer *old_fb;
	struct drm_framebuffer *fb;

	dmode = &cstate->mode;
	old_fb = pipe->plane.state->fb;
	fb = pstate->fb;

	/* Check timing? */
	/* TODO */

	return 0;
}

/* Horizontal size of LCD module - configuration dependent */
static unsigned int compute_hozval(struct lcdc_dc *lcdc_dc, unsigned int width)
{
	unsigned int valid_lcdd_data_line;
	unsigned int hoz_display_size;
	unsigned int disptype;
	unsigned int scanmode;
	unsigned int ifwidth;
	unsigned int lcdcon2;

	if (!lcdc_dc->desc->have_hozval)
		return width;

	regmap_read(lcdc_dc->regmap, ATMEL_LCDC_LCDCON2, &lcdcon2);
	disptype = lcdcon2 & ATMEL_LCDC_DISTYPE;

	if (disptype == ATMEL_LCDC_DISTYPE_TFT)
		return width;

	ifwidth = lcdcon2 & ATMEL_LCDC_IFWIDTH;
	scanmode = lcdcon2 & ATMEL_LCDC_SCANMOD;

	/*
	 * STN display
	 * Based on algorithm from datasheet calculate hozval
	 */
	if (disptype == ATMEL_LCDC_DISTYPE_STNCOLOR)
		hoz_display_size = width * 3;
	else
		hoz_display_size = width;

	switch (ifwidth) {
	case ATMEL_LCDC_IFWIDTH_4:
		valid_lcdd_data_line = 4;
		break;
	case ATMEL_LCDC_IFWIDTH_8:
		if (scanmode == ATMEL_LCDC_SCANMOD_DUAL)
			valid_lcdd_data_line = 4;
		else
			valid_lcdd_data_line = 8;
		break;
	default:
		valid_lcdd_data_line = 8;
		break;
	}

	return DIV_ROUND_UP(hoz_display_size, valid_lcdd_data_line);
}

static void set_vertical_timing(struct lcdc_dc *lcdc_dc,
				struct drm_display_mode *dmode)
{
	unsigned int tim1;
	unsigned int vfp;
	unsigned int vbp;
	unsigned int vpw;
	unsigned int vhdly;

	/* VFP: Vertical Front Porch */
	vfp = dmode->vsync_start - dmode->vdisplay;

	/* VBP: Vertical Back Porch */
	vbp = dmode->vtotal - dmode->vsync_end;

	/* VPW: Vertical Synchronization pulse width */
	vpw = dmode->vsync_end - dmode->vsync_start - 1;

	/* VHDLY: Vertical to horizontal delay */
	vhdly = 0;

	tim1 = vfp << ATMEL_LCDC_VFP_OFFSET |
	       vbp << ATMEL_LCDC_VBP_OFFSET |
	       vpw << ATMEL_LCDC_VPW_OFFSET |
	       vhdly << ATMEL_LCDC_VHDLY_OFFSET;

	DRM_DEV_DEBUG(lcdc_dc->dev, " TIM1 = %08x\n", tim1);
	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_TIM1, tim1);
}

static void set_horizontal_timing(struct lcdc_dc *lcdc_dc,
				  struct drm_display_mode *dmode)
{
	unsigned int tim2;
	unsigned int hbp;
	unsigned int hpw;
	unsigned int hfp;

	/* HBP: Horizontal Back Porch */
	hbp = dmode->htotal - dmode->hsync_end - 1;

	/* HPW: Horizontal synchronization pulse width */
	hpw = dmode->hsync_end - dmode->hsync_start - 1;

	/* HFP: Horizontal Front Porch */
	hfp = dmode->hsync_start - dmode->hdisplay - 2;

	tim2 = hbp << ATMEL_LCDC_HBP_OFFSET |
	       hpw << ATMEL_LCDC_HPW_OFFSET |
	       hfp << ATMEL_LCDC_HFP_OFFSET;

	DRM_DEV_DEBUG(lcdc_dc->dev, " TIM2 = %08x\n", tim2);
	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_TIM2, tim2);
}

static void lcdc_dc_crtc_mode_set_nofb(struct lcdc_dc *lcdc_dc)
{
	struct drm_display_mode *dmode;
	unsigned int hozval_linesz;
	unsigned int value;

	dmode = &lcdc_dc->pipe.crtc.state->adjusted_mode;

	/* Vertical & horizontal timing */
	set_vertical_timing(lcdc_dc, dmode);
	set_horizontal_timing(lcdc_dc, dmode);

	/* Horizontal value (aka line size) */
	hozval_linesz = compute_hozval(lcdc_dc, dmode->crtc_hdisplay);

	/* Display size */
	value = (hozval_linesz - 1) << ATMEL_LCDC_HOZVAL_OFFSET;
	value |= dmode->crtc_vdisplay - 1;
	DRM_DEV_DEBUG(lcdc_dc->dev, " LCDFRMCFG = %08x\n", value);
	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_LCDFRMCFG, value);

	/* FIFO Threshold: Use formula from data sheet */
	value = lcdc_dc->desc->fifo_size - (2 * ATMEL_LCDC_DMA_BURST_LEN + 3);
	DRM_DEV_DEBUG(lcdc_dc->dev, " FIFO = %08x\n", value);
	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_FIFO, value);

	/*
	 * Toggle LCD_MODE every frame
	 * Note: register not documented, this is from atmel_lcdfb
	 */
	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_MVAL, 0);
}

static void lcdc_dc_enable(struct lcdc_dc *lcdc_dc,
			   struct drm_device *drm,
			   struct drm_crtc *crtc)
{
	const struct drm_format_info *format;
	struct drm_display_mode *dmode;
	unsigned long clk_value_khz;
	unsigned int pix_factor;
	unsigned int lcdcon1;
	unsigned int lcdcon2;
	u32 bus_flags;

	dmode = &lcdc_dc->pipe.crtc.state->adjusted_mode;
	format = crtc->primary->state->fb->format;

	/* Control register 1 */

	/* Set pixel clock */
	if (lcdc_dc->desc->have_alt_pixclock)
		pix_factor = 1;
	else
		pix_factor = 2;

	clk_value_khz = clk_get_rate(lcdc_dc->mfd_lcdc->lcdc_clk) / 1000;
	lcdcon1 = DIV_ROUND_UP(clk_value_khz, dmode->clock);

	if (lcdcon1 < pix_factor) {
		DRM_DEV_INFO(lcdc_dc->dev, "Bypassing pixel clock divider\n");
		regmap_write(lcdc_dc->regmap,
			     ATMEL_LCDC_LCDCON1, ATMEL_LCDC_BYPASS);
	} else {

		lcdcon1 = (lcdcon1 / pix_factor) - 1;
		DRM_DEV_DEBUG(lcdc_dc->dev, "CLKVAL = 0x%08x\n", lcdcon1);
		regmap_write(lcdc_dc->regmap, ATMEL_LCDC_LCDCON1,
			     lcdcon1 << ATMEL_LCDC_CLKVAL_OFFSET);
		dmode->clock = clk_value_khz / (pix_factor * (lcdcon1 + 1));
		DRM_DEV_DEBUG(lcdc_dc->dev, "updated pixclk:  %u KHz\n",
					dmode->clock);
	}

	/* Control register 2 */
	/* Only TFT supported (Controller supports STN too) */
	lcdcon2 = ATMEL_LCDC_DISTYPE_TFT;

	/* scan mode (STN only) */
	if (dmode->flags & DRM_MODE_FLAG_DBLSCAN)
		lcdcon2 |= ATMEL_LCDC_SCANMOD_DUAL;
	else
		lcdcon2 |= ATMEL_LCDC_SCANMOD_SINGLE;

	/* Interface width 4 bits (STN only) */
	lcdcon2 |= ATMEL_LCDC_IFWIDTH_4;

	/* bits per pixel */
	switch (format->depth) {
	case 1:
		lcdcon2 |= ATMEL_LCDC_PIXELSIZE_1;
		break;
	case 2:
		lcdcon2 |= ATMEL_LCDC_PIXELSIZE_2;
		break;
	case 4:
		lcdcon2 |= ATMEL_LCDC_PIXELSIZE_4;
		break;
	case 8:
		lcdcon2 |= ATMEL_LCDC_PIXELSIZE_8;
		break;
	case 15:
		/* fall through */
	case 16:
		lcdcon2 |= ATMEL_LCDC_PIXELSIZE_16;
		break;
	case 24:
		lcdcon2 |= ATMEL_LCDC_PIXELSIZE_24;
		break;
	case 32:
		lcdcon2 |= ATMEL_LCDC_PIXELSIZE_32;
		break;
	default:
		DRM_DEV_ERROR(lcdc_dc->dev, "Unexpected depth (%d)",
			      format->depth);
		break;
	}

	/* Polarity normal */
	lcdcon2 |= ATMEL_LCDC_INVVD_NORMAL;

	/* vsync polarity */
	if (dmode->flags & DRM_MODE_FLAG_PVSYNC)
		lcdcon2 |= ATMEL_LCDC_INVFRAME_INVERTED;
	else
		lcdcon2 |= ATMEL_LCDC_INVFRAME_NORMAL;

	/* hsync polarity */
	if (dmode->flags & DRM_MODE_FLAG_PHSYNC)
		lcdcon2 |= ATMEL_LCDC_INVLINE_INVERTED;
	else
		lcdcon2 |= ATMEL_LCDC_INVLINE_NORMAL;

	bus_flags = lcdc_dc->connector.display_info.bus_flags;

	/* dot clock (pix clock) polarity */
	if (bus_flags & DRM_BUS_FLAG_PIXDATA_NEGEDGE)
		lcdcon2 |= ATMEL_LCDC_INVCLK_INVERTED;
	else
		lcdcon2 |= ATMEL_LCDC_INVCLK_NORMAL;

	/* Date Enable polarity */
	if (bus_flags & DRM_BUS_FLAG_DE_LOW)
		lcdcon2 |= ATMEL_LCDC_INVDVAL_INVERTED;
	else
		lcdcon2 |= ATMEL_LCDC_INVDVAL_NORMAL;

	/* Clock is always active */
	lcdcon2 |= ATMEL_LCDC_CLKMOD_ALWAYSACTIVE;

	/* Memory layout */
	if (format->format & DRM_FORMAT_BIG_ENDIAN)
		lcdcon2 |= ATMEL_LCDC_MEMOR_BIG;
	else
		lcdcon2 |= ATMEL_LCDC_MEMOR_LITTLE;

	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_LCDCON2, lcdcon2);

	lcdc_dc_start(lcdc_dc);

	drm_crtc_vblank_on(crtc);
}

static void lcdc_dc_display_enable(struct drm_simple_display_pipe *pipe,
				   struct drm_crtc_state *cstate,
				   struct drm_plane_state *plane_state)
{
	struct lcdc_dc *lcdc_dc;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	crtc = &pipe->crtc;
	drm = crtc->dev;
	lcdc_dc = drm->dev_private;

	ret = regulator_enable(lcdc_dc->lcd_supply);
	if (ret)
		DRM_DEV_ERROR(lcdc_dc->dev, "regulator_enable failed (%d)",
			      ret);

	drm_panel_prepare(lcdc_dc->panel);
	lcdc_dc_crtc_mode_set_nofb(lcdc_dc);

	/*
	 * drm_simple_kms_helper have no support for gamma setup.
	 * Add: lcdc_dc_setcolor(lcdc_dc, crtc); when we have it
	 */
	lcdc_dc_enable(lcdc_dc, drm, crtc);

	drm_panel_enable(lcdc_dc->panel);
}

static void lcdc_dc_display_disable(struct drm_simple_display_pipe *pipe)
{
	struct lcdc_dc *lcdc_dc;
	struct drm_device *drm;
	struct drm_crtc *crtc;

	crtc = &pipe->crtc;
	drm = crtc->dev;
	lcdc_dc = drm->dev_private;

	drm_crtc_vblank_off(crtc);

	drm_panel_disable(lcdc_dc->panel);

	lcdc_dc_stop(lcdc_dc);

	drm_panel_unprepare(lcdc_dc->panel);
	regulator_disable(lcdc_dc->lcd_supply);
}

/* Update DMA config */
static void lcdc_dc_update_dma(struct lcdc_dc *lcdc_dc,
			       struct drm_simple_display_pipe *pipe)
{
	struct drm_plane_state *plane_state;
	struct drm_framebuffer *fb;
	unsigned int burst_length;
	unsigned int frame_size;
	dma_addr_t dma_addr;

	plane_state = pipe->plane.state;
	fb = plane_state->fb;

	if (fb) {
		unsigned int dmafrmcfg;

		dma_addr = drm_fb_cma_get_gem_addr(fb, pipe->plane.state, 0);

		/* Set frame buffer DMA base address */
		regmap_write(lcdc_dc->regmap, ATMEL_LCDC_DMABADDR1, dma_addr);

		/*
		 * Set frame size and burst length
		 * Frame_size equals size of visible area * bits / 32
		 * (size in 32 bit words)
		 */
		frame_size = plane_state->crtc_w * plane_state->crtc_h;
		frame_size = (frame_size * fb->format->depth) / 32;

		burst_length = ATMEL_LCDC_DMA_BURST_LEN - 1;

		dmafrmcfg = frame_size;
		dmafrmcfg |= burst_length << ATMEL_LCDC_BLENGTH_OFFSET;

		regmap_write(lcdc_dc->regmap, ATMEL_LCDC_DMAFRMCFG, dmafrmcfg);
	}
}

static void lcdc_dc_update_event(struct drm_simple_display_pipe *pipe)
{
	struct drm_pending_vblank_event *event;
	struct drm_crtc *crtc;

	crtc = &pipe->crtc;
	if (!crtc)
		return;

	spin_lock_irq(&crtc->dev->event_lock);
	event = crtc->state->event;
	if (event) {
		crtc->state->event = NULL;

		if (drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, event);
		else
			drm_crtc_send_vblank_event(crtc, event);
	}
	spin_unlock_irq(&crtc->dev->event_lock);
}

static void lcdc_dc_display_update(struct drm_simple_display_pipe *pipe,
				   struct drm_plane_state *old_pstate)
{
	struct drm_device *drm;
	struct lcdc_dc *lcdc_dc;

	drm = pipe->crtc.dev;
	lcdc_dc = drm->dev_private;

	/* Re-initialize the DMA engine... */
	lcdc_dc_update_dma(lcdc_dc, pipe);

	/* vblank event handling */
	lcdc_dc_update_event(pipe);
}

static int lcdc_dc_display_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc;
	struct lcdc_dc *lcdc_dc;

	crtc  = &pipe->crtc;
	lcdc_dc = crtc->dev->dev_private;

	/* Last line interrupt enable */
	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_IER, ATMEL_LCDC_LSTLNI);

	return 0;
}

static void lcdc_dc_display_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc;
	struct lcdc_dc *lcdc_dc;

	crtc  = &pipe->crtc;
	lcdc_dc = crtc->dev->dev_private;

	/* Last line interrupt disable */
	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_IDR, ATMEL_LCDC_LSTLNI);
}

static const struct drm_simple_display_pipe_funcs lcdc_dc_display_funcs = {
	.check = lcdc_dc_display_check,
	.enable = lcdc_dc_display_enable,
	.disable = lcdc_dc_display_disable,
	.update = lcdc_dc_display_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
	.enable_vblank = lcdc_dc_display_enable_vblank,
	.disable_vblank = lcdc_dc_display_disable_vblank,
};

static const struct drm_mode_config_funcs mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* scheduled worker to reset LCD */
static void reset_lcdc_work(struct work_struct *work)
{
	struct lcdc_dc *lcdc_dc;

	lcdc_dc = container_of(work, struct lcdc_dc, reset_lcdc_work);

	lcdc_dc_stop(lcdc_dc);
	lcdc_dc_start(lcdc_dc);
}

static irqreturn_t lcdc_dc_irq_handler(int irq, void *arg)
{
	struct lcdc_dc *lcdc_dc;
	struct drm_device *drm;
	unsigned int status;
	struct device *dev;
	unsigned int imr;
	unsigned int isr;

	drm = arg;
	lcdc_dc = drm->dev_private;
	dev = lcdc_dc->dev;

	regmap_read(lcdc_dc->regmap, ATMEL_LCDC_IMR, &imr);
	regmap_read(lcdc_dc->regmap, ATMEL_LCDC_ISR, &isr);
	status = imr & isr;
	if (!status)
		return IRQ_NONE;

	if (status & ATMEL_LCDC_LSTLNI)
		drm_crtc_handle_vblank(&lcdc_dc->pipe.crtc);

	if (status & ATMEL_LCDC_UFLWI) {
		DRM_DEV_INFO(dev, "FIFO underflow %#x\n", status);
		/* reset DMA and FIFO to avoid screen shifting */
		schedule_work(&lcdc_dc->reset_lcdc_work);
	}
	if (status & ATMEL_LCDC_OWRI)
		DRM_DEV_INFO(dev, "FIFO overwrite interrupt");

	if (status & ATMEL_LCDC_MERI)
		DRM_DEV_INFO(dev, "DMA memory error");

	/* Clear all reported (from ISR) interrupts */
	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_ICR, isr);

	return IRQ_HANDLED;
}

static int lcdc_dc_irq_postinstall(struct drm_device *dev)
{
	struct lcdc_dc *lcdc_dc;
	unsigned int ier;

	lcdc_dc = dev->dev_private;

	ier = 0;
	/* FIFO underflow interrupt enable */
	ier |= ATMEL_LCDC_UFLWI;
	/* FIFO overwrite interrupt enable */
	ier |= ATMEL_LCDC_OWRI;
	/* DMA memory error interrupt enable */
	ier |= ATMEL_LCDC_MERI;
	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_IER, ier);

	return 0;
}

static void lcdc_dc_irq_uninstall(struct drm_device *dev)
{
	struct lcdc_dc *lcdc_dc;
	unsigned int isr;

	lcdc_dc  = dev->dev_private;

	/* disable all interrupts */
	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_IDR, ~0);
	regmap_write(lcdc_dc->regmap, ATMEL_LCDC_ICR, ~0);

	/* Clear any pending interrupts */
	regmap_read(lcdc_dc->regmap, ATMEL_LCDC_ISR, &isr);
}

DEFINE_DRM_GEM_CMA_FOPS(lcdc_dc_drm_fops);

static struct drm_driver lcdc_dc_drm_driver = {
	.driver_features = DRIVER_HAVE_IRQ |
			   DRIVER_GEM |
			   DRIVER_MODESET |
			   DRIVER_PRIME |
			   DRIVER_ATOMIC,
	.name = "atmel-lcdc",
	.desc = "Atmel LCD Display Controller DRM",
	.date = "20180808",
	.major = 1,
	.minor = 0,
	.patchlevel = 0,

	.lastclose = drm_fb_helper_lastclose,
	.fops = &lcdc_dc_drm_fops,

	.irq_handler = lcdc_dc_irq_handler,
	.irq_preinstall = lcdc_dc_irq_uninstall,
	.irq_postinstall = lcdc_dc_irq_postinstall,
	.irq_uninstall = lcdc_dc_irq_uninstall,

	.dumb_create = drm_gem_cma_dumb_create,

	.gem_print_info = drm_gem_cma_print_info,
	.gem_vm_ops = &drm_gem_cma_vm_ops,

	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,

	.gem_prime_import = drm_gem_prime_import,
	.gem_prime_export = drm_gem_prime_export,

	.gem_prime_get_sg_table	= drm_gem_cma_prime_get_sg_table,
	.gem_free_object_unlocked = drm_gem_cma_free_object,

	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.gem_prime_vmap = drm_gem_cma_prime_vmap,
	.gem_prime_vunmap = drm_gem_cma_prime_vunmap,
	.gem_prime_mmap = drm_gem_cma_prime_mmap,
};

static int lcdc_dc_modeset_init(struct lcdc_dc *lcdc_dc, struct drm_device *drm)
{
	struct drm_bridge *bridge;
	struct drm_panel *panel;
	struct device_node *np;
	struct device *dev;
	int ret;

	dev = drm->dev;

	drm_mode_config_init(drm);
	drm->mode_config.min_width  = lcdc_dc->desc->min_width;
	drm->mode_config.min_height = lcdc_dc->desc->min_height;
	drm->mode_config.max_width  = lcdc_dc->desc->max_width;
	drm->mode_config.max_height = lcdc_dc->desc->max_height;
	drm->mode_config.funcs	    = &mode_config_funcs;

	np = dev->of_node;
	/* port@0 is the output port */
	ret = drm_of_find_panel_or_bridge(np, 0, 0, &panel, &bridge);
	if (ret && ret != -ENODEV) {
		DRM_DEV_ERROR(dev, "Failed to find panel (%d)\n", ret);
		goto err_out;
	}

	bridge = drm_panel_bridge_add(panel, DRM_MODE_CONNECTOR_Unknown);
	if (IS_ERR(bridge)) {
		ret = PTR_ERR(bridge);
		DRM_DEV_ERROR(dev, "Failed to add bridge (%d)", ret);
		goto err_panel_remove;
	}

	lcdc_dc->panel = panel;
	lcdc_dc->bridge = bridge;

	ret = drm_simple_display_pipe_init(drm,
					   &lcdc_dc->pipe,
					   &lcdc_dc_display_funcs,
					   lcdc_dc_formats,
					   ARRAY_SIZE(lcdc_dc_formats),
					   NULL,
					   &lcdc_dc->connector);
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to init display pipe (%d)\n", ret);
		goto err_panel_remove;
	}

	ret = drm_simple_display_pipe_attach_bridge(&lcdc_dc->pipe, bridge);
	if (ret) {
		DRM_DEV_ERROR(dev, "failed to attach bridge (%d)", ret);
		goto err_panel_remove;
	}

	drm_mode_config_reset(drm);

	return 0;

err_panel_remove:
	if (panel)
		drm_panel_bridge_remove(bridge);

err_out:
	return ret;
}

static int lcdc_dc_load(struct drm_device *drm)
{
	const struct of_device_id *match;
	struct platform_device *pdev;
	struct lcdc_dc *lcdc_dc;
	struct device *dev;
	int ret;

	dev = drm->dev;
	pdev = to_platform_device(dev);

	match = of_match_node(atmel_lcdc_of_match, dev->parent->of_node);
	if (!match) {
		DRM_DEV_ERROR(dev, "invalid compatible string (node=%s)",
			      dev->parent->of_node->name);
		return -ENODEV;
	}

	if (!match->data) {
		DRM_DEV_ERROR(dev, "invalid lcdc_dc description\n");
		return -EINVAL;
	}

	lcdc_dc = devm_kzalloc(dev, sizeof(*lcdc_dc), GFP_KERNEL);
	if (!lcdc_dc) {
		DRM_DEV_ERROR(dev, "Failed to allocate lcdc_dc\n");
		return -ENOMEM;
	}

	/* reset of lcdc might sleep and require a preemptible task context */
	INIT_WORK(&lcdc_dc->reset_lcdc_work, reset_lcdc_work);

	platform_set_drvdata(pdev, drm);
	dev_set_drvdata(dev, lcdc_dc);

	lcdc_dc->mfd_lcdc = dev_get_drvdata(dev->parent);
	drm->dev_private = lcdc_dc;

	lcdc_dc->regmap = lcdc_dc->mfd_lcdc->regmap;
	lcdc_dc->desc = match->data;
	lcdc_dc->dev = dev;

	lcdc_dc->lcd_supply = devm_regulator_get(dev, "lcd");
	if (IS_ERR(lcdc_dc->lcd_supply)) {
		DRM_DEV_ERROR(dev, "Failed to get lcd-supply (%ld)\n",
			      PTR_ERR(lcdc_dc->lcd_supply));
		lcdc_dc->lcd_supply = NULL;
	}

	lcdc_dc_start_clock(lcdc_dc);

	pm_runtime_enable(dev);

	ret = drm_vblank_init(drm, 1);
	if (ret) {
		DRM_DEV_ERROR(dev, "failed to initialize vblank (%d)\n",
			      ret);
		goto err_pm_runtime_disable;
	}

	ret = lcdc_dc_modeset_init(lcdc_dc, drm);
	if (ret) {
		DRM_DEV_ERROR(dev, "modeset_init failed (%d)", ret);
		goto err_pm_runtime_disable;
	}

	pm_runtime_get_sync(dev);
	ret = drm_irq_install(drm, lcdc_dc->mfd_lcdc->irq);
	pm_runtime_put_sync(dev);
	if (ret < 0) {
		DRM_DEV_ERROR(dev, "Failed to install IRQ (%d)\n", ret);

		goto err_pm_runtime_disable;
	}

	/*
	 * Passing in 16 here will make the RGB656 mode the default
	 * Passing in 32 will use XRGB8888 mode
	 */
	drm_fb_cma_fbdev_init(drm, 16, 0);

	drm_kms_helper_poll_init(drm);

	lcdc_dc->fbdev = drm_fbdev_cma_init(drm, 8, 1);
	if (IS_ERR(lcdc_dc->fbdev)) {
		ret = PTR_ERR(lcdc_dc->fbdev);
		DRM_DEV_ERROR(dev, "Failed to init FB CMA area (%d)", ret);
		goto err_irq_uninstall;
	}

	drm_helper_hpd_irq_event(drm);

	return 0;

err_irq_uninstall:
	pm_runtime_get_sync(dev);
	drm_irq_uninstall(drm);
	pm_runtime_put_sync(dev);

err_pm_runtime_disable:
	pm_runtime_disable(dev);
	lcdc_dc_stop_clock(lcdc_dc);

	cancel_work_sync(&lcdc_dc->reset_lcdc_work);

	return ret;
}

static void lcdc_dc_unload(struct drm_device *dev)
{
	struct lcdc_dc *lcdc_dc = dev->dev_private;

	drm_fb_cma_fbdev_fini(dev);
	flush_work(&lcdc_dc->reset_lcdc_work);
	drm_kms_helper_poll_fini(dev);
	if (lcdc_dc->panel)
		drm_panel_bridge_remove(lcdc_dc->bridge);
	drm_mode_config_cleanup(dev);

	pm_runtime_get_sync(dev->dev);
	drm_irq_uninstall(dev);
	pm_runtime_put_sync(dev->dev);

	dev->dev_private = NULL;

	pm_runtime_disable(dev->dev);
	lcdc_dc_stop_clock(lcdc_dc);
	cancel_work_sync(&lcdc_dc->reset_lcdc_work);
}


static int lcdc_dc_probe(struct platform_device *pdev)
{
	struct drm_device *drm;
	struct device *dev;
	int ret;

	dev = &pdev->dev;

	drm = drm_dev_alloc(&lcdc_dc_drm_driver, dev);
	if (IS_ERR(drm)) {
		DRM_DEV_ERROR(dev, "Failed to allocate drm device\n");
		return PTR_ERR(drm);
	}

	ret = lcdc_dc_load(drm);
	if (ret)
		goto err_put_ref;

	ret = drm_dev_register(drm, 0);
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to register drm (%d)\n", ret);
		goto err_unload;
	}

	return 0;

err_unload:
	lcdc_dc_unload(drm);

err_put_ref:
	drm_dev_put(drm);
	return ret;
}

static int lcdc_dc_remove(struct platform_device *pdev)
{
	struct drm_device *drm;

	drm = platform_get_drvdata(pdev);

	drm_dev_unregister(drm);
	lcdc_dc_unload(drm);
	drm_dev_unref(drm);

	return 0;
}

static const struct of_device_id lcdc_dc_dt_ids[] = {
	{ .compatible = "atmel,lcdc-display-controller", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, lcdc_dc_dt_ids);

static struct platform_driver lcdc_dc_driver = {
	.probe		= lcdc_dc_probe,
	.remove		= lcdc_dc_remove,
	.driver		= {
		.of_match_table = lcdc_dc_dt_ids,
		.name	= "atmel-lcdc-dc",
	},
};

module_platform_driver(lcdc_dc_driver);

MODULE_AUTHOR("Sam Ravnborg <sam@ravnborg.org>");
MODULE_DESCRIPTION("Atmel LCDC Display Controller DRM Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:atmel-lcdc-dc");
