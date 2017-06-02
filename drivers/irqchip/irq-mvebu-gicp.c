/*
 * Copyright (C) 2017 Marvell
 *
 * Thomas Petazzoni <thomas.petazzoni@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "irq-mvebu-gicp.h"

#define GICP_SETSPI_NSR_OFFSET	0x0
#define GICP_CLRSPI_NSR_OFFSET	0x8

struct mvebu_gicp_spi_range {
	unsigned int start;
	unsigned int count;
};

struct mvebu_gicp {
	struct mvebu_gicp_spi_range *spi_ranges;
	unsigned int spi_ranges_cnt;
	unsigned int spi_cnt;
	unsigned long *spi_bitmap;
	spinlock_t spi_lock;
	struct resource *res;
};

int mvebu_gicp_alloc(struct mvebu_gicp *gicp)
{
	int idx;

	spin_lock(&gicp->spi_lock);
	idx = find_first_zero_bit(gicp->spi_bitmap, gicp->spi_cnt);
	if (idx == gicp->spi_cnt) {
		spin_unlock(&gicp->spi_lock);
		return -ENOSPC;
	}
	set_bit(idx, gicp->spi_bitmap);
	spin_unlock(&gicp->spi_lock);

	return idx;
}

void mvebu_gicp_free(struct mvebu_gicp *gicp, int idx)
{
	spin_lock(&gicp->spi_lock);
	clear_bit(idx, gicp->spi_bitmap);
	spin_unlock(&gicp->spi_lock);
}

int mvebu_gicp_idx_to_spi(struct mvebu_gicp *gicp, int idx)
{
	int i;

	for (i = 0; i < gicp->spi_ranges_cnt; i++) {
		struct mvebu_gicp_spi_range *r = &gicp->spi_ranges[i];

		if (idx < r->count)
			return r->start + idx;

		idx -= r->count;
	}

	return -EINVAL;
}

int mvebu_gicp_spi_to_idx(struct mvebu_gicp *gicp, int spi)
{
	int i;
	int idx = 0;

	for (i = 0; i < gicp->spi_ranges_cnt; i++) {
		struct mvebu_gicp_spi_range *r = &gicp->spi_ranges[i];

		if (spi >= r->start && spi < (r->start + r->count))
			return idx + (spi - r->start);

		idx += r->count;
	}

	return -EINVAL;
}

int mvebu_gicp_spi_count(struct mvebu_gicp *gicp)
{
	return gicp->spi_cnt;
}

phys_addr_t mvebu_gicp_setspi_phys_addr(struct mvebu_gicp *gicp)
{
	return gicp->res->start + GICP_SETSPI_NSR_OFFSET;
}

phys_addr_t mvebu_gicp_clrspi_phys_addr(struct mvebu_gicp *gicp)
{
	return gicp->res->start + GICP_CLRSPI_NSR_OFFSET;
}

static int mvebu_gicp_probe(struct platform_device *pdev)
{
	struct mvebu_gicp *gicp;
	int ret, i;

	gicp = devm_kzalloc(&pdev->dev, sizeof(*gicp), GFP_KERNEL);
	if (!gicp)
		return -ENOMEM;

	gicp->res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!gicp->res)
		return -ENODEV;

	ret = of_property_count_u32_elems(pdev->dev.of_node,
					  "marvell,spi-ranges");
	if (ret < 0)
		return ret;

	gicp->spi_ranges_cnt = ret / 2;

	gicp->spi_ranges =
		devm_kzalloc(&pdev->dev,
			     gicp->spi_ranges_cnt *
			     sizeof(struct mvebu_gicp_spi_range),
			     GFP_KERNEL);
	if (!gicp->spi_ranges)
		return -ENOMEM;

	for (i = 0; i < gicp->spi_ranges_cnt; i++) {
		of_property_read_u32_index(pdev->dev.of_node,
					   "marvell,spi-ranges",
					   i * 2,
					   &gicp->spi_ranges[i].start);

		of_property_read_u32_index(pdev->dev.of_node,
					   "marvell,spi-ranges",
					   i * 2 + 1,
					   &gicp->spi_ranges[i].count);

		gicp->spi_cnt += gicp->spi_ranges[i].count;
	}

	gicp->spi_bitmap = devm_kzalloc(&pdev->dev,
					BITS_TO_LONGS(gicp->spi_cnt),
					GFP_KERNEL);
	if (!gicp->spi_bitmap)
		return -ENOMEM;

	platform_set_drvdata(pdev, gicp);

	return 0;
}

static const struct of_device_id mvebu_gicp_of_match[] = {
	{ .compatible = "marvell,ap806-gicp", },
	{},
};

static struct platform_driver mvebu_gicp_driver = {
	.probe  = mvebu_gicp_probe,
	.driver = {
		.name = "mvebu-gicp",
		.of_match_table = mvebu_gicp_of_match,
	},
};
builtin_platform_driver(mvebu_gicp_driver);
