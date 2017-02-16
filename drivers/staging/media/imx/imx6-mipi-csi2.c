/*
 * MIPI CSI-2 Receiver Subdev for Freescale i.MX6 SOC.
 *
 * Copyright (c) 2012-2017 Mentor Graphics Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <media/v4l2-device.h>
#include <media/v4l2-of.h>
#include <media/v4l2-subdev.h>
#include "imx-media.h"

/*
 * there must be 5 pads: 1 input pad from sensor, and
 * the 4 virtual channel output pads
 */
#define CSI2_SINK_PAD       0
#define CSI2_NUM_SINK_PADS  1
#define CSI2_NUM_SRC_PADS   4
#define CSI2_NUM_PADS       5

struct csi2_dev {
	struct device          *dev;
	struct v4l2_subdev      sd;
	struct media_pad       pad[CSI2_NUM_PADS];
	struct v4l2_mbus_framefmt format_mbus;
	struct clk             *dphy_clk;
	struct clk             *cfg_clk;
	struct clk             *pix_clk; /* what is this? */
	void __iomem           *base;
	struct v4l2_of_bus_mipi_csi2 bus;
	bool                    on;
	bool                    stream_on;
	bool                    src_linked;
	bool                    sink_linked[CSI2_NUM_SRC_PADS];
};

#define DEVICE_NAME "imx6-mipi-csi2"

/* Register offsets */
#define CSI2_VERSION            0x000
#define CSI2_N_LANES            0x004
#define CSI2_PHY_SHUTDOWNZ      0x008
#define CSI2_DPHY_RSTZ          0x00c
#define CSI2_RESETN             0x010
#define CSI2_PHY_STATE          0x014
#define PHY_STOPSTATEDATA_BIT   4
#define PHY_STOPSTATEDATA(n)    BIT(PHY_STOPSTATEDATA_BIT + (n))
#define PHY_RXCLKACTIVEHS       BIT(8)
#define PHY_RXULPSCLKNOT        BIT(9)
#define PHY_STOPSTATECLK        BIT(10)
#define CSI2_DATA_IDS_1         0x018
#define CSI2_DATA_IDS_2         0x01c
#define CSI2_ERR1               0x020
#define CSI2_ERR2               0x024
#define CSI2_MSK1               0x028
#define CSI2_MSK2               0x02c
#define CSI2_PHY_TST_CTRL0      0x030
#define PHY_TESTCLR		BIT(0)
#define PHY_TESTCLK		BIT(1)
#define CSI2_PHY_TST_CTRL1      0x034
#define PHY_TESTEN		BIT(16)
#define CSI2_SFT_RESET          0xf00

static inline struct csi2_dev *sd_to_dev(struct v4l2_subdev *sdev)
{
	return container_of(sdev, struct csi2_dev, sd);
}

static void csi2_enable(struct csi2_dev *csi2, bool enable)
{
	if (enable) {
		writel(0x1, csi2->base + CSI2_PHY_SHUTDOWNZ);
		writel(0x1, csi2->base + CSI2_DPHY_RSTZ);
		writel(0x1, csi2->base + CSI2_RESETN);
	} else {
		writel(0x0, csi2->base + CSI2_PHY_SHUTDOWNZ);
		writel(0x0, csi2->base + CSI2_DPHY_RSTZ);
		writel(0x0, csi2->base + CSI2_RESETN);
	}
}

static void csi2_set_lanes(struct csi2_dev *csi2)
{
	int lanes = csi2->bus.num_data_lanes;

	writel(lanes - 1, csi2->base + CSI2_N_LANES);
}

static void dw_mipi_csi2_phy_write(struct csi2_dev *csi2,
				   u32 test_code, u32 test_data)
{
	/* Clear PHY test interface */
	writel(PHY_TESTCLR, csi2->base + CSI2_PHY_TST_CTRL0);
	writel(0x0, csi2->base + CSI2_PHY_TST_CTRL1);
	writel(0x0, csi2->base + CSI2_PHY_TST_CTRL0);

	/* Raise test interface strobe signal */
	writel(PHY_TESTCLK, csi2->base + CSI2_PHY_TST_CTRL0);

	/* Configure address write on falling edge and lower strobe signal */
	writel(PHY_TESTEN | test_code, csi2->base + CSI2_PHY_TST_CTRL1);
	writel(0x0, csi2->base + CSI2_PHY_TST_CTRL0);

	/* Configure data write on rising edge and raise strobe signal */
	writel(test_data, csi2->base + CSI2_PHY_TST_CTRL1);
	writel(PHY_TESTCLK, csi2->base + CSI2_PHY_TST_CTRL0);

	/* Clear strobe signal */
	writel(0x0, csi2->base + CSI2_PHY_TST_CTRL0);
}

static void csi2_dphy_init(struct csi2_dev *csi2)
{
	/*
	 * FIXME: 0x14 is derived from a fixed D-PHY reference
	 * clock from the HSI_TX PLL, and a fixed target lane max
	 * bandwidth of 300 Mbps. This value should be derived
	 * from the dphy_clk rate and the desired max lane bandwidth.
	 * See drivers/gpu/drm/rockchip/dw-mipi-dsi.c for more info
	 * on how this value is derived.
	 */
	dw_mipi_csi2_phy_write(csi2, 0x44, 0x14);
}

/*
 * Waits for ultra-low-power state on D-PHY clock lane. This is currently
 * unused and may not be needed at all, but keep around just in case.
 */
static int __maybe_unused csi2_dphy_wait_ulp(struct csi2_dev *csi2)
{
	u32 reg;
	int ret;

	/* wait for ULP on clock lane */
	ret = readl_poll_timeout(csi2->base + CSI2_PHY_STATE, reg,
				 !(reg & PHY_RXULPSCLKNOT), 0, 500000);
	if (ret) {
		v4l2_err(&csi2->sd, "ULP timeout, phy_state = 0x%08x\n", reg);
		return ret;
	}

	/* wait until no errors on bus */
	ret = readl_poll_timeout(csi2->base + CSI2_ERR1, reg,
				 reg == 0x0, 0, 500000);
	if (ret) {
		v4l2_err(&csi2->sd, "stable bus timeout, err1 = 0x%08x\n", reg);
		return ret;
	}

	return 0;
}

/*
 * Waits for low-power LP-11 state (aka STOPSTATE) on data and clock
 * lanes.
 */
static int csi2_dphy_wait_stopstate(struct csi2_dev *csi2)
{
	u32 mask, reg;
	int ret;

	mask = PHY_STOPSTATECLK |
		((csi2->bus.num_data_lanes - 1) << PHY_STOPSTATEDATA_BIT);

	ret = readl_poll_timeout(csi2->base + CSI2_PHY_STATE, reg,
				 (reg & mask) == mask, 0, 500000);
	if (ret) {
		v4l2_err(&csi2->sd, "LP-11 timeout, phy_state = 0x%08x\n", reg);
		return ret;
	}

	return 0;
}

/*
 * Wait for active clock on the clock lane.
 *
 * FIXME: Currently unused, but it should be! It should be called
 * from csi2_s_stream() below, at stream ON, but the required sequence
 * of MIPI CSI-2 startup does not allow for an opportunity for this to
 * be called. The sequence as specified in the i.MX6 reference manual
 * is as follows:
 *
 * 1. Deassert presetn signal (global reset).
 *        It's not clear what this "global reset" signal is (maybe APB
 *        global reset), but in any case this step corresponds to
 *        csi2_s_power(ON) here.
 *
 * 2. Configure MIPI Camera Sensor to put all Tx lanes in PL-11 state.
 *        This must be carried out by the MIPI sensor's s_power(ON) subdev
 *        op.
 *
 * 3. D-PHY initialization.
 * 4. CSI2 Controller programming (Set N_LANES, deassert PHY_SHUTDOWNZ,
 *    deassert PHY_RSTZ, deassert CSI2_RESETN).
 * 5. Read the PHY status register (PHY_STATE) to confirm that all data and
 *    clock lanes of the D-PHY are in Stop State.
 *        These steps (3,4,5) are carried out by csi2_s_stream(ON) here.
 *
 * 6. Configure the MIPI Camera Sensor to start transmitting a clock on the
 *    D-PHY clock lane.
 *        This must be carried out by the MIPI sensor's s_stream(ON) subdev
 *        op.
 * 
 * 7. CSI2 Controller programming - Read the PHY status register (PHY_STATE)
 *    to confirm that the D-PHY is receiving a clock on the D-PHY clock lane.
 *        This is implemented by this unused function, and _should_ be called
 *        by csi2_s_stream(ON) here, but csi2_s_stream(ON) has been taken up
 *        by steps 3,4,5 above already.
 *
 * In summary, a temporary solution would require a hard-coded delay in the
 * MIPI sensor's s_stream(ON) op, to allow time for a stable clock lane.
 *
 * A longer term solution might be to create a new subdev op, perhaps
 * called prepare_stream, that can be implemented here, and would be
 * assigned steps 3,4,5. Then csi2_s_stream(ON) would become available
 * as step 7.
 */
static int __maybe_unused csi2_dphy_wait_clock_lane(struct csi2_dev *csi2)
{
	u32 reg;
	int ret;

	ret = readl_poll_timeout(csi2->base + CSI2_PHY_STATE, reg,
				 (reg & PHY_RXCLKACTIVEHS), 0, 500000);
	if (ret) {
		v4l2_err(&csi2->sd, "clock lane timeout, phy_state = 0x%08x\n",
			 reg);
		return ret;
	}

	return 0;
}

/*
 * V4L2 subdev operations.
 */

/* Startup Sequence Step 1 */
static int csi2_s_power(struct v4l2_subdev *sd, int on)
{
	struct csi2_dev *csi2 = sd_to_dev(sd);
	int ret;

	if (on && !csi2->on) {
		dev_dbg(csi2->dev, "power ON\n");
		ret = clk_prepare_enable(csi2->cfg_clk);
		if (ret)
			return ret;
		ret = clk_prepare_enable(csi2->dphy_clk);
		if (ret)
			goto cfg_clk_off;
	} else if (!on && csi2->on) {
		dev_dbg(csi2->dev, "power OFF\n");
		clk_disable_unprepare(csi2->dphy_clk);
		clk_disable_unprepare(csi2->cfg_clk);
	}

	csi2->on = on;

	return 0;

cfg_clk_off:
	clk_disable_unprepare(csi2->cfg_clk);
	return ret;
}

/* Startup Sequence Steps 3, 4, 5 */
static int csi2_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct csi2_dev *csi2 = sd_to_dev(sd);
	int i, ret = 0;

	if (!csi2->src_linked)
		return -EPIPE;
	for (i = 0; i < CSI2_NUM_SRC_PADS; i++) {
		if (csi2->sink_linked[i])
			break;
	}
	if (i >= CSI2_NUM_SRC_PADS)
		return -EPIPE;

	if (enable && !csi2->stream_on) {
		dev_dbg(csi2->dev, "stream ON\n");

		ret = clk_prepare_enable(csi2->pix_clk);
		if (ret)
			return ret;

		/* Step 3 */
		csi2_dphy_init(csi2);
		/* Step 4 */
		csi2_set_lanes(csi2);
		csi2_enable(csi2, true);

		/* Step 5 */
		ret = csi2_dphy_wait_stopstate(csi2);
		if (ret) {
			csi2_enable(csi2, false);
			clk_disable_unprepare(csi2->pix_clk);
			return ret;
		}
	} else if (!enable && csi2->stream_on) {
		dev_dbg(csi2->dev, "stream OFF\n");
		csi2_enable(csi2, false);
		clk_disable_unprepare(csi2->pix_clk);
	}

	csi2->stream_on = enable;
	return 0;
}

static int csi2_link_setup(struct media_entity *entity,
			   const struct media_pad *local,
			   const struct media_pad *remote, u32 flags)
{
	struct v4l2_subdev *sd = media_entity_to_v4l2_subdev(entity);
	struct csi2_dev *csi2 = sd_to_dev(sd);
	struct v4l2_subdev *remote_sd;

	dev_dbg(csi2->dev, "link setup %s -> %s", remote->entity->name,
		local->entity->name);

	remote_sd = media_entity_to_v4l2_subdev(remote->entity);

	if (local->flags & MEDIA_PAD_FL_SOURCE) {
		if (flags & MEDIA_LNK_FL_ENABLED) {
			if (csi2->sink_linked[local->index])
				return -EBUSY;
			csi2->sink_linked[local->index] = true;
		} else {
			csi2->sink_linked[local->index] = false;
		}
	} else {
		if (flags & MEDIA_LNK_FL_ENABLED) {
			if (csi2->src_linked)
				return -EBUSY;
			csi2->src_linked = true;
		} else {
			csi2->src_linked = false;
		}
	}

	return 0;
}

static int csi2_get_fmt(struct v4l2_subdev *sd,
			struct v4l2_subdev_pad_config *cfg,
			struct v4l2_subdev_format *sdformat)
{
	struct csi2_dev *csi2 = sd_to_dev(sd);
	struct v4l2_mbus_framefmt *fmt;

	if (sdformat->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt = v4l2_subdev_get_try_format(&csi2->sd, cfg,
						 sdformat->pad);
	else
		fmt = &csi2->format_mbus;

	sdformat->format = *fmt;

	return 0;
}

static int csi2_set_fmt(struct v4l2_subdev *sd,
			struct v4l2_subdev_pad_config *cfg,
			struct v4l2_subdev_format *sdformat)
{
	struct csi2_dev *csi2 = sd_to_dev(sd);

	if (sdformat->pad >= CSI2_NUM_PADS)
		return -EINVAL;

	if (csi2->stream_on)
		return -EBUSY;

	/* Output pads mirror active input pad, no limits on input pads */
	if (sdformat->pad != CSI2_SINK_PAD)
		sdformat->format = csi2->format_mbus;

	if (sdformat->which == V4L2_SUBDEV_FORMAT_TRY)
		cfg->try_fmt = sdformat->format;
	else
		csi2->format_mbus = sdformat->format;

	return 0;
}

/*
 * retrieve our pads parsed from the OF graph by the media device
 */
static int csi2_registered(struct v4l2_subdev *sd)
{
	struct csi2_dev *csi2 = sd_to_dev(sd);
	int i, ret;

	for (i = 0; i < CSI2_NUM_PADS; i++) {
		csi2->pad[i].flags = (i == CSI2_SINK_PAD) ?
		MEDIA_PAD_FL_SINK : MEDIA_PAD_FL_SOURCE;
	}

	/* set a default mbus format  */
	ret = imx_media_init_mbus_fmt(&csi2->format_mbus,
				      640, 480, 0, V4L2_FIELD_NONE, NULL);
	if (ret)
		return ret;

	return media_entity_pads_init(&sd->entity, CSI2_NUM_PADS, csi2->pad);
}

static struct media_entity_operations csi2_entity_ops = {
	.link_setup = csi2_link_setup,
	.link_validate = v4l2_subdev_link_validate,
};

static struct v4l2_subdev_core_ops csi2_core_ops = {
	.s_power = csi2_s_power,
};

static struct v4l2_subdev_video_ops csi2_video_ops = {
	.s_stream = csi2_s_stream,
};

static struct v4l2_subdev_pad_ops csi2_pad_ops = {
	.get_fmt = csi2_get_fmt,
	.set_fmt = csi2_set_fmt,
};

static struct v4l2_subdev_ops csi2_subdev_ops = {
	.core = &csi2_core_ops,
	.video = &csi2_video_ops,
	.pad = &csi2_pad_ops,
};

static struct v4l2_subdev_internal_ops csi2_internal_ops = {
	.registered = csi2_registered,
};

static int csi2_parse_endpoints(struct csi2_dev *csi2)
{
	struct device_node *node = csi2->dev->of_node;
	struct device_node *epnode;
	struct v4l2_of_endpoint ep;

	epnode = of_graph_get_endpoint_by_regs(node, 0, -1);
	if (!epnode) {
		v4l2_err(&csi2->sd, "failed to get sink endpoint node\n");
		return -EINVAL;
	}

	v4l2_of_parse_endpoint(epnode, &ep);
	of_node_put(epnode);

	if (ep.bus_type != V4L2_MBUS_CSI2) {
		v4l2_err(&csi2->sd, "invalid bus type, must be MIPI CSI2\n");
		return -EINVAL;
	}

	csi2->bus = ep.bus.mipi_csi2;

	dev_dbg(csi2->dev, "data lanes: %d\n", csi2->bus.num_data_lanes);
	dev_dbg(csi2->dev, "flags: 0x%08x\n", csi2->bus.flags);
	return 0;
}

static int csi2_probe(struct platform_device *pdev)
{
	struct csi2_dev *csi2;
	struct resource *res;
	int ret;

	csi2 = devm_kzalloc(&pdev->dev, sizeof(*csi2), GFP_KERNEL);
	if (!csi2)
		return -ENOMEM;

	csi2->dev = &pdev->dev;

	v4l2_subdev_init(&csi2->sd, &csi2_subdev_ops);
	v4l2_set_subdevdata(&csi2->sd, &pdev->dev);
	csi2->sd.internal_ops = &csi2_internal_ops;
	csi2->sd.entity.ops = &csi2_entity_ops;
	csi2->sd.dev = &pdev->dev;
	csi2->sd.owner = THIS_MODULE;
	csi2->sd.flags = V4L2_SUBDEV_FL_HAS_DEVNODE;
	strcpy(csi2->sd.name, DEVICE_NAME);
	csi2->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	csi2->sd.grp_id = IMX_MEDIA_GRP_ID_CSI2;

	ret = csi2_parse_endpoints(csi2);
	if (ret)
		return ret;

	csi2->cfg_clk = devm_clk_get(&pdev->dev, "cfg");
	if (IS_ERR(csi2->cfg_clk)) {
		v4l2_err(&csi2->sd, "failed to get cfg clock\n");
		ret = PTR_ERR(csi2->cfg_clk);
		return ret;
	}

	csi2->dphy_clk = devm_clk_get(&pdev->dev, "dphy");
	if (IS_ERR(csi2->dphy_clk)) {
		v4l2_err(&csi2->sd, "failed to get dphy clock\n");
		ret = PTR_ERR(csi2->dphy_clk);
		return ret;
	}

	csi2->pix_clk = devm_clk_get(&pdev->dev, "pix");
	if (IS_ERR(csi2->pix_clk)) {
		v4l2_err(&csi2->sd, "failed to get pixel clock\n");
		ret = PTR_ERR(csi2->pix_clk);
		return ret;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		v4l2_err(&csi2->sd, "failed to get platform resources\n");
		return -ENODEV;
	}

	csi2->base = devm_ioremap(&pdev->dev, res->start, PAGE_SIZE);
	if (!csi2->base) {
		v4l2_err(&csi2->sd, "failed to map CSI-2 registers\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, &csi2->sd);

	return v4l2_async_register_subdev(&csi2->sd);
}

static int csi2_remove(struct platform_device *pdev)
{
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);

	csi2_s_power(sd, 0);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);

	return 0;
}

static const struct of_device_id csi2_dt_ids[] = {
	{ .compatible = "fsl,imx6-mipi-csi2", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, csi2_dt_ids);

static struct platform_driver csi2_driver = {
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = csi2_dt_ids,
	},
	.probe = csi2_probe,
	.remove = csi2_remove,
};

module_platform_driver(csi2_driver);

MODULE_DESCRIPTION("i.MX5/6 MIPI CSI-2 Receiver driver");
MODULE_AUTHOR("Steve Longerbeam <steve_longerbeam@mentor.com>");
MODULE_LICENSE("GPL");
