// SPDX-License-Identifier: GPL-2.0
/*
 * Tegra20 External Memory Controller driver
 *
 * Author: Dmitry Osipenko <digetx@gmail.com>
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sort.h>
#include <linux/types.h>

#include <soc/tegra/fuse.h>

#define EMC_INTSTATUS				0x000
#define EMC_INTMASK				0x004
#define EMC_TIMING_CONTROL			0x028
#define EMC_RC					0x02c
#define EMC_RFC					0x030
#define EMC_RAS					0x034
#define EMC_RP					0x038
#define EMC_R2W					0x03c
#define EMC_W2R					0x040
#define EMC_R2P					0x044
#define EMC_W2P					0x048
#define EMC_RD_RCD				0x04c
#define EMC_WR_RCD				0x050
#define EMC_RRD					0x054
#define EMC_REXT				0x058
#define EMC_WDV					0x05c
#define EMC_QUSE				0x060
#define EMC_QRST				0x064
#define EMC_QSAFE				0x068
#define EMC_RDV					0x06c
#define EMC_REFRESH				0x070
#define EMC_BURST_REFRESH_NUM			0x074
#define EMC_PDEX2WR				0x078
#define EMC_PDEX2RD				0x07c
#define EMC_PCHG2PDEN				0x080
#define EMC_ACT2PDEN				0x084
#define EMC_AR2PDEN				0x088
#define EMC_RW2PDEN				0x08c
#define EMC_TXSR				0x090
#define EMC_TCKE				0x094
#define EMC_TFAW				0x098
#define EMC_TRPAB				0x09c
#define EMC_TCLKSTABLE				0x0a0
#define EMC_TCLKSTOP				0x0a4
#define EMC_TREFBW				0x0a8
#define EMC_QUSE_EXTRA				0x0ac
#define EMC_ODT_WRITE				0x0b0
#define EMC_ODT_READ				0x0b4
#define EMC_FBIO_CFG5				0x104
#define EMC_FBIO_CFG6				0x114
#define EMC_AUTO_CAL_INTERVAL			0x2a8
#define EMC_CFG_2				0x2b8
#define EMC_CFG_DIG_DLL				0x2bc
#define EMC_DLL_XFORM_DQS			0x2c0
#define EMC_DLL_XFORM_QUSE			0x2c4
#define EMC_ZCAL_REF_CNT			0x2e0
#define EMC_ZCAL_WAIT_CNT			0x2e4
#define EMC_CFG_CLKTRIM_0			0x2d0
#define EMC_CFG_CLKTRIM_1			0x2d4
#define EMC_CFG_CLKTRIM_2			0x2d8

#define EMC_CLKCHANGE_REQ_ENABLE		BIT(0)

#define EMC_TIMING_UPDATE			BIT(0)

#define EMC_CLKCHANGE_COMPLETE_INT		BIT(4)

static const unsigned long emc_timing_registers[] = {
	EMC_RC,
	EMC_RFC,
	EMC_RAS,
	EMC_RP,
	EMC_R2W,
	EMC_W2R,
	EMC_R2P,
	EMC_W2P,
	EMC_RD_RCD,
	EMC_WR_RCD,
	EMC_RRD,
	EMC_REXT,
	EMC_WDV,
	EMC_QUSE,
	EMC_QRST,
	EMC_QSAFE,
	EMC_RDV,
	EMC_REFRESH,
	EMC_BURST_REFRESH_NUM,
	EMC_PDEX2WR,
	EMC_PDEX2RD,
	EMC_PCHG2PDEN,
	EMC_ACT2PDEN,
	EMC_AR2PDEN,
	EMC_RW2PDEN,
	EMC_TXSR,
	EMC_TCKE,
	EMC_TFAW,
	EMC_TRPAB,
	EMC_TCLKSTABLE,
	EMC_TCLKSTOP,
	EMC_TREFBW,
	EMC_QUSE_EXTRA,
	EMC_FBIO_CFG6,
	EMC_ODT_WRITE,
	EMC_ODT_READ,
	EMC_FBIO_CFG5,
	EMC_CFG_DIG_DLL,
	EMC_DLL_XFORM_DQS,
	EMC_DLL_XFORM_QUSE,
	EMC_ZCAL_REF_CNT,
	EMC_ZCAL_WAIT_CNT,
	EMC_AUTO_CAL_INTERVAL,
	EMC_CFG_CLKTRIM_0,
	EMC_CFG_CLKTRIM_1,
	EMC_CFG_CLKTRIM_2,
};

struct emc_timing {
	unsigned long rate;
	u32 emc_registers_data[ARRAY_SIZE(emc_timing_registers)];
};

struct tegra_emc {
	struct device *dev;
	struct notifier_block clk_nb;
	struct clk *backup_clk;
	struct clk *emc_mux;
	struct clk *pll_m;
	struct clk *clk;
	void __iomem *regs;

	struct completion clk_handshake_complete;
	int irq;

	struct emc_timing *timings;
	unsigned int num_timings;
};

static irqreturn_t tegra_emc_isr(int irq, void *data)
{
	struct tegra_emc *emc = data;
	u32 intmask, status;

	if (completion_done(&emc->clk_handshake_complete))
		return IRQ_NONE;

	intmask = EMC_CLKCHANGE_COMPLETE_INT;

	status = readl_relaxed(emc->regs + EMC_INTSTATUS) & intmask;
	if (!status)
		return IRQ_NONE;

	writel_relaxed(status, emc->regs + EMC_INTSTATUS);

	complete(&emc->clk_handshake_complete);

	return IRQ_HANDLED;
}

static struct emc_timing *tegra_emc_find_timing(struct tegra_emc *emc,
						unsigned long rate)
{
	struct emc_timing *timing = NULL;
	unsigned int i;

	for (i = 0; i < emc->num_timings; i++) {
		if (emc->timings[i].rate >= rate) {
			timing = &emc->timings[i];
			break;
		}
	}

	if (!timing) {
		dev_err(emc->dev, "no timing for rate %lu\n", rate);
		return NULL;
	}

	return timing;
}

static int emc_prepare_timing_change(struct tegra_emc *emc, unsigned long rate)
{
	struct emc_timing *timing = tegra_emc_find_timing(emc, rate);
	unsigned int i;

	if (!timing)
		return -ENOENT;

	dev_dbg(emc->dev, "%s: timing rate %lu emc rate %lu\n",
		__func__, timing->rate, rate);

	/* program shadow registers */
	for (i = 0; i < ARRAY_SIZE(timing->emc_registers_data); i++)
		writel(timing->emc_registers_data[i],
		       emc->regs + emc_timing_registers[i]);

	/* read last-written register to wait until programming has settled */
	readl(emc->regs + emc_timing_registers[i - 1]);

	if (emc->irq < 0)
		writel(EMC_CLKCHANGE_COMPLETE_INT, emc->regs + EMC_INTMASK);
	else
		reinit_completion(&emc->clk_handshake_complete);

	return 0;
}

static int emc_complete_timing_change(struct tegra_emc *emc, bool flush)
{
	long timeout;
	u32 value;
	int err;

	dev_dbg(emc->dev, "%s: flush %d\n", __func__, flush);

	if (flush) {
		/* manually initiate memory timings update */
		writel(EMC_TIMING_UPDATE, emc->regs + EMC_TIMING_CONTROL);
		return 0;
	}

	if (emc->irq < 0) {
		/* poll interrupt status if IRQ isn't available */
		err = readl_relaxed_poll_timeout(emc->regs + EMC_INTSTATUS,
				value, value & EMC_CLKCHANGE_COMPLETE_INT,
				1, 100);
		if (err) {
			dev_err(emc->dev, "EMC handshake failed\n");
			return -EIO;
		}

		return 0;
	}

	timeout = wait_for_completion_timeout(&emc->clk_handshake_complete,
					      usecs_to_jiffies(100));
	if (timeout == 0) {
		dev_err(emc->dev, "EMC handshake failed\n");
		return -EIO;
	} else if (timeout < 0) {
		dev_err(emc->dev, "failed to wait for EMC handshake: %ld\n",
			timeout);
		return timeout;
	}

	return 0;
}

static int load_one_timing_from_dt(struct tegra_emc *emc,
				   struct emc_timing *timing,
				   struct device_node *node)
{
	u32 rate;
	int err;

	if (!of_device_is_compatible(node, "nvidia,tegra20-emc-table")) {
		dev_err(emc->dev, "incompatible DT node \"%s\"\n",
			node->name);
		return -EINVAL;
	}

	err = of_property_read_u32(node, "clock-frequency", &rate);
	if (err) {
		dev_err(emc->dev, "timing %s: failed to read rate: %d\n",
			node->name, err);
		return err;
	}

	err = of_property_read_u32_array(node, "nvidia,emc-registers",
					 timing->emc_registers_data,
					 ARRAY_SIZE(emc_timing_registers));
	if (err) {
		dev_err(emc->dev,
			"timing %s: failed to read emc timing data: %d\n",
			node->name, err);
		return err;
	}

	/*
	 * The EMC clock rate is twice the bus rate, and the bus rate is
	 * measured in kHz.
	 */
	timing->rate = rate * 2 * 1000;

	dev_dbg(emc->dev, "%s: emc rate %ld\n", __func__, timing->rate);

	return 0;
}

static int cmp_timings(const void *_a, const void *_b)
{
	const struct emc_timing *a = _a;
	const struct emc_timing *b = _b;

	if (a->rate < b->rate)
		return -1;
	else if (a->rate == b->rate)
		return 0;
	else
		return 1;
}

static int tegra_emc_load_timings_from_dt(struct tegra_emc *emc,
					  struct device_node *node)
{
	struct device_node *child;
	struct emc_timing *timing;
	int child_count;
	int err;

	child_count = of_get_child_count(node);
	if (!child_count)
		return -ENOENT;

	emc->timings = devm_kcalloc(emc->dev, child_count, sizeof(*timing),
				    GFP_KERNEL);
	if (!emc->timings)
		return -ENOMEM;

	emc->num_timings = child_count;
	timing = emc->timings;

	for_each_child_of_node(node, child) {
		err = load_one_timing_from_dt(emc, timing++, child);
		if (err) {
			of_node_put(child);
			return err;
		}
	}

	sort(emc->timings, emc->num_timings, sizeof(*timing), cmp_timings,
	     NULL);

	return 0;
}

static struct device_node *
tegra_emc_find_node_by_ram_code(struct device_node *node, u32 ram_code)
{
	struct device_node *np;
	int err;

	for_each_child_of_node(node, np) {
		u32 value;

		err = of_property_read_u32(np, "nvidia,ram-code", &value);
		if (err || value != ram_code)
			continue;

		return np;
	}

	return NULL;
}

static int tegra_emc_clk_change_notify(struct notifier_block *nb,
				       unsigned long msg, void *data)
{
	struct tegra_emc *emc = container_of(nb, struct tegra_emc, clk_nb);
	struct clk_notifier_data *cnd = data;
	int err;

	switch (msg) {
	case PRE_RATE_CHANGE:
		err = emc_prepare_timing_change(emc, cnd->new_rate);
		break;

	case ABORT_RATE_CHANGE:
		err = emc_prepare_timing_change(emc, cnd->old_rate);
		if (err)
			break;

		err = emc_complete_timing_change(emc, true);
		break;

	case POST_RATE_CHANGE:
		err = emc_complete_timing_change(emc, false);
		break;

	default:
		return NOTIFY_DONE;
	}

	return notifier_from_errno(err);
}

static void emc_setup_hw(struct tegra_emc *emc)
{
	u32 value;

	/* allow EMC and CAR to handshake on PLL divider/source changes */
	value = readl_relaxed(emc->regs + EMC_CFG_2);
	value |= EMC_CLKCHANGE_REQ_ENABLE;
	writel(value, emc->regs + EMC_CFG_2);

	/* initialize interrupt */
	writel(EMC_CLKCHANGE_COMPLETE_INT, emc->regs + EMC_INTMASK);
	writel(EMC_CLKCHANGE_COMPLETE_INT, emc->regs + EMC_INTSTATUS);
}

static int emc_init(struct tegra_emc *emc, unsigned long rate)
{
	int err;

	err = clk_set_parent(emc->emc_mux, emc->backup_clk);
	if (err) {
		dev_err(emc->dev,
			"failed to reparent to backup source: %d\n", err);
		return err;
	}

	err = clk_set_rate(emc->pll_m, rate);
	if (err)
		dev_err(emc->dev,
			"failed to change pll_m rate: %d\n", err);

	err = clk_set_parent(emc->emc_mux, emc->pll_m);
	if (err) {
		dev_err(emc->dev,
			"failed to reparent to pll_m: %d\n", err);
		return err;
	}

	return 0;
}

static int tegra_emc_probe(struct platform_device *pdev)
{
	struct device_node *np;
	struct tegra_emc *emc;
	struct resource *res;
	u32 ram_code;
	int err;

	emc = devm_kzalloc(&pdev->dev, sizeof(*emc), GFP_KERNEL);
	if (!emc)
		return -ENOMEM;

	emc->dev = &pdev->dev;

	ram_code = tegra_read_ram_code();

	np = tegra_emc_find_node_by_ram_code(pdev->dev.of_node, ram_code);
	if (!np) {
		dev_info(&pdev->dev,
			 "no memory timings for RAM code %u found in DT\n",
			 ram_code);
		return -ENOENT;
	}

	err = tegra_emc_load_timings_from_dt(emc, np);
	of_node_put(np);
	if (err)
		return err;

	if (emc->num_timings == 0) {
		dev_err(&pdev->dev,
			"no memory timings for RAM code %u registered\n",
			ram_code);
		return -ENOENT;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	emc->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(emc->regs))
		return PTR_ERR(emc->regs);

	emc_setup_hw(emc);

	emc->irq = platform_get_irq(pdev, 0);
	if (emc->irq < 0) {
		dev_warn(&pdev->dev, "interrupt not specified\n");
		dev_warn(&pdev->dev, "continuing, but please update your DT\n");
	} else {
		init_completion(&emc->clk_handshake_complete);

		err = devm_request_irq(&pdev->dev, emc->irq, tegra_emc_isr, 0,
				       dev_name(&pdev->dev), emc);
		if (err < 0) {
			dev_err(&pdev->dev, "failed to request IRQ#%u: %d\n",
				emc->irq, err);
			return err;
		}
	}

	emc->pll_m = clk_get_sys(NULL, "pll_m");
	if (IS_ERR(emc->pll_m)) {
		err = PTR_ERR(emc->pll_m);
		dev_err(&pdev->dev, "failed to get pll_m: %d\n", err);
		return -EPROBE_DEFER;
	}

	emc->backup_clk = clk_get_sys(NULL, "pll_p");
	if (IS_ERR(emc->backup_clk)) {
		err = PTR_ERR(emc->backup_clk);
		dev_err(&pdev->dev, "failed to get pll_p: %d\n", err);
		goto put_pll_m;
	}

	emc->clk = clk_get_sys(NULL, "emc");
	if (IS_ERR(emc->clk)) {
		err = PTR_ERR(emc->clk);
		dev_err(&pdev->dev, "failed to get emc: %d\n", err);
		goto put_backup;
	}

	emc->emc_mux = clk_get_parent(emc->clk);
	if (IS_ERR(emc->emc_mux)) {
		err = PTR_ERR(emc->emc_mux);
		dev_err(&pdev->dev, "failed to get emc_mux: %d\n", err);
		goto put_emc;
	}

	emc->clk_nb.notifier_call = tegra_emc_clk_change_notify;

	err = clk_notifier_register(emc->clk, &emc->clk_nb);
	if (err) {
		dev_err(&pdev->dev, "failed to register clk notifier: %d\n",
			err);
		goto put_emc;
	}

	/* set DRAM clock rate to maximum */
	err = emc_init(emc, emc->timings[emc->num_timings - 1].rate);
	if (err) {
		dev_err(&pdev->dev, "failed to initialize clk rate: %d\n",
			err);
		goto unreg_notifier;
	}

	return 0;

unreg_notifier:
	clk_notifier_unregister(emc->emc_mux, &emc->clk_nb);
put_emc:
	clk_put(emc->clk);
put_backup:
	clk_put(emc->backup_clk);
put_pll_m:
	clk_put(emc->pll_m);

	return err;
}

static const struct of_device_id tegra_emc_of_match[] = {
	{ .compatible = "nvidia,tegra20-emc", },
	{},
};

static struct platform_driver tegra_emc_driver = {
	.probe = tegra_emc_probe,
	.driver = {
		.name = "tegra20-emc",
		.of_match_table = tegra_emc_of_match,
		.suppress_bind_attrs = true,
	},
};

static int __init tegra_emc_init(void)
{
	return platform_driver_register(&tegra_emc_driver);
}
subsys_initcall(tegra_emc_init);
