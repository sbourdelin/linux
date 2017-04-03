/*
 * Copyright (c) 2017, NVIDIA CORPORATION. All rights reserved
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/cpufreq.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <soc/tegra/bpmp.h>
#include <soc/tegra/bpmp-abi.h>

#define EDVD_CORE_VOLT_FREQ(core)		(0x20 + (core) * 0x4)
#define EDVD_CORE_VOLT_FREQ_F_SHIFT		0
#define EDVD_CORE_VOLT_FREQ_F_MASK		0xff
#define EDVD_CORE_VOLT_FREQ_V_SHIFT		16
#define EDVD_CORE_VOLT_FREQ_V_MASK		0xff

#define CLUSTER_DENVER				0
#define CLUSTER_A57				1
#define NUM_CLUSTERS				2

struct tegra186_cpufreq_cluster {
	const char *name;
	unsigned int num_cores;
};

static const struct tegra186_cpufreq_cluster CLUSTERS[] = {
	{
		.name = "denver",
		.num_cores = 2,
	},
	{
		.name = "a57",
		.num_cores = 4,
	}
};

struct tegra186_cpufreq_data {
	void __iomem *regs[NUM_CLUSTERS];
	struct cpufreq_frequency_table *tables[NUM_CLUSTERS];
};

static void get_cluster_core(int cpu, int *cluster, int *core)
{
	switch (cpu) {
	case 0:
		*cluster = CLUSTER_A57; *core = 0; break;
	case 3:
		*cluster = CLUSTER_A57; *core = 1; break;
	case 4:
		*cluster = CLUSTER_A57; *core = 2; break;
	case 5:
		*cluster = CLUSTER_A57; *core = 3; break;
	case 1:
		*cluster = CLUSTER_DENVER; *core = 0; break;
	case 2:
		*cluster = CLUSTER_DENVER; *core = 1; break;
	}
}

static int tegra186_cpufreq_init(struct cpufreq_policy *policy)
{
	struct tegra186_cpufreq_data *data = cpufreq_get_driver_data();
	struct cpufreq_frequency_table *table;
	int cluster, core;

	get_cluster_core(policy->cpu, &cluster, &core);

	table = data->tables[cluster];
	cpufreq_table_validate_and_show(policy, table);

	policy->cpuinfo.transition_latency = 300 * 1000;

	return 0;
}

static void write_volt_freq(uint8_t vidx, uint8_t ndiv, void __iomem *regs,
			unsigned int core)
{
	u32 val = 0;

	val |= ndiv << EDVD_CORE_VOLT_FREQ_F_SHIFT;
	val |= vidx << EDVD_CORE_VOLT_FREQ_V_SHIFT;

	writel(val, regs + EDVD_CORE_VOLT_FREQ(core));
}

static int tegra186_cpufreq_set_target(struct cpufreq_policy *policy,
				       unsigned int index)
{
	struct cpufreq_frequency_table *tbl = policy->freq_table + index;
	struct tegra186_cpufreq_data *data = cpufreq_get_driver_data();
	uint16_t vidx = tbl->driver_data >> 16;
	uint16_t ndiv = tbl->driver_data & 0xffff;
	int cluster, core;

	get_cluster_core(policy->cpu, &cluster, &core);
	write_volt_freq(vidx, ndiv, data->regs[cluster], core);

	return 0;
}

static struct cpufreq_driver tegra186_cpufreq_driver = {
	.name = "tegra186",
	.flags = CPUFREQ_STICKY | CPUFREQ_HAVE_GOVERNOR_PER_POLICY,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = tegra186_cpufreq_set_target,
	.init = tegra186_cpufreq_init,
	.attr = cpufreq_generic_attr,
};

static int init_vhint_table(struct platform_device *pdev,
			    struct tegra_bpmp *bpmp, uint32_t cluster_id,
			    struct cpufreq_frequency_table **table)
{
	struct mrq_cpu_vhint_request req;
	struct tegra_bpmp_message msg;
	struct cpu_vhint_data *data;
	int err, i, j, num_rates;
	dma_addr_t phys;
	void *virt;

	virt = dma_alloc_coherent(bpmp->dev, MSG_DATA_MIN_SZ, &phys,
				  GFP_KERNEL | GFP_DMA32);
	if (!virt)
		return -ENOMEM;

	data = (struct cpu_vhint_data *)virt;

	memset(&req, 0, sizeof(req));
	req.addr = phys;
	req.cluster_id = cluster_id;

	memset(&msg, 0, sizeof(msg));
	msg.mrq = MRQ_CPU_VHINT;
	msg.tx.data = &req;
	msg.tx.size = sizeof(req);

	err = tegra_bpmp_transfer(bpmp, &msg);
	if (err)
		goto end;

	num_rates = 0;

	for (i = data->vfloor; i < data->vceil + 1; ++i) {
		uint16_t ndiv = data->ndiv[i];

		if (ndiv < data->ndiv_min || ndiv > data->ndiv_max)
			continue;

		/* Only store lowest voltage index for each rate */
		if (i > 0 && ndiv == data->ndiv[i-1])
			continue;

		++num_rates;
	}

	*table = devm_kcalloc(&pdev->dev, num_rates + 1, sizeof(**table),
			      GFP_KERNEL);
	if (!*table) {
		err = -ENOMEM;
		goto end;
	}

	for (i = data->vfloor, j = 0; i < data->vceil + 1; ++i) {
		struct cpufreq_frequency_table *point;
		uint16_t ndiv = data->ndiv[i];

		if (ndiv < data->ndiv_min || ndiv > data->ndiv_max)
			continue;

		/* Only store lowest voltage index for each rate */
		if (i > 0 && ndiv == data->ndiv[i-1])
			continue;

		point = &(*table)[j++];
		point->driver_data = (i << 16) | (ndiv);
		point->frequency = data->ref_clk_hz * ndiv / data->pdiv /
			data->mdiv / 1000;
	}

	(*table)[j].frequency = CPUFREQ_TABLE_END;

end:
	dma_free_coherent(bpmp->dev, MSG_DATA_MIN_SZ, virt, phys);

	return err;
}

static int tegra186_cpufreq_probe(struct platform_device *pdev)
{
	struct tegra186_cpufreq_data *data;
	struct tegra_bpmp *bpmp;
	int i, err;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	bpmp = tegra_bpmp_get(&pdev->dev);
	if (IS_ERR(bpmp))
		return PTR_ERR(bpmp);

	for (i = 0; i < NUM_CLUSTERS; ++i) {
		struct resource *res;

		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   CLUSTERS[i].name);
		if (!res) {
			err = -ENXIO;
			goto put_bpmp;
		}

		data->regs[i] = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(data->regs[i])) {
			err = PTR_ERR(data->regs[i]);
			goto put_bpmp;
		}

		err = init_vhint_table(pdev, bpmp, i, &data->tables[i]);
		if (err)
			goto put_bpmp;
	}

	tegra_bpmp_put(bpmp);

	tegra186_cpufreq_driver.driver_data = data;

	err = cpufreq_register_driver(&tegra186_cpufreq_driver);
	if (err)
		return err;

	return 0;

put_bpmp:
	tegra_bpmp_put(bpmp);

	return err;
}

static int tegra186_cpufreq_remove(struct platform_device *pdev)
{
	cpufreq_unregister_driver(&tegra186_cpufreq_driver);

	return 0;
}

static const struct of_device_id tegra186_cpufreq_of_match[] = {
	{ .compatible = "nvidia,tegra186-ccplex-cluster", },
	{ }
};
MODULE_DEVICE_TABLE(of, tegra186_cpufreq_of_match);

static struct platform_driver tegra186_cpufreq_platform_driver = {
	.driver = {
		.name = "tegra186-cpufreq",
		.of_match_table = tegra186_cpufreq_of_match,
	},
	.probe = tegra186_cpufreq_probe,
	.remove = tegra186_cpufreq_remove,
};
module_platform_driver(tegra186_cpufreq_platform_driver);

MODULE_AUTHOR("Mikko Perttunen <mperttunen@nvidia.com>");
MODULE_DESCRIPTION("Tegra186 cpufreq driver");
MODULE_LICENSE("GPL v2");
