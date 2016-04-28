/*
 * Copyright (c) 2016 Hisilicon Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _HNS_ROCE_DEVICE_H
#define _HNS_ROCE_DEVICE_H

#include <linux/platform_device.h>
#include <linux/radix-tree.h>
#include <linux/semaphore.h>
#include <rdma/ib_addr.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_verbs.h>

#define DRV_NAME "hns_roce"

#define HNS_ROCE_MAX_IRQ_NUM			34
#define HNS_ROCE_MAX_PORTS			6

struct hns_roce_ib_iboe {
	struct net_device      *netdevs[HNS_ROCE_MAX_PORTS];
	u8			phy_port[HNS_ROCE_MAX_PORTS];
};

struct hns_roce_caps {
	u8			num_ports;
};

struct hns_roce_hw {
	int (*reset)(struct hns_roce_dev *hr_dev, u32 val);
};

struct hns_roce_dev {
	struct ib_device	ib_dev;
	struct platform_device  *pdev;
	struct hns_roce_ib_iboe iboe;

	int			irq[HNS_ROCE_MAX_IRQ_NUM];
	u8 __iomem		*reg_base;
	struct hns_roce_caps	caps;

	int			cmd_mod;
	int			loop_idc;
	struct hns_roce_hw	*hw;
};

extern struct hns_roce_hw hns_roce_hw_v1;

#endif /* _HNS_ROCE_DEVICE_H */
