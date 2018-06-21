// SPDX-License-Identifier: GPL-2.0
/*
 * Panel driver for the TPO TPG110 400CH LTPS TFT LCD Single Chip
 * Digital Driver.
 *
 * This chip drives a TFT LCD, so it does not know what kind of
 * display is actually connected to it, so the width and height of that
 * display needs to be supplied from the machine configuration.
 *
 * Author:
 * Linus Walleij <linus.wallei@linaro.org>
 */
#include <drm/drmP.h>
#include <drm/drm_panel.h>

#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/backlight.h>

#include <video/of_videomode.h>
#include <video/videomode.h>

#define TPG110_TEST			0x00
#define TPG110_CHIPID			0x01
#define TPG110_CTRL1			0x02
#define TPG110_RES_MASK			GENMASK(2, 0)
#define TPG110_RES_800X480		0x07
#define TPG110_RES_640X480		0x06
#define TPG110_RES_480X272		0x05
#define TPG110_RES_480X640		0x04
#define TPG110_RES_480X272_D		0x01 /* Dual scan: outputs 800x480 */
#define TPG110_RES_400X240_D		0x00 /* Dual scan: outputs 800x480 */
#define TPG110_CTRL2			0x03
#define TPG110_CTRL2_PM			BIT(0)
#define TPG110_CTRL2_RES_PM_CTRL	BIT(7)

/**
 * struct tpg110_panel_mode - lookup struct for the supported modes
 */
struct tpg110_panel_mode {
	/**
	 * @name: the name of this panel
	 */
	const char *name;
	/**
	 * @magic: the magic value from the detection register
	 */
	u32 magic;
	/**
	 * @mode: the DRM display mode for this panel
	 */
	struct drm_display_mode mode;
	/**
	 * @bus_flags: the DRM bus flags for this panel e.g. inverted clock
	 */
	u32 bus_flags;
};

/**
 * struct tpg110 - state container for the TPG110 panel
 */
struct tpg110 {
	/**
	 * @dev: the container device
	 */
	struct device *dev;
	/**
	 * @panel: the DRM panel instance for this device
	 */
	struct drm_panel panel;
	/**
	 * @backlight: backlight for this panel
	 */
	struct backlight_device *backlight;
	/**
	 * @panel_type: the panel mode as detected
	 */
	const struct tpg110_panel_mode *panel_mode;
	/**
	 * @width: the width of this panel in mm
	 */
	u32 width;
	/**
	 * @height: the height of this panel in mm
	 */
	u32 height;
	/**
	 * @grestb: reset GPIO line
	 */
	struct gpio_desc *grestb;
	/**
	 * @scen: scen GPIO line
	 */
	struct gpio_desc *scen;
	/**
	 * @scl: scl (clock) GPIO line
	 */
	struct gpio_desc *scl;
	/**
	 * @sda: sda (data) GPIO line
	 */
	struct gpio_desc *sda;
};

/*
 * TPG110 modes, these are the simple modes, the dualscan modes that
 * take 400x240 or 480x272 in and display as 800x480 are not listed.
 */
static const struct tpg110_panel_mode tpg110_modes[] = {
	{
		.name = "800x480 RGB",
		.magic = TPG110_RES_800X480,
		.mode = {
			.clock = 33200,
			.hdisplay = 800,
			.hsync_start = 800 + 40,
			.hsync_end = 800 + 40 + 1,
			.htotal = 800 + 40 + 1 + 216,
			.vdisplay = 480,
			.vsync_start = 480 + 10,
			.vsync_end = 480 + 10 + 1,
			.vtotal = 480 + 10 + 1 + 35,
			.vrefresh = 60,
		},
	},
	{
		.name = "640x480 RGB",
		.magic = TPG110_RES_640X480,
		.mode = {
			.clock = 25200,
			.hdisplay = 640,
			.hsync_start = 640 + 24,
			.hsync_end = 640 + 24 + 1,
			.htotal = 640 + 24 + 1 + 136,
			.vdisplay = 480,
			.vsync_start = 480 + 18,
			.vsync_end = 480 + 18 + 1,
			.vtotal = 480 + 18 + 1 + 27,
			.vrefresh = 60,
		},
	},
	{
		.name = "480x272 RGB",
		.magic = TPG110_RES_480X272,
		.mode = {
			.clock = 9000,
			.hdisplay = 480,
			.hsync_start = 480 + 2,
			.hsync_end = 480 + 2 + 1,
			.htotal = 480 + 2 + 1 + 43,
			.vdisplay = 272,
			.vsync_start = 272 + 2,
			.vsync_end = 272 + 2 + 1,
			.vtotal = 272 + 2 + 1 + 12,
			.vrefresh = 60,
		},
	},
	{
		.name = "480x640 RGB",
		.magic = TPG110_RES_480X640,
		.mode = {
			.clock = 20500,
			.hdisplay = 480,
			.hsync_start = 480 + 2,
			.hsync_end = 480 + 2 + 1,
			.htotal = 480 + 2 + 1 + 43,
			.vdisplay = 640,
			.vsync_start = 640 + 4,
			.vsync_end = 640 + 4 + 1,
			.vtotal = 640 + 4 + 1 + 8,
			.vrefresh = 60,
		},
	},
	{
		.name = "400x240 RGB",
		.magic = TPG110_RES_400X240_D,
		.mode = {
			.clock = 8300,
			.hdisplay = 400,
			.hsync_start = 400 + 20,
			.hsync_end = 400 + 20 + 1,
			.htotal = 400 + 20 + 1 + 108,
			.vdisplay = 240,
			.vsync_start = 240 + 2,
			.vsync_end = 240 + 2 + 1,
			.vtotal = 240 + 2 + 1 + 20,
			.vrefresh = 60,
		},
	},
};

static inline struct tpg110 *
to_tpg110(struct drm_panel *panel)
{
	return container_of(panel, struct tpg110, panel);
}

static u8 tpg110_readwrite_reg(struct tpg110 *tpg, bool write,
			       u8 address, u8 outval)
{
	int i;
	u8 inval = 0;

	/* Assert SCEN */
	gpiod_set_value_cansleep(tpg->scen, 1);
	ndelay(150);
	/* Hammer out the address */
	for (i = 5; i >= 0; i--) {
		if (address & BIT(i))
			gpiod_set_value_cansleep(tpg->sda, 1);
		else
			gpiod_set_value_cansleep(tpg->sda, 0);
		ndelay(150);
		/* Send an SCL pulse */
		gpiod_set_value_cansleep(tpg->scl, 1);
		ndelay(160);
		gpiod_set_value_cansleep(tpg->scl, 0);
		ndelay(160);
	}

	if (write) {
		/* WRITE */
		gpiod_set_value_cansleep(tpg->sda, 0);
	} else {
		/* READ */
		gpiod_set_value_cansleep(tpg->sda, 1);
	}
	ndelay(150);
	/* Send an SCL pulse */
	gpiod_set_value_cansleep(tpg->scl, 1);
	ndelay(160);
	gpiod_set_value_cansleep(tpg->scl, 0);
	ndelay(160);

	if (!write)
		/* HiZ turn-around cycle */
		gpiod_direction_input(tpg->sda);
	ndelay(150);
	/* Send an SCL pulse */
	gpiod_set_value_cansleep(tpg->scl, 1);
	ndelay(160);
	gpiod_set_value_cansleep(tpg->scl, 0);
	ndelay(160);

	/* Hammer in/out the data */
	for (i = 7; i >= 0; i--) {
		int value;

		if (write) {
			value = !!(outval & BIT(i));
			gpiod_set_value_cansleep(tpg->sda, value);
		} else {
			value = gpiod_get_value(tpg->sda);
			if (value)
				inval |= BIT(i);
		}
		ndelay(150);
		/* Send an SCL pulse */
		gpiod_set_value_cansleep(tpg->scl, 1);
		ndelay(160);
		gpiod_set_value_cansleep(tpg->scl, 0);
		ndelay(160);
	}

	gpiod_direction_output(tpg->sda, 0);
	/* Deassert SCEN */
	gpiod_set_value_cansleep(tpg->scen, 0);
	/* Satisfies SCEN pulse width */
	udelay(1);

	return inval;
}

static u8 tpg110_read_reg(struct tpg110 *tpg, u8 address)
{
	return tpg110_readwrite_reg(tpg, false, address, 0);
}

static void tpg110_write_reg(struct tpg110 *tpg, u8 address, u8 outval)
{
	tpg110_readwrite_reg(tpg, true, address, outval);
}

static int tpg110_startup(struct tpg110 *tpg)
{
	u8 val;
	int i;

	/* De-assert the reset signal */
	gpiod_set_value_cansleep(tpg->grestb, 0);
	mdelay(1);
	dev_info(tpg->dev, "de-asserted GRESTB\n");

	/* Test display communication */
	tpg110_write_reg(tpg, TPG110_TEST, 0x55);
	val = tpg110_read_reg(tpg, TPG110_TEST);
	if (val != 0x55) {
		dev_err(tpg->dev, "failed communication test\n");
		return -ENODEV;
	}

	val = tpg110_read_reg(tpg, TPG110_CHIPID);
	dev_info(tpg->dev, "TPG110 chip ID: %d version: %d\n",
		 val >> 4, val & 0x0f);

	/* Show display resolution */
	val = tpg110_read_reg(tpg, TPG110_CTRL1);
	val &= TPG110_RES_MASK;
	switch (val) {
	case TPG110_RES_400X240_D:
		dev_info(tpg->dev,
			 "IN 400x240 RGB -> OUT 800x480 RGB (dual scan)");
		break;
	case TPG110_RES_480X272_D:
		dev_info(tpg->dev,
			 "IN 480x272 RGB -> OUT 800x480 RGB (dual scan)");
		break;
	case TPG110_RES_480X640:
		dev_info(tpg->dev, "480x640 RGB");
		break;
	case TPG110_RES_480X272:
		dev_info(tpg->dev, "480x272 RGB");
		break;
	case TPG110_RES_640X480:
		dev_info(tpg->dev, "640x480 RGB");
		break;
	case TPG110_RES_800X480:
		dev_info(tpg->dev, "800x480 RGB");
		break;
	default:
		dev_info(tpg->dev, "ILLEGAL RESOLUTION");
		break;
	}

	/* From the producer side, this is the same resolution */
	if (val == TPG110_RES_480X272_D)
		val = TPG110_RES_480X272;

	for (i = 0; i < ARRAY_SIZE(tpg110_modes); i++) {
		const struct tpg110_panel_mode *pm;

		pm = &tpg110_modes[i];
		if (pm->magic == val) {
			tpg->panel_mode = pm;
			break;
		}
	}
	if (i == ARRAY_SIZE(tpg110_modes)) {
		dev_err(tpg->dev, "unsupported mode (%02x) detected\n",
			val);
		return -ENODEV;
	}

	val = tpg110_read_reg(tpg, TPG110_CTRL2);
	dev_info(tpg->dev, "resolution and standby is controlled by %s\n",
		 (val & TPG110_CTRL2_RES_PM_CTRL) ? "software" : "hardware");
	/* Take control over resolution and standby */
	val |= TPG110_CTRL2_RES_PM_CTRL;
	tpg110_write_reg(tpg, TPG110_CTRL2, val);

	return 0;
}

static int tpg110_disable(struct drm_panel *panel)
{
	struct tpg110 *tpg = to_tpg110(panel);
	u8 val;

	/* Put chip into standby */
	val = tpg110_read_reg(tpg, TPG110_CTRL2_PM);
	val &= ~TPG110_CTRL2_PM;
	tpg110_write_reg(tpg, TPG110_CTRL2_PM, val);

	if (tpg->backlight) {
		tpg->backlight->props.power = FB_BLANK_POWERDOWN;
		tpg->backlight->props.state |= BL_CORE_FBBLANK;
		backlight_update_status(tpg->backlight);
	}

	return 0;
}

static int tpg110_enable(struct drm_panel *panel)
{
	struct tpg110 *tpg = to_tpg110(panel);
	u8 val;

	if (tpg->backlight) {
		tpg->backlight->props.state &= ~BL_CORE_FBBLANK;
		tpg->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(tpg->backlight);
	}

	/* Take chip out of standby */
	val = tpg110_read_reg(tpg, TPG110_CTRL2_PM);
	val |= TPG110_CTRL2_PM;
	tpg110_write_reg(tpg, TPG110_CTRL2_PM, val);

	return 0;
}

/**
 * tpg110_get_modes() - return the appropriate mode
 * @panel: the panel to get the mode for
 *
 * This currently does not present a forest of modes, instead it
 * presents the mode that is configured for the system under use,
 * and which is detected by reading the registers of the display.
 */
static int tpg110_get_modes(struct drm_panel *panel)
{
	struct drm_connector *connector = panel->connector;
	struct tpg110 *tpg = to_tpg110(panel);
	struct drm_display_mode *mode;

	strncpy(connector->display_info.name, tpg->panel_mode->name,
		DRM_DISPLAY_INFO_LEN);
	connector->display_info.width_mm = tpg->width;
	connector->display_info.height_mm = tpg->height;
	connector->display_info.bus_flags = tpg->panel_mode->bus_flags;

	mode = drm_mode_duplicate(panel->drm, &tpg->panel_mode->mode);
	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	mode->width_mm = tpg->width;
	mode->height_mm = tpg->height;

	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs tpg110_drm_funcs = {
	.disable = tpg110_disable,
	.enable = tpg110_enable,
	.get_modes = tpg110_get_modes,
};

static int tpg110_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *backlight;
	struct tpg110 *tpg;
	int ret;

	tpg = devm_kzalloc(dev, sizeof(*tpg), GFP_KERNEL);
	if (!tpg)
		return -ENOMEM;
	tpg->dev = dev;

	/* We get the physical display dimensions from the DT */
	ret = of_property_read_u32(np, "width-mm", &tpg->width);
	if (ret)
		dev_err(dev, "no panel width specified\n");
	ret = of_property_read_u32(np, "height-mm", &tpg->height);
	if (ret)
		dev_err(dev, "no panel height specified\n");

	/* Look for some optional backlight */
	backlight = of_parse_phandle(np, "backlight", 0);
	if (backlight) {
		tpg->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!tpg->backlight)
			return -EPROBE_DEFER;
	}

	/* This asserts the GRESTB signal, putting the display into reset */
	tpg->grestb = devm_gpiod_get(dev, "grestb", GPIOD_OUT_HIGH);
	if (IS_ERR(tpg->grestb)) {
		dev_err(dev, "no GRESTB GPIO\n");
		return -ENODEV;
	}
	tpg->scen = devm_gpiod_get(dev, "scen", GPIOD_OUT_LOW);
	if (IS_ERR(tpg->scen)) {
		dev_err(dev, "no SCEN GPIO\n");
		return -ENODEV;
	}
	tpg->scl = devm_gpiod_get(dev, "scl", GPIOD_OUT_LOW);
	if (IS_ERR(tpg->scl)) {
		dev_err(dev, "no SCL GPIO\n");
		return -ENODEV;
	}
	tpg->sda = devm_gpiod_get(dev, "sda", GPIOD_OUT_LOW);
	if (IS_ERR(tpg->sda)) {
		dev_err(dev, "no SDA GPIO\n");
		return -ENODEV;
	}

	ret = tpg110_startup(tpg);
	if (ret)
		return ret;

	drm_panel_init(&tpg->panel);
	tpg->panel.dev = dev;
	tpg->panel.funcs = &tpg110_drm_funcs;

	return drm_panel_add(&tpg->panel);
}

static const struct of_device_id tpg110_match[] = {
	{ .compatible = "tpo,tpg110", },
	{},
};
MODULE_DEVICE_TABLE(of, tpg110_match);

static struct platform_driver tpg110_driver = {
	.probe		= tpg110_probe,
	.driver		= {
		.name	= "tpo-tpg110-panel",
		.of_match_table = tpg110_match,
	},
};
module_platform_driver(tpg110_driver);

MODULE_AUTHOR("Linus Walleij <linus.walleij@linaro.org>");
MODULE_DESCRIPTION("TPO TPG110 panel driver");
MODULE_LICENSE("GPL v2");
