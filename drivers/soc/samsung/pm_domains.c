/*
 * Exynos Generic power domain support.
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Implementation of Exynos specific power domain control which is used in
 * conjunction with runtime-pm. Support for both device-tree and non-device-tree
 * based power domain support is included.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/io.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/pm_domain.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/sched.h>

#define MAX_CLK_PER_DOMAIN	4

struct exynos_pm_domain_config {
	/* Value for LOCAL_PWR_CFG and STATUS fields for each domain */
	u32 local_pwr_cfg;
};

struct exynos_pm_domain_data {
	const char *name;
	u32 base;
};

struct exynos_pm_domain_soc_data {
	const char *compatible;
	unsigned int nr_domains;
	const struct exynos_pm_domain_data *domains;
};

/*
 * Exynos specific wrapper around the generic power domain
 */
struct exynos_pm_domain {
	void __iomem *base;
	bool is_off;
	struct generic_pm_domain pd;
	struct clk *oscclk;
	struct clk *clk[MAX_CLK_PER_DOMAIN];
	struct clk *pclk[MAX_CLK_PER_DOMAIN];
	struct clk *asb_clk[MAX_CLK_PER_DOMAIN];
	u32 local_pwr_cfg;
};

static int exynos_pd_power(struct generic_pm_domain *domain, bool power_on)
{
	struct exynos_pm_domain *pd;
	void __iomem *base;
	u32 timeout, pwr;
	char *op;
	int i;

	pd = container_of(domain, struct exynos_pm_domain, pd);
	base = pd->base;

	for (i = 0; i < MAX_CLK_PER_DOMAIN; i++) {
		if (IS_ERR(pd->asb_clk[i]))
			break;
		clk_prepare_enable(pd->asb_clk[i]);
	}

	/* Set oscclk before powering off a domain*/
	if (!power_on) {
		for (i = 0; i < MAX_CLK_PER_DOMAIN; i++) {
			if (IS_ERR(pd->clk[i]))
				break;
			pd->pclk[i] = clk_get_parent(pd->clk[i]);
			if (clk_set_parent(pd->clk[i], pd->oscclk))
				pr_err("%s: error setting oscclk as parent to clock %d\n",
						domain->name, i);
		}
	}

	pwr = power_on ? pd->local_pwr_cfg : 0;
	writel_relaxed(pwr, base);

	/* Wait max 1ms */
	timeout = 10;

	while ((readl_relaxed(base + 0x4) & pd->local_pwr_cfg) != pwr) {
		if (!timeout) {
			op = (power_on) ? "enable" : "disable";
			pr_err("Power domain %s %s failed\n", domain->name, op);
			return -ETIMEDOUT;
		}
		timeout--;
		cpu_relax();
		usleep_range(80, 100);
	}

	/* Restore clocks after powering on a domain*/
	if (power_on) {
		for (i = 0; i < MAX_CLK_PER_DOMAIN; i++) {
			if (IS_ERR(pd->clk[i]))
				break;

			if (IS_ERR(pd->pclk[i]))
				continue; /* Skip on first power up */
			if (clk_set_parent(pd->clk[i], pd->pclk[i]))
				pr_err("%s: error setting parent to clock%d\n",
						domain->name, i);
		}
	}

	for (i = 0; i < MAX_CLK_PER_DOMAIN; i++) {
		if (IS_ERR(pd->asb_clk[i]))
			break;
		clk_disable_unprepare(pd->asb_clk[i]);
	}

	return 0;
}

static int exynos_pd_power_on(struct generic_pm_domain *domain)
{
	return exynos_pd_power(domain, true);
}

static int exynos_pd_power_off(struct generic_pm_domain *domain)
{
	return exynos_pd_power(domain, false);
}

static const struct exynos_pm_domain_data exynos4210_domains[] __initconst = {
	{ "LCD1",	0x10023CA0 },
};

static const struct exynos_pm_domain_data exynos4412_domains[] __initconst = {
	{ "CAM",	0x10023C00 },
	{ "TV",		0x10023C20 },
	{ "MFC",	0x10023C40 },
	{ "G3D",	0x10023C60 },
	{ "LCD0",	0x10023C80 },
	{ "ISP",	0x10023CA0 },
	{ "GPS",	0x10023CE0 },
	{ "GPS alive",	0x10023D00 },
};

static const struct exynos_pm_domain_data exynos5250_domains[] __initconst = {
	{ "GSCL",	0x10044000 },
	{ "ISP",	0x10044020 },
	{ "MFC",	0x10044040 },
	{ "G3D",	0x10044060 },
	{ "DISP1",	0x100440A0 },
	{ "MAU",	0x100440C0 },
};

static const struct exynos_pm_domain_data exynos542x_domains[] __initconst = {
	{ "SCALER",	0x10044000 },
	{ "ISP",	0x10044020 },
	{ "MFC",	0x10044060 },
	{ "G3D",	0x10044080 },
	{ "DISP1",	0x100440C0 },
	{ "MAU",	0x100440E0 },
	{ "G2D",	0x10044100 },
	{ "MSCL",	0x10044120 },
	{ "FSYS",	0x10044140 },
	{ "PERIC",	0x100441A0 },
	{ "CAM",	0x10045100 },
};

static const struct exynos_pm_domain_data exynos5433_domains[] __initconst = {
	{ "GSCL",	0x105c4000 },
	{ "MSCL",	0x105c4040 },
	{ "DISP",	0x105c4080 },
	{ "MFC",	0x105c4180 },
	{ "CAM0",	0x105c4020 },
	{ "CAM1",	0x105c40a0 },
	{ "ISP",	0x105c4140 },
	{ "G2D",	0x105c4120 },
	{ "G3D",	0x105c4060 },
	{ "AUD",	0x105c40c0 },
	{ "FSYS",	0x105c40e0 },
	{ "HEVC",	0x105c41c0 },
};

static const struct exynos_pm_domain_soc_data soc_domains_data[] __initconst = {
	{ /* Exynos3250 uses a subset of 4412 domains */
		.compatible = "samsung,exynos3250",
		.nr_domains = ARRAY_SIZE(exynos4412_domains),
		.domains = exynos4412_domains,
	}, { /* first check samsung,exynos4210 to detect LCD1 domain */
		.compatible = "samsung,exynos4210",
		.nr_domains = ARRAY_SIZE(exynos4210_domains),
		.domains = exynos4210_domains,
	}, { /* remaining domains for Exynos4210 and 4412 */
		.compatible = "samsung,exynos4",
		.nr_domains = ARRAY_SIZE(exynos4412_domains),
		.domains = exynos4412_domains
	}, {
		.compatible = "samsung,exynos5250",
		.nr_domains = ARRAY_SIZE(exynos5250_domains),
		.domains = exynos5250_domains,
	}, {
		.compatible = "samsung,exynos5420",
		.nr_domains = ARRAY_SIZE(exynos542x_domains),
		.domains = exynos542x_domains,
	}, {
		.compatible = "samsung,exynos5800",
		.nr_domains = ARRAY_SIZE(exynos542x_domains),
		.domains = exynos542x_domains,
	}, {
		.compatible = "samsung,exynos5433",
		.nr_domains = ARRAY_SIZE(exynos5433_domains),
		.domains = exynos5433_domains,
	},
};

static const struct exynos_pm_domain_config exynos4210_cfg __initconst = {
	.local_pwr_cfg		= 0x7,
};

static const struct of_device_id exynos_pm_domain_of_match[] __initconst = {
	{
		.compatible = "samsung,exynos4210-pd",
		.data = &exynos4210_cfg,
	},
	{ },
};

static __init const char *exynos_get_domain_name(struct device_node *np)
{
	const struct exynos_pm_domain_soc_data *soc = soc_domains_data;
	const __be32 *reg;
	u64 addr;
	int i, j;


	reg = of_get_property(np, "reg", NULL);
	if (!reg || (addr = of_translate_address(np, reg)) == OF_BAD_ADDR)
		goto not_found;

	for (i = 0; i < ARRAY_SIZE(soc_domains_data); i++, soc++) {
		if (!of_machine_is_compatible(soc->compatible))
			continue;

		for (j = 0; j < soc->nr_domains; j++) {
			if (soc->domains[j].base == addr)
				return kstrdup_const(soc->domains[j].name,
						     GFP_KERNEL);
		}
	}
not_found:
	return kstrdup_const(strrchr(np->full_name, '/') + 1, GFP_KERNEL);
}

static __init int exynos4_pm_init_power_domain(void)
{
	struct device_node *np;
	const struct of_device_id *match;

	for_each_matching_node_and_match(np, exynos_pm_domain_of_match, &match) {
		const struct exynos_pm_domain_config *pm_domain_cfg;
		struct exynos_pm_domain *pd;
		int on, i;

		pm_domain_cfg = match->data;

		pd = kzalloc(sizeof(*pd), GFP_KERNEL);
		if (!pd) {
			of_node_put(np);
			return -ENOMEM;
		}
		pd->pd.name = exynos_get_domain_name(np);
		if (!pd->pd.name) {
			kfree(pd);
			of_node_put(np);
			return -ENOMEM;
		}

		pd->base = of_iomap(np, 0);
		if (!pd->base) {
			pr_warn("%s: failed to map memory\n", __func__);
			kfree_const(pd->pd.name);
			kfree(pd);
			continue;
		}

		pd->pd.power_off = exynos_pd_power_off;
		pd->pd.power_on = exynos_pd_power_on;
		pd->local_pwr_cfg = pm_domain_cfg->local_pwr_cfg;

		for (i = 0; i < MAX_CLK_PER_DOMAIN; i++) {
			char clk_name[8];

			snprintf(clk_name, sizeof(clk_name), "asb%d", i);
			pd->asb_clk[i] = of_clk_get_by_name(np, clk_name);
			if (IS_ERR(pd->asb_clk[i]))
				break;
		}

		pd->oscclk = of_clk_get_by_name(np, "oscclk");
		if (IS_ERR(pd->oscclk))
			goto no_clk;

		for (i = 0; i < MAX_CLK_PER_DOMAIN; i++) {
			char clk_name[8];

			snprintf(clk_name, sizeof(clk_name), "clk%d", i);
			pd->clk[i] = of_clk_get_by_name(np, clk_name);
			if (IS_ERR(pd->clk[i]))
				break;
			/*
			 * Skip setting parent on first power up.
			 * The parent at this time may not be useful at all.
			 */
			pd->pclk[i] = ERR_PTR(-EINVAL);
		}

		if (IS_ERR(pd->clk[0]))
			clk_put(pd->oscclk);

no_clk:
		on = readl_relaxed(pd->base + 0x4) & pd->local_pwr_cfg;

		pm_genpd_init(&pd->pd, NULL, !on);
		of_genpd_add_provider_simple(np, &pd->pd);
	}

	/* Assign the child power domains to their parents */
	for_each_matching_node(np, exynos_pm_domain_of_match) {
		struct of_phandle_args child, parent;

		child.np = np;
		child.args_count = 0;

		if (of_parse_phandle_with_args(np, "power-domains",
					       "#power-domain-cells", 0,
					       &parent) != 0)
			continue;

		if (of_genpd_add_subdomain(&parent, &child))
			pr_warn("%s failed to add subdomain: %s\n",
				parent.np->full_name, child.np->full_name);
		else
			pr_info("%s has as child subdomain: %s.\n",
				parent.np->full_name, child.np->full_name);
	}

	return 0;
}
core_initcall(exynos4_pm_init_power_domain);
