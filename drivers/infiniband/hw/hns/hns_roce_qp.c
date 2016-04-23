/*
 * Copyright (c) 2016 Hisilicon Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/log2.h>
#include <linux/slab.h>
#include <rdma/ib_cache.h>
#include <rdma/ib_pack.h>
#include "hns_roce_device.h"

#define SQP_NUM				12

void hns_roce_qp_event(struct hns_roce_dev *hr_dev, u32 qpn, int event_type)
{
	struct hns_roce_qp_table *qp_table = &hr_dev->qp_table;
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_qp *qp;

	spin_lock(&qp_table->lock);

	qp = __hns_roce_qp_lookup(hr_dev, qpn);
	if (qp)
		atomic_inc(&qp->refcount);

	spin_unlock(&qp_table->lock);

	if (!qp) {
		dev_warn(dev, "Async event for bogus QP %08x\n", qpn);
		return;
	}

	qp->event(qp, (enum hns_roce_event)event_type);

	if (atomic_dec_and_test(&qp->refcount))
		complete(&qp->free);
}

int hns_roce_init_qp_table(struct hns_roce_dev *hr_dev)
{
	struct hns_roce_qp_table *qp_table = &hr_dev->qp_table;
	int reserved_from_top = 0;
	int ret;

	spin_lock_init(&qp_table->lock);
	INIT_RADIX_TREE(&hr_dev->qp_table_tree, GFP_ATOMIC);

	/* A port include two SQP, six port total 12 */
	ret = hns_roce_bitmap_init(&qp_table->bitmap, hr_dev->caps.num_qps,
				   hr_dev->caps.num_qps - 1,
				   hr_dev->caps.sqp_start + SQP_NUM,
				   reserved_from_top);
	if (ret) {
		dev_err(&hr_dev->pdev->dev, "qp bitmap init failed!error=%d\n",
			ret);
		return ret;
	}

	return 0;
}

void hns_roce_cleanup_qp_table(struct hns_roce_dev *hr_dev)
{
	hns_roce_bitmap_cleanup(&hr_dev->qp_table.bitmap);
}
