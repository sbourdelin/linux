// SPDX-License-Identifier: GPL-2.0+
/* Copyright (c) 2018 Jernej Skrabec <jernej.skrabec@siol.net> */

#include <drm/drmP.h>

#include <dt-bindings/clock/sun8i-tcon-top.h>

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

#include "sun8i_tcon_top.h"

#define TCON_TOP_PORT_SEL_REG		0x1C
#define TCON_TOP_PORT_DE0_MSK			GENMASK(1, 0)
#define TCON_TOP_PORT_DE1_MSK			GENMASK(5, 4)
#define TCON_TOP_PORT_TCON_LCD0			0
#define TCON_TOP_PORT_TCON_LCD1			1
#define TCON_TOP_PORT_TCON_TV0			2
#define TCON_TOP_PORT_TCON_TV1			3

#define TCON_TOP_GATE_SRC_REG		0x20
#define TCON_TOP_HDMI_SRC_MSK			GENMASK(29, 28)
#define TCON_TOP_HDMI_SRC_NONE			0
#define TCON_TOP_HDMI_SRC_TCON_TV0		1
#define TCON_TOP_HDMI_SRC_TCON_TV1		2
#define TCON_TOP_TCON_TV1_GATE			24
#define TCON_TOP_TCON_TV0_GATE			20
#define TCON_TOP_TCON_DSI_GATE			16

#define CLK_NUM					3

struct sun8i_tcon_top {
	struct clk		*bus;
	void __iomem		*regs;
	struct reset_control	*rst;

	/*
	 * spinlock is used for locking access to registers from different
	 * places - tcon driver and clk subsystem.
	 */
	spinlock_t		reg_lock;
};

struct sun8i_tcon_top_gate {
	const char	*name;
	u8		bit;
	int		index;
};

static const struct sun8i_tcon_top_gate gates[] = {
	{"bus-tcon-top-dsi", TCON_TOP_TCON_DSI_GATE, CLK_BUS_TCON_TOP_DSI},
	{"bus-tcon-top-tv0", TCON_TOP_TCON_TV0_GATE, CLK_BUS_TCON_TOP_TV0},
	{"bus-tcon-top-tv1", TCON_TOP_TCON_TV1_GATE, CLK_BUS_TCON_TOP_TV1},
};

void sun8i_tcon_top_set_hdmi_src(struct sun8i_tcon_top *tcon_top, int tcon)
{
	unsigned long flags;
	u32 val;

	if (tcon > 1) {
		DRM_ERROR("TCON index is too high!\n");
		return;
	}

	spin_lock_irqsave(&tcon_top->reg_lock, flags);

	val = readl(tcon_top->regs + TCON_TOP_GATE_SRC_REG);
	val &= ~TCON_TOP_HDMI_SRC_MSK;
	val |= FIELD_PREP(TCON_TOP_HDMI_SRC_MSK,
			  TCON_TOP_HDMI_SRC_TCON_TV0 + tcon);
	writel(val, tcon_top->regs + TCON_TOP_GATE_SRC_REG);

	spin_unlock_irqrestore(&tcon_top->reg_lock, flags);
}

void sun8i_tcon_top_de_config(struct sun8i_tcon_top *tcon_top,
			      int mixer, enum tcon_type tcon_type, int tcon)
{
	unsigned long flags;
	u32 val, reg;

	if (mixer > 1) {
		DRM_ERROR("Mixer index is too high!\n");
		return;
	}

	if (tcon > 1) {
		DRM_ERROR("TCON index is too high!\n");
		return;
	}

	switch (tcon_type) {
	case tcon_type_lcd:
		val = TCON_TOP_PORT_TCON_LCD0 + tcon;
		break;
	case tcon_type_tv:
		val = TCON_TOP_PORT_TCON_TV0 + tcon;
		break;
	default:
		DRM_ERROR("Invalid TCON type!\n");
		return;
	}

	spin_lock_irqsave(&tcon_top->reg_lock, flags);

	reg = readl(tcon_top->regs + TCON_TOP_PORT_SEL_REG);
	if (mixer == 0) {
		reg &= ~TCON_TOP_PORT_DE0_MSK;
		reg |= FIELD_PREP(TCON_TOP_PORT_DE0_MSK, val);
	} else {
		reg &= ~TCON_TOP_PORT_DE1_MSK;
		reg |= FIELD_PREP(TCON_TOP_PORT_DE1_MSK, val);
	}
	writel(reg, tcon_top->regs + TCON_TOP_PORT_SEL_REG);

	spin_unlock_irqrestore(&tcon_top->reg_lock, flags);
}

static int sun8i_tcon_top_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	struct sun8i_tcon_top *tcon_top;
	struct device *dev = &pdev->dev;
	struct resource *res;
	int ret, i;

	tcon_top = devm_kzalloc(dev, sizeof(*tcon_top), GFP_KERNEL);
	if (!tcon_top)
		return -ENOMEM;

	clk_data = devm_kzalloc(&pdev->dev, sizeof(*clk_data) +
				sizeof(*clk_data->hws) * CLK_NUM,
				GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;

	spin_lock_init(&tcon_top->reg_lock);

	tcon_top->rst = devm_reset_control_get(dev, "rst");
	if (IS_ERR(tcon_top->rst)) {
		dev_err(dev, "Couldn't get our reset line\n");
		return PTR_ERR(tcon_top->rst);
	}

	tcon_top->bus = devm_clk_get(dev, "bus");
	if (IS_ERR(tcon_top->bus)) {
		dev_err(dev, "Couldn't get the bus clock\n");
		return PTR_ERR(tcon_top->bus);
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	tcon_top->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(tcon_top->regs))
		return PTR_ERR(tcon_top->regs);

	ret = reset_control_deassert(tcon_top->rst);
	if (ret) {
		dev_err(dev, "Could not deassert ctrl reset control\n");
		return ret;
	}

	ret = clk_prepare_enable(tcon_top->bus);
	if (ret) {
		dev_err(dev, "Could not enable bus clock\n");
		goto err_assert_reset;
	}

	/*
	 * Default register values might have some reserved bits set, which
	 * prevents TCON TOP from working properly. Set them to 0 here.
	 */
	writel(0, tcon_top->regs + TCON_TOP_PORT_SEL_REG);
	writel(0, tcon_top->regs + TCON_TOP_GATE_SRC_REG);

	for (i = 0; i < CLK_NUM; i++) {
		const char *parent_name = "bus-tcon-top";
		struct clk_init_data init;
		struct clk_gate *gate;

		gate = devm_kzalloc(dev, sizeof(*gate), GFP_KERNEL);
		if (!gate) {
			ret = -ENOMEM;
			goto err_disable_clock;
		}

		init.name = gates[i].name;
		init.ops = &clk_gate_ops;
		init.flags = CLK_IS_BASIC;
		init.parent_names = &parent_name;
		init.num_parents = 1;

		gate->reg = tcon_top->regs + TCON_TOP_GATE_SRC_REG;
		gate->bit_idx = gates[i].bit;
		gate->lock = &tcon_top->reg_lock;
		gate->hw.init = &init;

		ret = devm_clk_hw_register(dev, &gate->hw);
		if (ret)
			goto err_disable_clock;

		clk_data->hws[gates[i].index] = &gate->hw;
	}

	clk_data->num = CLK_NUM;

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clk_data);
	if (ret)
		goto err_disable_clock;

	platform_set_drvdata(pdev, tcon_top);

	return 0;

err_disable_clock:
	clk_disable_unprepare(tcon_top->bus);
err_assert_reset:
	reset_control_assert(tcon_top->rst);

	return ret;
}

static int sun8i_tcon_top_remove(struct platform_device *pdev)
{
	struct sun8i_tcon_top *tcon_top = platform_get_drvdata(pdev);

	clk_disable_unprepare(tcon_top->bus);
	reset_control_assert(tcon_top->rst);

	return 0;
}

const struct of_device_id sun8i_tcon_top_of_table[] = {
	{ .compatible = "allwinner,sun8i-r40-tcon-top" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sun8i_tcon_top_of_table);

static struct platform_driver sun8i_tcon_top_platform_driver = {
	.probe		= sun8i_tcon_top_probe,
	.remove		= sun8i_tcon_top_remove,
	.driver		= {
		.name		= "sun8i-tcon-top",
		.of_match_table	= sun8i_tcon_top_of_table,
	},
};
module_platform_driver(sun8i_tcon_top_platform_driver);

MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@siol.net>");
MODULE_DESCRIPTION("Allwinner R40 TCON TOP driver");
MODULE_LICENSE("GPL");
