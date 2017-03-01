/*
 * Copyright (C) 2017 Linaro Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/interconnect-provider.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <dt-bindings/interconnect/qcom,msm8916.h>

#define to_qcom_icp(_icp) container_of(_icp, struct qcom_interconnect_provider, icp)
#define to_qcom_node(_node) container_of(_node, struct qcom_interconnect_node, node)

#define BW_TO_CLK_FREQ_HZ(width, bw) msm_bus_div64(width, bw)

enum qcom_bus_type {
	QCOM_BUS_TYPE_NOC = 0,
	QCOM_BUS_TYPE_MEM,
	QCOM_BUS_TYPE_MAX,
};

struct qcom_interconnect_provider {
	struct icp		icp;
	void __iomem		*base;
	enum qcom_bus_type	type;
	u32			base_offset;
	u32			qos_offset;
	struct clk		*bus_clk;
	struct clk		*bus_a_clk;
};

struct qcom_interconnect_node {
	unsigned int id;
	struct interconnect_node node;
	unsigned char *name;
	unsigned int links[8];
	int num_links;
	int port;
	int buswidth;
	u64 ib;
	u64 ab;
	u64 rate;
};

struct qcom_interconnect_desc {
	struct qcom_interconnect_node **nodes;
	size_t num_nodes;
};

static struct qcom_interconnect_node snoc_int_0 = {
	.id = 10004,
	.name = "snoc-int-0",
	.links = { 588, 519, 10027 }, /* slv_qdss_stm, slv_imem, snoc_pnoc_mas */
	.num_links = 3,
	.buswidth = 8,
};

static struct qcom_interconnect_node snoc_int_1 = {
	.id = 10005,
	.name = "snoc-int-1",
	.links = { 517, 663, 664 }, /* slv_apss, slv_cats_0, slv_cats_1 */
	.num_links = 3,
	.buswidth = 8,
};

static struct qcom_interconnect_node snoc_int_bimc = {
	.id = 10006,
	.name = "snoc-bimc",
	.links = { 10007 }, /* snoc_bimc_0_mas */
	.num_links = 1,
	.buswidth = 8,
};

static struct qcom_interconnect_node snoc_bimc_0_mas = {
	.id = 10007,
	.name = "snoc-bimc-0-mas",
	.links = { 10025 }, /* snoc_bimc_0_slv */
	.num_links = 1,
	.buswidth = 8,
};

static struct qcom_interconnect_node pnoc_snoc_slv = {
	.id = 10011,
	.name = "snoc-pnoc",
	.links = { 10004, 10006, 10005 }, /* snoc_int_0, snoc_int_bimc, snoc_int_1 */
	.num_links = 3,
	.buswidth = 8,
};

static struct qcom_interconnect_node *msm8916_snoc_nodes[] = {
	[SNOC_INT_0] = &snoc_int_0,
	[SNOC_INT_1] = &snoc_int_1,
	[SNOC_INT_BIMC] = &snoc_int_bimc,
	[SNOC_BIMC_0_MAS] = &snoc_bimc_0_mas,
	[PNOC_SNOC_SLV] = &pnoc_snoc_slv,
};

static const struct qcom_interconnect_desc msm8916_snoc = {
	.nodes = msm8916_snoc_nodes,
	.num_nodes = ARRAY_SIZE(msm8916_snoc_nodes),
};

static struct qcom_interconnect_node snoc_bimc_0_slv = {
	.id = 10025,
	.name = "snoc_bimc_0_slv",
	.links = { 512 }, /* slv_ebi_ch0 */
	.num_links = 1,
	.buswidth = 8,
};

static struct qcom_interconnect_node slv_ebi_ch0 = {
	.id = 512,
	.name = "slv-ebi-ch0",
	.buswidth = 8,
};

static struct qcom_interconnect_node *msm8916_bimc_nodes[] = {
	[SNOC_BIMC_0_SLV] = &snoc_bimc_0_slv,
	[SLV_EBI_CH0] = &slv_ebi_ch0,
};

static struct qcom_interconnect_desc msm8916_bimc = {
	.nodes = msm8916_bimc_nodes,
	.num_nodes = ARRAY_SIZE(msm8916_bimc_nodes),
};

static struct qcom_interconnect_node pnoc_int_1 = {
	.id = 10013,
	.name = "pnoc-int-1",
	.links = { 10010 }, /* pnoc_snoc_mas */
	.num_links = 1,
	.buswidth = 8,
};

static struct qcom_interconnect_node mas_pnoc_sdcc_1 = {
	.id = 78,
	.name = "mas-pnoc-sdcc-1",
	.links = { 10013 }, /* pnoc_int_1 */
	.num_links = 1,
	.port = 7,
	.buswidth = 8,
};

static struct qcom_interconnect_node mas_pnoc_sdcc_2 = {
	.id = 81,
	.name = "mas-pnoc-sdcc-2",
	.links = { 10013 }, /* pnoc_int_1 */
	.num_links = 1,
	.port = 8,
	.buswidth = 8,
};

static struct qcom_interconnect_node pnoc_snoc_mas = {
	.id = 10010,
	.name = "pnoc-snoc-mas",
	.links = { 10011 }, /* pnoc_snoc_slv */
	.num_links = 1,
	.buswidth = 8,
};

static struct qcom_interconnect_node *msm8916_pnoc_nodes[] = {
	[PNOC_INT_1] = &pnoc_int_1,
	[MAS_PNOC_SDCC_1] = &mas_pnoc_sdcc_1,
	[MAS_PNOC_SDCC_2] = &mas_pnoc_sdcc_2,
	[PNOC_SNOC_MAS] = &pnoc_snoc_mas,
};

static const struct qcom_interconnect_desc msm8916_pnoc = {
	.nodes = msm8916_pnoc_nodes,
	.num_nodes = ARRAY_SIZE(msm8916_pnoc_nodes),
};

static int qcom_interconnect_init(struct interconnect_node *node)
{
	struct qcom_interconnect_node *qn = to_qcom_node(node);

	if (!qn->buswidth)
		qn->buswidth = 8;

	/* TODO: init qos and priority */

	return 0;
}

static uint64_t qcom_div64(unsigned int w, uint64_t bw)
{
	uint64_t *b = &bw;

	if ((bw > 0) && (bw < w))
		return 1;

	switch (w) {
	case 0:
		WARN(1, "AXI: Divide by 0 attempted\n");
	case 1: return bw;
	case 2: return (bw >> 1);
	case 4: return (bw >> 2);
	case 8: return (bw >> 3);
	case 16: return (bw >> 4);
	case 32: return (bw >> 5);
	}

	do_div(*b, w);
	return *b;
}

static u64 arbitrate_bus_req(struct qcom_interconnect_node *qn)
{
	struct icp *icp = qn->node.icp;
	struct interconnect_node *node;
	u64 max_ib = 0;
	u64 sum_ab = 0;
	u64 bw_max_hz = 0;

	list_for_each_entry(node, &icp->nodes, icn_list) {
		struct qcom_interconnect_node *tmp = to_qcom_node(node);

		max_ib = max(max_ib, tmp->ib);
		sum_ab += tmp->ab;
	}

	sum_ab *= 100;
	sum_ab = qcom_div64(100, sum_ab);
	max_ib *= 100;
	max_ib = qcom_div64(100, max_ib);

	/* TODO: account for multiple channels if any */

	bw_max_hz = max(max_ib, sum_ab);
	bw_max_hz = qcom_div64(qn->buswidth, bw_max_hz);

	return bw_max_hz;
}

static int qcom_interconnect_set(struct interconnect_node *node, u32 bandwidth)
{
	struct qcom_interconnect_node *qn = to_qcom_node(node);
	struct qcom_interconnect_provider *qicp = to_qcom_icp(node->icp);
	struct icn_qos *req;
	u64 rate;
	int ret;

	/* aggregate bandwidth requests */
	hlist_for_each_entry(req, &node->qos_list, node) {
		req->bandwidth += bandwidth;
	}

	if (qn->ab != bandwidth)
		qn->ab = bandwidth;

	rate = arbitrate_bus_req(qn);

	if (qn->rate != rate) {

		ret = clk_set_rate(qicp->bus_clk, rate);
		if (ret) {
			pr_err("set clk rate %lld error %d\n", rate, ret);
			return ret;
		}

		ret = clk_set_rate(qicp->bus_a_clk, rate);
		if (ret) {
			pr_err("set clk rate %lld error %d\n", rate, ret);
			return ret;
		}

		qn->rate = rate;
	}

	/* TODO: set bw */

	/* TODO: commit */

	return ret;
}

static struct qcom_interconnect_node *get_qcom_node_by_id(struct qcom_interconnect_provider *qicp,
							  unsigned int id)
{
	struct qcom_interconnect_node *qn;
	struct interconnect_node *node;

	list_for_each_entry(node, &qicp->icp.nodes, icn_list) {
		qn = to_qcom_node(node);
		if (qn->id == id)
			return qn;
	}

	return NULL;
}

struct interconnect_onecell_data {
	struct interconnect_node **nodes;
	unsigned int num_nodes;
};

static struct interconnect_node *qcom_xlate(struct of_phandle_args *spec, void *data)
{
	struct interconnect_onecell_data *pdata = data;
	unsigned int idx = spec->args[0];

	if (spec->args_count != 1)
		return ERR_PTR(-EINVAL);

	if (idx >= pdata->num_nodes)
		return ERR_PTR(-ENXIO);

	if (!pdata->nodes[idx])
		return ERR_PTR(-ENOENT);

	return pdata->nodes[idx];
}

static const struct icp_ops qcom_ops = {
	.set = qcom_interconnect_set,
	.xlate = qcom_xlate,
};

static int qnoc_probe(struct platform_device *pdev)
{
	struct qcom_interconnect_provider *qicp;
	struct icp *icp;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource *res;
	void __iomem *base;
	struct clk *bus_clk, *bus_a_clk;
	size_t num_nodes, i;
	const struct qcom_interconnect_desc *desc;
	struct interconnect_node *node;
	struct qcom_interconnect_node **nodes;
	struct interconnect_onecell_data *data;
	u32 type, base_offset, qos_offset = 0;

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	nodes = desc->nodes;
	num_nodes = desc->num_nodes;

	qicp = devm_kzalloc(dev, sizeof(*qicp), GFP_KERNEL);
	if (!qicp)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	bus_clk = devm_clk_get(&pdev->dev, "bus_clk");
	if (IS_ERR(bus_clk))
		return PTR_ERR(bus_clk);
	bus_a_clk = devm_clk_get(&pdev->dev, "bus_a_clk");
	if (IS_ERR(bus_a_clk))
		return PTR_ERR(bus_clk);

	of_property_read_u32(np, "type", &type);
	of_property_read_u32(np, "base-offset", &base_offset);
	of_property_read_u32(np, "qos-offset", &qos_offset);

	qicp->base = base;
	qicp->type = type;
	qicp->base_offset = base_offset;
	qicp->qos_offset = qos_offset;
	qicp->bus_clk = bus_clk;
	qicp->bus_a_clk = bus_a_clk;
	icp = &qicp->icp;
	icp->dev = dev;
	icp->name = np->name;
	icp->of_node = of_node_get(np);
	icp->ops = &qcom_ops;
	INIT_LIST_HEAD(&icp->nodes);

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	icp->data = data;
	data->num_nodes = num_nodes;

	data->nodes = devm_kcalloc(dev, num_nodes, sizeof(*node), GFP_KERNEL);
	if (!data->nodes)
		return -ENOMEM;

	for (i = 0; i < num_nodes; i++) {
		struct qcom_interconnect_node *qn;
		int ret;

		if (!nodes[i])
			continue;

		qn = devm_kzalloc(dev, sizeof(*qn), GFP_KERNEL);
		qn->node.icp = icp;
		qn->node.num_links = nodes[i]->num_links;
		qn->node.links = devm_kcalloc(dev, qn->node.num_links,
					      sizeof(*qn->node.links),
					      GFP_KERNEL);
		if (!qn->node.links)
			return -ENOMEM;

		qn->id = nodes[i]->id;
		qn->name = nodes[i]->name;
		qn->buswidth = nodes[i]->buswidth;
		qn->port = nodes[i]->port;

		data->nodes[i] = &qn->node;
		list_add_tail(&qn->node.icn_list, &icp->nodes);
		dev_info(&pdev->dev, "registered interconnect node %p %s\n",
			 &qn->node, qn->name);

		ret = qcom_interconnect_init(&qn->node);
		if (ret)
			dev_err(&pdev->dev, "%s node init error\n", qn->name);
	}

	/* populate links */
	for (i = 0; i < data->num_nodes; i++) {
		struct interconnect_node *pn = data->nodes[i];
		size_t j;

		if (!pn || !pn->num_links)
			continue;

		for (j = 0; j < pn->num_links; j++) {
			struct qcom_interconnect_node *dst;

			dst = get_qcom_node_by_id(qicp, nodes[i]->links[j]);
			if (dst) {
				pn->links[j] = &dst->node;
			} else {
				pr_err("%s: link not found %u\n", icp->name,
				       nodes[i]->links[j]);
			}
		}
	}

	return interconnect_add_provider(icp);
}

static const struct of_device_id qnoc_of_match[] = {
	{ .compatible = "qcom,msm-bus-pnoc", .data = &msm8916_pnoc },
	{ .compatible = "qcom,msm-bus-snoc", .data = &msm8916_snoc },
	{ .compatible = "qcom,msm-bus-bimc", .data = &msm8916_bimc },
	{ },
};
MODULE_DEVICE_TABLE(of, qnoc_of_match);

static struct platform_driver qnoc_driver = {
	.probe = qnoc_probe,
	.driver = {
		.name = "qcom,qnoc",
		.of_match_table = qnoc_of_match,
	},
};
module_platform_driver(qnoc_driver);
MODULE_AUTHOR("Georgi Djakov <georgi.djakov@linaro.org>");
MODULE_DESCRIPTION("Qualcomm msm8916 NoC driver");
MODULE_LICENSE("GPL v2");
