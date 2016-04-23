/*
 * Copyright (c) 2016 Hisilicon Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/hardirq.h>
#include <linux/log2.h>
#include <linux/slab.h>
#include "hns_roce_device.h"

void hns_roce_cq_completion(struct hns_roce_dev *hr_dev, u32 cqn)
{
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_cq *cq;

	cq = radix_tree_lookup(&hr_dev->cq_table.tree,
			       cqn & (hr_dev->caps.num_cqs - 1));
	if (!cq) {
		dev_warn(dev, "Completion event for bogus CQ 0x%08x\n", cqn);
		return;
	}

	cq->comp(cq);
}

void hns_roce_cq_event(struct hns_roce_dev *hr_dev, u32 cqn, int event_type)
{
	struct hns_roce_cq_table *cq_table = &hr_dev->cq_table;
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_cq *cq;

	spin_lock(&cq_table->lock);

	cq = radix_tree_lookup(&cq_table->tree,
			       cqn & (hr_dev->caps.num_cqs - 1));
	if (cq)
		atomic_inc(&cq->refcount);

	spin_unlock(&cq_table->lock);

	if (!cq) {
		dev_warn(dev, "Async event for bogus CQ %08x\n", cqn);
		return;
	}

	cq->event(cq, (enum hns_roce_event)event_type);

	if (atomic_dec_and_test(&cq->refcount))
		complete(&cq->free);
}

int hns_roce_init_cq_table(struct hns_roce_dev *hr_dev)
{
	struct hns_roce_cq_table *cq_table = &hr_dev->cq_table;
	struct device *dev = &hr_dev->pdev->dev;
	int ret;

	spin_lock_init(&cq_table->lock);
	INIT_RADIX_TREE(&cq_table->tree, GFP_ATOMIC);

	ret = hns_roce_bitmap_init(&cq_table->bitmap, hr_dev->caps.num_cqs,
				   hr_dev->caps.num_cqs - 1,
				   hr_dev->caps.reserved_cqs, 0);
	if (ret) {
		dev_err(dev, "init_cq_table.Failed to bitmap_init.\n");
		return ret;
	}

	return 0;
}

void hns_roce_cleanup_cq_table(struct hns_roce_dev *hr_dev)
{
	hns_roce_bitmap_cleanup(&hr_dev->cq_table.bitmap);
}
