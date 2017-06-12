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

#include <asm/div64.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/interconnect-provider.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <dt-bindings/interconnect/qcom,msm8916.h>

#define to_qcom_icp(_icp) \
	container_of(_icp, struct qcom_interconnect_provider, icp)
#define to_qcom_node(_node) \
	container_of(_node, struct qcom_interconnect_node, node)

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
	struct interconnect_node node;
	unsigned char *name;
	struct interconnect_node *links[8];
	u16 id;
	u16 num_links;
	u16 port;
	u16 buswidth;
	u64 rate;
};

static struct qcom_interconnect_node snoc_int_0;
static struct qcom_interconnect_node snoc_int_1;
static struct qcom_interconnect_node snoc_int_bimc;
static struct qcom_interconnect_node snoc_bimc_0_mas;
static struct qcom_interconnect_node pnoc_snoc_slv;

static struct qcom_interconnect_node snoc_bimc_0_slv;
static struct qcom_interconnect_node slv_ebi_ch0;

static struct qcom_interconnect_node pnoc_int_1;
static struct qcom_interconnect_node mas_pnoc_sdcc_1;
static struct qcom_interconnect_node mas_pnoc_sdcc_2;
static struct qcom_interconnect_node pnoc_snoc_mas;

struct qcom_interconnect_desc {
	struct qcom_interconnect_node **nodes;
	size_t num_nodes;
};

static struct qcom_interconnect_node snoc_int_0 = {
	.id = 10004,
	.name = "snoc-int-0",
	/*.links = { &snoc_pnoc_mas.node },
	.num_links = 1,*/
	.buswidth = 8,
};

static struct qcom_interconnect_node snoc_int_1 = {
	.id = 10005,
	.name = "snoc-int-1",
	/*.links = { &slv_apss.node, &slv_cats_0.node, &slv_cats_1.node },
	.num_links = 3,*/
	.buswidth = 8,
};

static struct qcom_interconnect_node snoc_int_bimc = {
	.id = 10006,
	.name = "snoc-bimc",
	.links = { &snoc_bimc_0_mas.node },
	.num_links = 1,
	.buswidth = 8,
};

static struct qcom_interconnect_node snoc_bimc_0_mas = {
	.id = 10007,
	.name = "snoc-bimc-0-mas",
	.links = { &snoc_bimc_0_slv.node },
	.num_links = 1,
	.buswidth = 8,
};

static struct qcom_interconnect_node pnoc_snoc_slv = {
	.id = 10011,
	.name = "snoc-pnoc",
	.links = { &snoc_int_0.node, &snoc_int_bimc.node, &snoc_int_1.node },
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

static struct qcom_interconnect_desc msm8916_snoc = {
	.nodes = msm8916_snoc_nodes,
	.num_nodes = ARRAY_SIZE(msm8916_snoc_nodes),
};

static struct qcom_interconnect_node snoc_bimc_0_slv = {
	.id = 10025,
	.name = "snoc_bimc_0_slv",
	.links = { &slv_ebi_ch0.node },
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
	.links = { &pnoc_snoc_mas.node },
	.num_links = 1,
	.buswidth = 8,
};

static struct qcom_interconnect_node mas_pnoc_sdcc_1 = {
	.id = 78,
	.name = "mas-pnoc-sdcc-1",
	.links = { &pnoc_int_1.node },
	.num_links = 1,
	.port = 7,
	.buswidth = 8,
};

static struct qcom_interconnect_node mas_pnoc_sdcc_2 = {
	.id = 81,
	.name = "mas-pnoc-sdcc-2",
	.links = { &pnoc_int_1.node },
	.num_links = 1,
	.port = 8,
	.buswidth = 8,
};

static struct qcom_interconnect_node pnoc_snoc_mas = {
	.id = 10010,
	.name = "pnoc-snoc-mas",
	.links = { &pnoc_snoc_slv.node },
	.num_links = 1,
	.buswidth = 8,
};

static struct qcom_interconnect_node *msm8916_pnoc_nodes[] = {
	[PNOC_INT_1] = &pnoc_int_1,
	[MAS_PNOC_SDCC_1] = &mas_pnoc_sdcc_1,
	[MAS_PNOC_SDCC_2] = &mas_pnoc_sdcc_2,
	[PNOC_SNOC_MAS] = &pnoc_snoc_mas,
};

static struct qcom_interconnect_desc msm8916_pnoc = {
	.nodes = msm8916_pnoc_nodes,
	.num_nodes = ARRAY_SIZE(msm8916_pnoc_nodes),
};

static int qcom_interconnect_init(struct interconnect_node *node)
{
	struct qcom_interconnect_node *qn = to_qcom_node(node);
	int ret = 0;

	/* populate default values */
	if (!qn->buswidth)
		qn->buswidth = 8;

	/* TODO: init qos and priority */

	return ret;
}

static int qcom_interconnect_aggregate(struct interconnect_node *node,
				       struct interconnect_creq *creq)
{
	struct interconnect_node *n;
	struct interconnect_req *r;
	struct icp *icp = node->icp;
	u32 avg_bw = 0;
	u32 max_bw = 0;

	list_for_each_entry(n, &node->icp->nodes, icn_list) {
		hlist_for_each_entry(r, &n->req_list, req_node) {
			if (n == node) {
				/* update constraints */
				r->avg_bw = creq->avg_bw;
				r->max_bw = creq->max_bw;
			}
			avg_bw += r->avg_bw;
			max_bw = max(max_bw, r->max_bw);
		}
	}

	/* save the aggregated values */
	icp->creq.avg_bw = avg_bw;
	icp->creq.max_bw = max_bw;

	return 0;
}

static int qcom_interconnect_set(struct interconnect_node *src,
				 struct interconnect_node *dst,
				 struct interconnect_creq *creq)
{
	struct qcom_interconnect_provider *qicp;
	struct qcom_interconnect_node *qn;
	struct interconnect_node *node;
	struct icp *icp;
	u64 rate = 0;
	int ret = 0;

	if (!src && !dst)
		return -ENODEV;

	if (!src)
		node = dst;
	else
		node = src;

	qn = to_qcom_node(node);
	icp = qn->node.icp;
	qicp = to_qcom_icp(node->icp);

	rate = max(icp->creq.avg_bw, icp->creq.max_bw);

	do_div(rate, qn->buswidth);

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

	return ret;
}

struct interconnect_onecell_data {
	struct interconnect_node **nodes;
	unsigned int num_nodes;
};

static const struct icp_ops qcom_ops = {
	.aggregate = qcom_interconnect_aggregate,
	.set = qcom_interconnect_set,
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
	struct qcom_interconnect_node **qnodes;
	struct interconnect_node *nodes;
	struct interconnect_onecell_data *data;
	u32 type, base_offset, qos_offset = 0;

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	qnodes = desc->nodes;
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
		return PTR_ERR(bus_a_clk);

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
	icp->ops = &qcom_ops;
	INIT_LIST_HEAD(&icp->nodes);

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	icp->data = data;
	data->num_nodes = num_nodes;

	data->nodes = devm_kcalloc(dev, num_nodes, sizeof(*nodes), GFP_KERNEL);
	if (!data->nodes)
		return -ENOMEM;

	for (i = 0; i < num_nodes; i++) {
		struct interconnect_node *node;
		int ret;
		size_t j;

		if (!qnodes[i])
			continue;

		node = &qnodes[i]->node;
		node->dev_id = kstrdup_const(qnodes[i]->name, GFP_KERNEL);
		node->con_id = qnodes[i]->id;
		node->icp = icp;
		node->num_links = qnodes[i]->num_links;
		node->links = devm_kcalloc(dev, node->num_links,
				sizeof(*node->links), GFP_KERNEL);
		if (!node->links)
			return -ENOMEM;

		/* populate links */
		for (j = 0; j < node->num_links; j++)
			node->links[j] = qnodes[i]->links[j];

		/* add node to interconnect provider */
		data->nodes[i] = node;
		list_add_tail(&node->icn_list, &icp->nodes);
		dev_dbg(&pdev->dev, "registered node %p %s %d\n", node,
			node->dev_id, node->con_id);

		ret = qcom_interconnect_init(node);
		if (ret)
			dev_err(&pdev->dev, "node init error (%d)\n", ret);
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
