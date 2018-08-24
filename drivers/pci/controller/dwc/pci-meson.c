// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Amlogic MESON SoCs
 *
 * Copyright (c) 2018 Amlogic, inc.
 * Author: Yue Wang <yue.wang@amlogic.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/resource.h>
#include <linux/types.h>
#include <linux/reset.h>

#include "pcie-designware.h"

#define to_meson_pcie(x) dev_get_drvdata((x)->dev)

/* External local bus interface registers */
#define PLR_OFFSET			0x700
#define PCIE_PORT_LINK_CTRL_OFF		(PLR_OFFSET + 0x10)
#define FAST_LINK_MODE			BIT(7)
#define LINK_CAPABLE_MASK		GENMASK(21, 16)
#define LINK_CAPABLE_X1			BIT(16)

#define PCIE_GEN2_CTRL_OFF		(PLR_OFFSET + 0x10c)
#define NUM_OF_LANES_MASK		GENMASK(12, 8)
#define NUM_OF_LANES_X1			BIT(8)
#define DIRECT_SPEED_CHANGE		BIT(17)

#define TYPE1_HDR_OFFSET		0x0
#define PCIE_STATUS_COMMAND		(TYPE1_HDR_OFFSET + 0x04)
#define PCI_IO_EN			BIT(0)
#define PCI_MEM_SPACE_EN		BIT(1)
#define PCI_BUS_MASTER_EN		BIT(2)

#define PCIE_BASE_ADDR0			(TYPE1_HDR_OFFSET + 0x10)
#define PCIE_BASE_ADDR1			(TYPE1_HDR_OFFSET + 0x14)

#define PCIE_CAP_OFFSET			0x70
#define PCIE_DEV_CTRL_DEV_STUS		(PCIE_CAP_OFFSET + 0x08)
#define PCIE_CAP_MAX_PAYLOAD_MASK	GENMASK(7, 5)
#define PCIE_CAP_MAX_PAYLOAD_SIZE(x)	((x) << 5)
#define PCIE_CAP_MAX_READ_REQ_MASK	GENMASK(14, 12)
#define PCIE_CAP_MAX_READ_REQ_SIZE(x)	((x) << 12)

#define PCI_CLASS_REVISION_MASK		GENMASK(7, 0)

/* PCIe specific config registers */
#define PCIE_CFG0			0x0
#define APP_LTSSM_ENABLE		BIT(7)

#define PCIE_CFG_STATUS12		0x30
#define IS_SMLH_LINK_UP(x)		((x) & (1 << 6))
#define IS_RDLH_LINK_UP(x)		((x) & (1 << 16))
#define IS_LTSSM_UP(x)			((((x) >> 10) & 0x1f) == 0x11)

#define PCIE_CFG_STATUS17		0x44
#define PM_CURRENT_STATE(x)		(((x) >> 7) & 0x1)

#define WAIT_LINKUP_TIMEOUT		2000
#define PORT_CLK_RATE			100000000UL
#define MAX_PAYLOAD_SIZE		256
#define MAX_READ_REQ_SIZE		256

enum pcie_data_rate {
	PCIE_GEN1,
	PCIE_GEN2,
	PCIE_GEN3,
	PCIE_GEN4
};

struct meson_pcie_mem_res {
	void __iomem *elbi_base; /* DT 0th resource */
	void __iomem *cfg_base; /* DT 2nd resource */
};

struct meson_pcie_clk_res {
	struct clk *clk;
	struct clk *mipi_gate;
	struct clk *port_clk;
	struct clk *general_clk;
};

struct meson_pcie_rc_reset {
	struct reset_control *port;
	struct reset_control *apb;
};

struct meson_pcie {
	struct dw_pcie pci;
	struct meson_pcie_mem_res mem_res;
	struct meson_pcie_clk_res clk_res;
	struct meson_pcie_rc_reset mrst;
	struct gpio_desc *reset_gpio;

	struct phy *phy;
	enum of_gpio_flags gpio_flag;
	int pcie_num;
	u32 port_num;
	u32 device_attch;
};

static int meson_pcie_get_mem(struct platform_device *pdev,
			      struct meson_pcie *mp)
{
	struct device *dev = mp->pci.dev;
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "elbi");
	if (!res)
		return -ENODEV;

	mp->mem_res.elbi_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(mp->mem_res.elbi_base))
		return PTR_ERR(mp->mem_res.elbi_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
	if (!res)
		return -ENODEV;

	mp->mem_res.cfg_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(mp->mem_res.cfg_base))
		return PTR_ERR(mp->mem_res.cfg_base);

	return 0;
}

static void meson_pcie_rc_reset(struct meson_pcie *mp)
{
	struct meson_pcie_rc_reset *mrst = &mp->mrst;

	reset_control_assert(mrst->port);
	reset_control_assert(mrst->apb);
	udelay(400);
	reset_control_deassert(mrst->port);
	reset_control_deassert(mrst->apb);
	udelay(500);
}

static int meson_pcie_init_clk(struct meson_pcie *mp)
{
	struct device *dev = mp->pci.dev;
	struct meson_pcie_clk_res *res = &mp->clk_res;
	int ret;

	res->port_clk = devm_clk_get(dev, "port");
	if (IS_ERR(res->port_clk)) {
		if (PTR_ERR(res->port_clk) != -EPROBE_DEFER)
			dev_err(dev, "Failed to get pcie port clock\n");

		return PTR_ERR(res->port_clk);
	}

	res->mipi_gate = devm_clk_get(dev, "pcie_mipi_en");
	if (IS_ERR(res->mipi_gate)) {
		if (PTR_ERR(res->mipi_gate) != -EPROBE_DEFER)
			dev_err(dev, "Failed to get pcie mipi clock\n");

		return PTR_ERR(res->mipi_gate);
	}

	res->general_clk = devm_clk_get(dev, "pcie_general");
	if (IS_ERR(res->general_clk)) {
		if (PTR_ERR(res->general_clk) != -EPROBE_DEFER)
			dev_err(dev, "Failed to get pcie general clock\n");

		return PTR_ERR(res->general_clk);
	}

	res->clk = devm_clk_get(dev, "pcie");
	if (IS_ERR(res->clk)) {
		if (PTR_ERR(res->clk) != -EPROBE_DEFER)
			dev_err(dev, "Failed to get pcie rc clock\n");

		return PTR_ERR(res->clk);
	}

	phy_init(mp->phy);
	meson_pcie_rc_reset(mp);

	ret = clk_set_rate(res->port_clk, PORT_CLK_RATE);
	if (ret) {
		dev_err(dev, "set bus clk rate failed, ret = %d", ret);
		goto err_clk;
	}

	ret = clk_prepare_enable(res->port_clk);
	if (ret) {
		dev_err(dev, "cannot enable pcie port clock");
		goto err_clk;
	}

	ret = clk_prepare_enable(res->mipi_gate);
	if (ret) {
		dev_err(dev, "cannot enable pcie mipi gate clock");
		goto err_port_clk;
	}

	ret = clk_prepare_enable(res->general_clk);
	if (ret) {
		dev_err(dev, "cannot enable pcie general clock");
		goto err_mipi_gate;
	}

	ret = clk_prepare_enable(res->clk);
	if (ret) {
		dev_err(dev, "cannot enable pcie rc clock");
		goto err_general_clk;
	}

	return 0;

err_general_clk:
	clk_disable_unprepare(res->general_clk);
err_mipi_gate:
	clk_disable_unprepare(res->mipi_gate);
err_port_clk:
	clk_disable_unprepare(res->port_clk);
err_clk:
	return ret;
}

static void meson_pcie_deinit_clk(struct meson_pcie *mp)
{
	clk_disable_unprepare(mp->clk_res.clk);
	clk_disable_unprepare(mp->clk_res.general_clk);
	clk_disable_unprepare(mp->clk_res.mipi_gate);
	clk_disable_unprepare(mp->clk_res.port_clk);
}

static inline void meson_elb_writel(struct meson_pcie *mp, u32 val, u32 reg)
{
	writel(val, mp->mem_res.elbi_base + reg);
}

static inline u32 meson_elb_readl(struct meson_pcie *mp, u32 reg)
{
	return readl(mp->mem_res.elbi_base + reg);
}

static inline u32 meson_cfg_readl(struct meson_pcie *mp, u32 reg)
{
	return readl(mp->mem_res.cfg_base + reg);
}

static inline void meson_cfg_writel(struct meson_pcie *mp, u32 val, u32 reg)
{
	writel(val, mp->mem_res.cfg_base + reg);
}

static void meson_pcie_assert_reset(struct meson_pcie *mp)
{
	gpiod_set_value_cansleep(mp->reset_gpio, 0);
	udelay(500);
	gpiod_set_value_cansleep(mp->reset_gpio, 1);
}

static void meson_pcie_init_dw(struct meson_pcie *mp)
{
	u32 val = 0;

	val = meson_cfg_readl(mp, PCIE_CFG0);
	val |= APP_LTSSM_ENABLE;
	meson_cfg_writel(mp, val, PCIE_CFG0);

	val = meson_elb_readl(mp, PCIE_PORT_LINK_CTRL_OFF);
	val &= ~LINK_CAPABLE_MASK;
	meson_elb_writel(mp, val, PCIE_PORT_LINK_CTRL_OFF);

	val = meson_elb_readl(mp, PCIE_PORT_LINK_CTRL_OFF);
	val |= LINK_CAPABLE_X1 | FAST_LINK_MODE;
	meson_elb_writel(mp, val, PCIE_PORT_LINK_CTRL_OFF);

	val = meson_elb_readl(mp, PCIE_GEN2_CTRL_OFF);
	val &= ~NUM_OF_LANES_MASK;
	meson_elb_writel(mp, val, PCIE_GEN2_CTRL_OFF);

	val = meson_elb_readl(mp, PCIE_GEN2_CTRL_OFF);
	val |= NUM_OF_LANES_X1 | DIRECT_SPEED_CHANGE;
	meson_elb_writel(mp, val, PCIE_GEN2_CTRL_OFF);

	meson_elb_writel(mp, 0x0, PCIE_BASE_ADDR0);
	meson_elb_writel(mp, 0x0, PCIE_BASE_ADDR1);
}

static int meson_size_to_payload(int size)
{
	if (size & (size - 1) || size < 128 || size > 4096)
		return 1;

	return fls(size) - 8;
}

static void meson_set_max_payload(struct meson_pcie *mp, int size)
{
	u32 val = 0;
	int max_payload_size = meson_size_to_payload(size);

	val = meson_elb_readl(mp, PCIE_DEV_CTRL_DEV_STUS);
	val &= ~PCIE_CAP_MAX_PAYLOAD_MASK;
	meson_elb_writel(mp, val, PCIE_DEV_CTRL_DEV_STUS);

	val = meson_elb_readl(mp, PCIE_DEV_CTRL_DEV_STUS);
	val |= PCIE_CAP_MAX_PAYLOAD_SIZE(max_payload_size);
	meson_elb_writel(mp, val, PCIE_DEV_CTRL_DEV_STUS);
}

static void meson_set_max_rd_req_size(struct meson_pcie *mp, int size)
{
	u32 val = 0;
	int max_rd_req_size = meson_size_to_payload(size);

	val = meson_elb_readl(mp, PCIE_DEV_CTRL_DEV_STUS);
	val &= ~PCIE_CAP_MAX_READ_REQ_MASK;
	meson_elb_writel(mp, val, PCIE_DEV_CTRL_DEV_STUS);

	val = meson_elb_readl(mp, PCIE_DEV_CTRL_DEV_STUS);
	val |= PCIE_CAP_MAX_READ_REQ_SIZE(max_rd_req_size);
	meson_elb_writel(mp, val, PCIE_DEV_CTRL_DEV_STUS);
}

static inline void meson_enable_memory_space(struct meson_pcie *mp)
{
	/* Set the RC Bus Master, Memory Space and I/O Space enables */
	meson_elb_writel(mp, PCI_IO_EN | PCI_MEM_SPACE_EN | PCI_BUS_MASTER_EN,
			 PCIE_STATUS_COMMAND);
}

static int meson_pcie_establish_link(struct meson_pcie *mp)
{
	struct dw_pcie *pci = &mp->pci;
	struct pcie_port *pp = &pci->pp;

	meson_pcie_init_dw(mp);
	meson_set_max_payload(mp, MAX_PAYLOAD_SIZE);
	meson_set_max_rd_req_size(mp, MAX_READ_REQ_SIZE);

	dw_pcie_setup_rc(pp);
	meson_enable_memory_space(mp);

	meson_pcie_assert_reset(mp);

	/* check if the link is up or not */
	if (!dw_pcie_wait_for_link(pci))
		return 0;

	return -ETIMEDOUT;
}

static void meson_pcie_msi_init(struct meson_pcie *mp)
{
	struct pcie_port *pp = &mp->pci.pp;

	dw_pcie_msi_init(pp);
}

static void meson_pcie_enable_interrupts(struct meson_pcie *mp)
{
	if (IS_ENABLED(CONFIG_PCI_MSI))
		meson_pcie_msi_init(mp);
}

static u32 meson_pcie_read_dbi(struct dw_pcie *pci, void __iomem *base,
			       u32 reg, size_t size)
{
	u32 val;

	dw_pcie_read(base + reg, size, &val);

	return val;
}

static void meson_pcie_write_dbi(struct dw_pcie *pci, void __iomem *base,
				 u32 reg, size_t size, u32 val)
{
	dw_pcie_write(base + reg, size, val);
}

static int meson_pcie_rd_own_conf(struct pcie_port *pp, int where, int size,
				  u32 *val)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct meson_pcie *mp = to_meson_pcie(pci);

	if (!mp->device_attch)
		return 0;

	/* the device class is not reported correctly from the register */
	if (where == PCI_CLASS_REVISION) {
		*val = readl(pci->dbi_base + PCI_CLASS_REVISION);
		/* keep revision id */
		*val &= PCI_CLASS_REVISION_MASK;
		*val |= PCI_CLASS_BRIDGE_PCI << 16;
		return PCIBIOS_SUCCESSFUL;
	}

	return dw_pcie_read(pci->dbi_base + where, size, val);
}

static int meson_pcie_wr_own_conf(struct pcie_port *pp, int where,
				  int size, u32 val)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct meson_pcie *mp = to_meson_pcie(pci);

	if (!mp->device_attch)
		return 0;

	return dw_pcie_write(pci->dbi_base + where, size, val);
}

static int meson_pcie_link_up(struct dw_pcie *pci)
{
	struct meson_pcie *mp = to_meson_pcie(pci);
	struct device *dev = pci->dev;
	u32 smlh_up = 0;
	u32 ltssm_up = 0;
	u32 rdlh_up = 0;
	u32 speed_okay = 0;
	u32 cnt = 0;
	u32 state12, state17;

	while (smlh_up == 0 || rdlh_up == 0 || ltssm_up == 0 ||
	       speed_okay == 0) {
		udelay(20);

		state12 = meson_cfg_readl(mp, PCIE_CFG_STATUS12);
		state17 = meson_cfg_readl(mp, PCIE_CFG_STATUS17);
		smlh_up = IS_SMLH_LINK_UP(state12);
		rdlh_up = IS_RDLH_LINK_UP(state12);
		ltssm_up = IS_LTSSM_UP(state12);

		if (PM_CURRENT_STATE(state17) < PCIE_GEN3)
			speed_okay = 1;

		if (smlh_up)
			dev_dbg(dev, "smlh_link_up is on\n");
		if (rdlh_up)
			dev_dbg(dev, "rdlh_link_up is on\n");
		if (ltssm_up)
			dev_dbg(dev, "ltssm_up is on\n");
		if (speed_okay)
			dev_dbg(dev, "speed_okay\n");

		cnt++;

		if (cnt >= WAIT_LINKUP_TIMEOUT) {
			dev_err(dev, "Error: Wait linkup timeout.\n");
			return 0;
		}
	}

	return 1;
}

static int meson_pcie_host_init(struct pcie_port *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct meson_pcie *mp = to_meson_pcie(pci);
	int ret;

	ret = meson_pcie_establish_link(mp);

	if (ret)
		return ret;

	mp->device_attch = 1;
	meson_pcie_enable_interrupts(mp);

	return 0;
}

static const struct dw_pcie_host_ops meson_pcie_host_ops = {
	.rd_own_conf = meson_pcie_rd_own_conf,
	.wr_own_conf = meson_pcie_wr_own_conf,
	.host_init = meson_pcie_host_init,
};

static int meson_add_pcie_port(struct meson_pcie *mp,
			       struct platform_device *pdev)
{
	struct dw_pcie *pci = &mp->pci;
	struct pcie_port *pp = &pci->pp;
	struct device *dev = &pdev->dev;
	int ret;

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		pp->msi_irq = platform_get_irq(pdev, 0);
		if (pp->msi_irq < 0) {
			dev_err(dev, "failed to get msi irq\n");
			return pp->msi_irq;
		}
	}

	pp->root_bus_nr = -1;
	pp->ops = &meson_pcie_host_ops;
	pci->dbi_base = mp->mem_res.elbi_base;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "failed to initialize host\n");
		return ret;
	}

	return 0;
}

static const struct dw_pcie_ops dw_pcie_ops = {
	.read_dbi = meson_pcie_read_dbi,
	.write_dbi = meson_pcie_write_dbi,
	.link_up = meson_pcie_link_up,
};

static int meson_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct dw_pcie *pci;
	struct meson_pcie *mp;
	struct meson_pcie_rc_reset *mrst;
	int ret;

	mp = devm_kzalloc(dev, sizeof(*mp), GFP_KERNEL);
	if (!mp)
		return -ENOMEM;

	pci = &mp->pci;
	pci->dev = dev;
	pci->ops = &dw_pcie_ops;

	mp->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(mp->reset_gpio)) {
		dev_err(dev, "Get reset gpio failed\n");
		return PTR_ERR(mp->reset_gpio);
	}

	mp->phy = devm_of_phy_get(dev, np, NULL);
	if (IS_ERR(mp->phy)) {
		if (PTR_ERR(mp->phy) != -EPROBE_DEFER)
			dev_err(dev, "Get phy failed, set default %ld\n",
				PTR_ERR(mp->phy));
		return PTR_ERR(mp->phy);
	}

	mrst = &mp->mrst;

	mrst->port = devm_reset_control_get_shared(dev, "port");
	if (IS_ERR(mrst->port)) {
		if (PTR_ERR(mrst->port) != -EPROBE_DEFER)
			dev_err(dev, "couldn't get port reset %ld\n",
				PTR_ERR(mrst->port));

		return PTR_ERR(mrst->port);
	}

	mrst->apb = devm_reset_control_get_shared(dev, "apb");
	if (IS_ERR(mrst->apb)) {
		if (PTR_ERR(mrst->apb) != -EPROBE_DEFER)
			dev_err(dev, "couldn't get apb reset\n");

		return PTR_ERR(mrst->apb);
	}

	reset_control_deassert(mrst->port);
	reset_control_deassert(mrst->apb);

	phy_power_on(mp->phy);

	ret = meson_pcie_init_clk(mp);
	if (ret) {
		dev_err(dev, "Init clock resources failed, %d\n", ret);
		return ret;
	}

	ret = meson_pcie_get_mem(pdev, mp);
	if (ret) {
		dev_err(dev, "Get memory resource failed, %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, mp);

	ret = meson_add_pcie_port(mp, pdev);
	if (ret < 0) {
		dev_err(dev, "Add PCIE port failed, %d\n", ret);
		meson_pcie_deinit_clk(mp);
	}

	return ret;
}

static const struct of_device_id meson_pcie_of_match[] = {
	{
		.compatible = "amlogic,axg-pcie",
	},
	{},
};

static struct platform_driver meson_pcie_driver = {
	.probe = meson_pcie_probe,
	.driver = {
		.name = "meson-pcie",
		.of_match_table = meson_pcie_of_match,
	},
};

builtin_platform_driver(meson_pcie_driver);
