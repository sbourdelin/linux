// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 The Linux Foundation. All rights reserved.
 */
#include <linux/atomic.h>
#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/rtb.h>
#include <linux/sched/clock.h>

static struct platform_device *rtb_dev;
static atomic_t rtb_idx;

struct rtb_state {
	struct rtb_layout *rtb;
	phys_addr_t phys;
	unsigned int nentries;
	unsigned int size;
	int enabled;
};

static struct rtb_state rtb = {
	.enabled = 0,
};

static int rtb_panic_notifier(struct notifier_block *this,
					unsigned long event, void *ptr)
{
	rtb.enabled = 0;
	return NOTIFY_DONE;
}

static struct notifier_block rtb_panic_blk = {
	.notifier_call  = rtb_panic_notifier,
	.priority = INT_MAX,
};

static void rtb_write_type(const char *log_type,
			struct rtb_layout *start)
{
	start->log_type = log_type;
}

static void rtb_write_caller(u64 caller, struct rtb_layout *start)
{
	start->caller = caller;
}

static void rtb_write_data(u64 data, struct rtb_layout *start)
{
	start->data = data;
}

static void rtb_write_timestamp(struct rtb_layout *start)
{
	start->timestamp = sched_clock();
}

static void uncached_logk_pc_idx(const char *log_type, u64 caller,
				u64 data, int idx)
{
	struct rtb_layout *start;

	start = &rtb.rtb[idx & (rtb.nentries - 1)];

	rtb_write_type(log_type, start);
	rtb_write_caller(caller, start);
	rtb_write_data(data, start);
	rtb_write_timestamp(start);
	/* Make sure data is written */
	mb();
#if defined(CONFIG_PSTORE_RTB)
	pstore_rtb_call(start);
#endif
}

static int rtb_get_idx(void)
{
	int i, offset;

	i = atomic_inc_return(&rtb_idx);
	i--;

	/* Check if index has wrapped around */
	offset = (i & (rtb.nentries - 1)) -
		 ((i - 1) & (rtb.nentries - 1));
	if (offset < 0) {
		i = atomic_inc_return(&rtb_idx);
		i--;
	}

	return i;
}

noinline void notrace uncached_logk(const char *log_type, void *data)
{
	int i;

	if (!rtb.enabled)
		return;

	i = rtb_get_idx();
	uncached_logk_pc_idx(log_type, (u64)(__builtin_return_address(0)),
				(u64)(data), i);
}
EXPORT_SYMBOL(uncached_logk);

int rtb_init(void)
{
	struct device_node *np;
	u32 size;
	int ret;

	np = of_find_node_by_name(NULL, "ramoops");
	if (!np)
		return -ENODEV;

	ret = of_property_read_u32(np, "rtb-size", &size);
	if (ret) {
		of_node_put(np);
		return ret;
	}

	rtb.size = size;

	/* Create a dummy platform device to use dma api */
	rtb_dev = platform_device_register_simple("rtb", -1, NULL, 0);
	if (IS_ERR(rtb_dev))
		return PTR_ERR(rtb_dev);

	/*
	 * The device is a dummy, so arch_setup_dma_ops
	 * is not called, thus leaving the device with dummy DMA ops
	 * which returns null in case of arm64.
	 */
	of_dma_configure(&rtb_dev->dev, NULL, true);
	rtb.rtb = dma_alloc_coherent(&rtb_dev->dev, rtb.size,
					&rtb.phys, GFP_KERNEL);
	if (!rtb.rtb)
		return -ENOMEM;

	rtb.nentries = rtb.size / sizeof(struct rtb_layout);
	/* Round this down to a power of 2 */
	rtb.nentries = __rounddown_pow_of_two(rtb.nentries);

	memset(rtb.rtb, 0, rtb.size);
	atomic_set(&rtb_idx, 0);

	atomic_notifier_chain_register(&panic_notifier_list,
						&rtb_panic_blk);
	rtb.enabled = 1;
	return 0;
}

void rtb_exit(void)
{
	dma_free_coherent(&rtb_dev->dev, rtb.size, rtb.rtb, rtb.phys);
	platform_device_unregister(rtb_dev);
}
