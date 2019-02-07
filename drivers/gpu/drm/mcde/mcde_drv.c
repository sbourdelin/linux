// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Linus Walleij <linus.walleij@linaro.org>
 * Parts of this file were based on the MCDE driver by Marcus Lorentzon
 * (C) ST-Ericsson SA 2013
 */

/**
 * DOC: ST-Ericsson MCDE Driver
 *
 * The MCDE (short for Multi-channel display engine) is a graphics
 * controller found in the Ux500 chipsets, such as NovaThor U8500.
 * It was initially conceptualized by ST Microelectronics for the
 * successor of the Nomadik line, STn8500 but productified in the
 * ST-Ericsson U8500 where is was used for mass-market deployments
 * in Android phones from Samsung and Sony Ericsson.
 *
 * It can do 1080p30 on SDTV CCIR656, DPI-2, DBI-2 or DSI for
 * panels with or without frame buffering and can convert most
 * input formats including most variants of RGB and YUV.
 *
 * The hardware has four display pipes, and the layout is a little
 * bit like this:
 *
 * Memory     -> 6 channels -> 5 formatters -> DSI/DPI -> LCD/HDMI
 * 10 sources    (overlays)                    3 x DSI
 *
 * The memory has 5 input channels (memory ports):
 * 2 channel A (LCD/TV)
 * 2 channel B (LCD/TV)
 * 1 channel CO/C1 (Panel with embedded buffer)
 *
 * 3 of the formatters are for DSI
 * 2 of the formatters are for DPI
 *
 * Behind the formatters are the DSI or DPI ports, that route to
 * the external pins of the chip. As there are 3 DSI ports and one
 * DPI port, it is possible to configure up to 4 display pipelines.
 */

#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/dma-buf.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/component.h>

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_panel.h>
#include <drm/drm_of.h>
#include <drm/drm_bridge.h>

#include "mcde_drm.h"

#define DRIVER_DESC	"DRM module for MCDE"

#define MCDE_CR 0x00000000
#define MCDE_CR_IFIFOEMPTYLINECOUNT_V422_SHIFT 0
#define MCDE_CR_IFIFOEMPTYLINECOUNT_V422_MASK 0x0000003F
#define MCDE_CR_IFIFOCTRLEN BIT(15)
#define MCDE_CR_UFRECOVERY_MODE_V422 BIT(16)
#define MCDE_CR_WRAP_MODE_V422_SHIFT BIT(17)
#define MCDE_CR_AUTOCLKG_EN BIT(30)
#define MCDE_CR_MCDEEN BIT(31)

#define MCDE_CONF0 0x00000004
#define MCDE_CONF0_SYNCMUX0 BIT(0)
#define MCDE_CONF0_SYNCMUX1 BIT(1)
#define MCDE_CONF0_SYNCMUX2 BIT(2)
#define MCDE_CONF0_SYNCMUX3 BIT(3)
#define MCDE_CONF0_SYNCMUX4 BIT(4)
#define MCDE_CONF0_SYNCMUX5 BIT(5)
#define MCDE_CONF0_SYNCMUX6 BIT(6)
#define MCDE_CONF0_SYNCMUX7 BIT(7)
#define MCDE_CONF0_IFIFOCTRLWTRMRKLVL_SHIFT 12
#define MCDE_CONF0_IFIFOCTRLWTRMRKLVL_MASK 0x00007000
#define MCDE_CONF0_OUTMUX0_SHIFT 16
#define MCDE_CONF0_OUTMUX0_MASK 0x00070000
#define MCDE_CONF0_OUTMUX1_SHIFT 19
#define MCDE_CONF0_OUTMUX1_MASK 0x00380000
#define MCDE_CONF0_OUTMUX2_SHIFT 22
#define MCDE_CONF0_OUTMUX2_MASK 0x01C00000
#define MCDE_CONF0_OUTMUX3_SHIFT 25
#define MCDE_CONF0_OUTMUX3_MASK 0x0E000000
#define MCDE_CONF0_OUTMUX4_SHIFT 28
#define MCDE_CONF0_OUTMUX4_MASK 0x70000000

#define MCDE_SSP 0x00000008
#define MCDE_AIS 0x00000100
#define MCDE_IMSCERR 0x00000110
#define MCDE_RISERR 0x00000120
#define MCDE_MISERR 0x00000130
#define MCDE_SISERR 0x00000140

#define MCDE_PID 0x000001FC
#define MCDE_PID_METALFIX_VERSION_SHIFT 0
#define MCDE_PID_METALFIX_VERSION_MASK 0x000000FF
#define MCDE_PID_DEVELOPMENT_VERSION_SHIFT 8
#define MCDE_PID_DEVELOPMENT_VERSION_MASK 0x0000FF00
#define MCDE_PID_MINOR_VERSION_SHIFT 16
#define MCDE_PID_MINOR_VERSION_MASK 0x00FF0000
#define MCDE_PID_MAJOR_VERSION_SHIFT 24
#define MCDE_PID_MAJOR_VERSION_MASK 0xFF000000

static const struct drm_mode_config_funcs mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static irqreturn_t mcde_irq(int irq, void *data)
{
	struct mcde *mcde = data;
	u32 val;

	val = readl(mcde->regs + MCDE_MISERR);

	mcde_display_irq(mcde);

	if (val)
		dev_info(mcde->dev, "some error IRQ\n");
	writel(val, mcde->regs + MCDE_RISERR);

	return IRQ_HANDLED;
}

static int mcde_modeset_init(struct drm_device *drm)
{
	struct drm_mode_config *mode_config;
	struct mcde *mcde = drm->dev_private;
	int ret;

	mode_config = &drm->mode_config;
	mode_config->funcs = &mode_config_funcs;
	/* This hardware can do 1080p */
	mode_config->min_width = 1;
	mode_config->max_width = 1920;
	mode_config->min_height = 1;
	mode_config->max_height = 1080;

	if (mcde->te_sync) {
		ret = drm_vblank_init(drm, 1);
		if (ret) {
			dev_err(drm->dev, "failed to init vblank\n");
			goto out_config;
		}
	}

	ret = mcde_display_init(drm);
	if (ret) {
		dev_err(drm->dev, "failed to init display\n");
		goto out_config;
	}

	drm_mode_config_reset(drm);
	drm_fb_cma_fbdev_init(drm, 32, 0);
	drm_kms_helper_poll_init(drm);

	return 0;

out_config:
	drm_mode_config_cleanup(drm);
	return ret;
}

DEFINE_DRM_GEM_CMA_FOPS(drm_fops);

static struct drm_driver mcde_drm_driver = {
	.driver_features =
		DRIVER_MODESET | DRIVER_GEM | DRIVER_PRIME | DRIVER_ATOMIC,
	.lastclose = drm_fb_helper_lastclose,
	.ioctls = NULL,
	.fops = &drm_fops,
	.name = "mcde",
	.desc = DRIVER_DESC,
	.date = "20180529",
	.major = 1,
	.minor = 0,
	.patchlevel = 0,
	.dumb_create = drm_gem_cma_dumb_create,
	.gem_free_object_unlocked = drm_gem_cma_free_object,
	.gem_vm_ops = &drm_gem_cma_vm_ops,

	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_import = drm_gem_prime_import,
	.gem_prime_export = drm_gem_prime_export,
	.gem_prime_get_sg_table	= drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.gem_prime_vmap = drm_gem_cma_prime_vmap,
	.gem_prime_vunmap = drm_gem_cma_prime_vunmap,
	.gem_prime_mmap = drm_gem_cma_prime_mmap,
};

static int mcde_drm_bind(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	int ret;

	drm_mode_config_init(drm);

	ret = component_bind_all(drm->dev, drm);
	if (ret) {
		dev_err(dev, "can't bind component devices\n");
		return ret;
	}

	ret = mcde_modeset_init(drm);
	if (ret)
		goto unbind;

	ret = drm_dev_register(drm, 0);
	if (ret < 0)
		goto unbind;

	return 0;

unbind:
	component_unbind_all(drm->dev, drm);
	return ret;
}

static void mcde_drm_unbind(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);

	drm_dev_unregister(drm);
	component_unbind_all(drm->dev, drm);
	drm_mode_config_cleanup(drm);
}

static const struct component_master_ops mcde_drm_comp_ops = {
	.bind = mcde_drm_bind,
	.unbind = mcde_drm_unbind,
};

static struct platform_driver *const mcde_component_drivers[] = {
	&mcde_dsi_driver,
};

static int mcde_compare_dev(struct device *dev, void *data)
{
	return dev == data;
}

static int mcde_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct drm_device *drm;
	struct mcde *mcde;
	struct component_match *match;
	struct resource *res;
	u32 pid;
	u32 val;
	int irq;
	int ret;
	int i;

	mcde = devm_kzalloc(dev, sizeof(*mcde), GFP_KERNEL);
	if (!mcde)
		return -ENOMEM;
	mcde->dev = dev;

	drm = drm_dev_alloc(&mcde_drm_driver, dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);
	platform_set_drvdata(pdev, drm);
	mcde->drm = drm;
	/* Enable use of the TE signal and interrupt */
	mcde->te_sync = true;
	/* Enable continuous updates: this is what Linux' framebuffer expects */
	mcde->oneshot_mode = false;
	drm->dev_private = mcde;

	/* First obtain and turn on the main power */
	mcde->epod = devm_regulator_get(dev, "epod");
	if (IS_ERR(mcde->epod)) {
		ret = PTR_ERR(mcde->epod);
		dev_err(dev, "can't get EPOD regulator\n");
		goto dev_unref;
	}
	ret = regulator_enable(mcde->epod);
	if (ret) {
		dev_err(dev, "can't enable EPOD regulator\n");
		goto dev_unref;
	}
	mcde->vana = devm_regulator_get(dev, "vana");
	if (IS_ERR(mcde->vana)) {
		ret = PTR_ERR(mcde->vana);
		dev_err(dev, "can't get VANA regulator\n");
		goto regulator_epod_off;
	}
	ret = regulator_enable(mcde->vana);
	if (ret) {
		dev_err(dev, "can't enable VANA regulator\n");
		goto regulator_epod_off;
	}
	/* Vendor code uses v-esram34 but we don't, yet */

	/* Clock the silicon so we can access the registers */
	mcde->mcde_clk = devm_clk_get(dev, "mcde");
	if (IS_ERR(mcde->mcde_clk)) {
		dev_err(dev, "unable to get MCDE main clock\n");
		ret = PTR_ERR(mcde->mcde_clk);
		goto regulator_off;
	}
	ret = clk_prepare_enable(mcde->mcde_clk);
	if (ret) {
		dev_err(dev, "failed to enable MCDE main clock\n");
		goto regulator_off;
	}
	dev_info(dev, "MCDE clk rate %lu Hz\n", clk_get_rate(mcde->mcde_clk));

	/* Also retrieve the additional clocks */
	mcde->dsi0_clk = devm_clk_get(dev, "dsi0");
	if (IS_ERR(mcde->dsi0_clk)) {
		dev_err(dev, "unable to get DSI0 clock\n");
		ret = PTR_ERR(mcde->dsi0_clk);
		goto clk_disable;
	}
	mcde->dsi1_clk = devm_clk_get(dev, "dsi1");
	if (IS_ERR(mcde->dsi1_clk)) {
		dev_err(dev, "unable to get DSI1 clock\n");
		ret = PTR_ERR(mcde->dsi1_clk);
		goto clk_disable;
	}
	/*
	 * ES = Energy Save, or LP = Low Power clocks
	 * These clocks are also used for TV out.
	 */
	mcde->dsi0es_clk = devm_clk_get(dev, "dsi0es");
	if (IS_ERR(mcde->dsi0es_clk)) {
		dev_err(dev, "unable to get DSI0ES clock\n");
		ret = PTR_ERR(mcde->dsi0es_clk);
		goto clk_disable;
	}
	mcde->dsi1es_clk = devm_clk_get(dev, "dsi1es");
	if (IS_ERR(mcde->dsi1es_clk)) {
		dev_err(dev, "unable to get DSI1ES clock\n");
		ret = PTR_ERR(mcde->dsi1es_clk);
		goto clk_disable;
	}
	mcde->dsi2es_clk = devm_clk_get(dev, "dsi2es");
	if (IS_ERR(mcde->dsi2es_clk)) {
		dev_err(dev, "unable to get DSI2ES clock\n");
		ret = PTR_ERR(mcde->dsi2es_clk);
		goto clk_disable;
	}
	mcde->lcd_clk = devm_clk_get(dev, "lcd");
	if (IS_ERR(mcde->lcd_clk)) {
		dev_err(dev, "unable to get LCD clock\n");
		ret = PTR_ERR(mcde->lcd_clk);
		goto clk_disable;
	}
	mcde->hdmi_clk = devm_clk_get(dev, "hdmi");
	if (IS_ERR(mcde->hdmi_clk)) {
		dev_err(dev, "unable to get HDMI clock\n");
		ret = PTR_ERR(mcde->hdmi_clk);
		goto clk_disable;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mcde->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(mcde->regs)) {
		dev_err(dev, "no MCDE regs\n");
		ret = -EINVAL;
		goto clk_disable;
	}

	irq = platform_get_irq(pdev, 0);
	if (!irq) {
		ret = -EINVAL;
		goto clk_disable;
	}

	ret = devm_request_irq(dev, irq, mcde_irq, 0, "mcde", mcde);
	if (ret) {
		dev_err(dev, "failed to request irq %d\n", ret);
		goto clk_disable;
	}

	/*
	 * Check hardware revision, we only support U8500v2 version
	 * as this was the only version used for mass market deployment,
	 * but surely you can add more versions if you have them and
	 * need them.
	 */
	pid = readl(mcde->regs + MCDE_PID);
	dev_info(dev, "found MCDE HW revision %d.%d (dev %d, metal fix %d)\n",
		 (pid & MCDE_PID_MAJOR_VERSION_MASK)
		 >> MCDE_PID_MAJOR_VERSION_SHIFT,
		 (pid & MCDE_PID_MINOR_VERSION_MASK)
		 >> MCDE_PID_MINOR_VERSION_SHIFT,
		 (pid & MCDE_PID_DEVELOPMENT_VERSION_MASK)
		 >> MCDE_PID_DEVELOPMENT_VERSION_SHIFT,
		 (pid & MCDE_PID_METALFIX_VERSION_MASK)
		 >> MCDE_PID_METALFIX_VERSION_SHIFT);
	if (pid != 0x03000800) {
		dev_err(dev, "unsupported hardware revision\n");
		ret = -ENODEV;
		goto clk_disable;
	}

	/* Set up the main control, watermark level at 7 */
	val = 7 << MCDE_CONF0_IFIFOCTRLWTRMRKLVL_SHIFT;
	/* 24 bits DPI: connect LSB Ch B to D[0:7] */
	val |= 3 << MCDE_CONF0_OUTMUX0_SHIFT;
	/* TV out: connect LSB Ch B to D[8:15] */
	val |= 3 << MCDE_CONF0_OUTMUX1_SHIFT;
	/* Don't care about this muxing */
	val |= 0 << MCDE_CONF0_OUTMUX2_SHIFT;
	/* 24 bits DPI: connect MID Ch B to D[24:31] */
	val |= 4 << MCDE_CONF0_OUTMUX3_SHIFT;
	/* 5: 24 bits DPI: connect MSB Ch B to D[32:39] */
	val |= 5 << MCDE_CONF0_OUTMUX4_SHIFT;
	/* Syncmux bits zero: DPI channel A and B on output pins A and B resp */
	writel(val, mcde->regs + MCDE_CONF0);

	/* Enable automatic clock gating */
	val = readl(mcde->regs + MCDE_CR);
	val |= MCDE_CR_MCDEEN | MCDE_CR_AUTOCLKG_EN;
	writel(val, mcde->regs + MCDE_CR);

	/* Clear any pending interrupts */
	mcde_display_disable_irqs(mcde);
	writel(0, mcde->regs + MCDE_IMSCERR);
	writel(0xFFFFFFFF, mcde->regs + MCDE_RISERR);

	/* Spawn child devices for the DSI ports */
	devm_of_platform_populate(dev);

	/* Create something that will match the subdrivers when we bind */
	for (i = 0; i < ARRAY_SIZE(mcde_component_drivers); i++) {
		struct device_driver *drv = &mcde_component_drivers[i]->driver;
		struct device *p = NULL, *d;

		while ((d = bus_find_device(&platform_bus_type, p, drv,
					    (void *)platform_bus_type.match))) {
			put_device(p);
			component_match_add(dev, &match, mcde_compare_dev, d);
			p = d;
		}
		put_device(p);
	}
	if (IS_ERR(match)) {
		dev_err(dev, "could not create component match\n");
		ret = PTR_ERR(match);
		goto clk_disable;
	}
	ret = component_master_add_with_match(&pdev->dev, &mcde_drm_comp_ops,
					      match);
	if (ret) {
		dev_err(dev, "faule to add component master\n");
		goto clk_disable;
	}
	return 0;

clk_disable:
	clk_disable_unprepare(mcde->mcde_clk);
regulator_off:
	regulator_disable(mcde->vana);
regulator_epod_off:
	regulator_disable(mcde->epod);
dev_unref:
	drm_dev_put(drm);
	return ret;

}

static int mcde_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);
	struct mcde *mcde = drm->dev_private;

	component_master_del(&pdev->dev, &mcde_drm_comp_ops);
	clk_disable_unprepare(mcde->mcde_clk);
	regulator_disable(mcde->vana);
	regulator_disable(mcde->epod);
	drm_dev_put(drm);

	return 0;
}

static const struct of_device_id mcde_of_match[] = {
	{
		.compatible = "ste,mcde",
	},
	{},
};

static struct platform_driver mcde_driver = {
	.driver = {
		.name           = "mcde",
		.of_match_table = of_match_ptr(mcde_of_match),
	},
	.probe = mcde_probe,
	.remove = mcde_remove,
};

static struct platform_driver *const component_drivers[] = {
	&mcde_dsi_driver,
};

static int __init mcde_drm_register(void)
{
	int ret;

	ret = platform_register_drivers(component_drivers,
					ARRAY_SIZE(component_drivers));
	if (ret)
		return ret;

	return platform_driver_register(&mcde_driver);
}

static void __exit mcde_drm_unregister(void)
{
	platform_unregister_drivers(component_drivers,
				    ARRAY_SIZE(component_drivers));
	platform_driver_unregister(&mcde_driver);
}

module_init(mcde_drm_register);
module_exit(mcde_drm_unregister);

MODULE_ALIAS("platform:mcde-drm");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("Linus Walleij <linus.walleij@linaro.org>");
MODULE_LICENSE("GPL");
