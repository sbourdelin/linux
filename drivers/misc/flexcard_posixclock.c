/*
 * Eberspächer Flexcard PMC II - posix clock driver
 *
 * Copyright (c) 2014 - 2016, Linutronix GmbH
 * Author: Benedikt Spranger <b.spranger@linutronix.de>
 *         Holger Dengler <dengler@linutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/flexcard.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/posix-clock.h>
#include <linux/mfd/core.h>
#include <linux/mfd/flexcard.h>

#define MAX_CLOCKS   16
#define CLKSEL_OFF   0x10

#define FLEXCARD_RST_TS		0x8000

#define FLEXCARD_CLK_1MHZ	0

static dev_t flexcard_clk_devt;
static struct class *flexcard_clk_class;

struct flexcard_clk {
	struct posix_clock	clock;
	dev_t			devid;
	struct device		*dev;
	void __iomem		*ts64;
	void __iomem		*reset;
	u32			mul;
	struct flexcard_clk_desc desc;
};

static int flexcard_clk_getres(struct posix_clock *pc, struct timespec *tp)
{
	struct flexcard_clk *clk = container_of(pc, struct flexcard_clk, clock);
	tp->tv_sec = 0;
	tp->tv_nsec = clk->mul;

	return 0;
}

static int flexcard_clk_gettime(struct posix_clock *pc, struct timespec *tp)
{
	struct flexcard_clk *clk = container_of(pc, struct flexcard_clk, clock);
	u64 now;
	u32 upper, rem;

retry:
	upper = readl(clk->ts64);
	now = ((u64) upper << 32) | readl(clk->ts64 + 4);
	if (upper != readl(clk->ts64))
		goto retry;

	tp->tv_sec = div_u64_rem(now, clk->desc.freq, &rem);
	tp->tv_nsec = rem * clk->mul;

	return 0;
}

static int flexcard_clk_settime(struct posix_clock *pc,
				  const struct timespec *tp)
{
	struct flexcard_clk *clk = container_of(pc, struct flexcard_clk, clock);

	/* The FlexCard posix clock could only be reset to 0 and not set */
	if (tp->tv_sec || tp->tv_nsec)
		return -EINVAL;

	writel(FLEXCARD_RST_TS, clk->reset);

	return 0;
}

static long flexcard_clk_ioctl(struct posix_clock *pc, unsigned int cmd,
			       unsigned long arg)
{
	struct flexcard_clk *clk = container_of(pc, struct flexcard_clk, clock);
	struct flexcard_clk_desc desc;

	switch (cmd) {
	case FCSCLKSRC:
		if (copy_from_user(&desc, (void __user *)arg, sizeof(desc)))
			return -EFAULT;

		switch (desc.type) {
		case FLEXCARD_CLK_1MHZ:
			desc.freq = 1000000;
			break;
		case FLEXCARD_CLK_10MHZ:
			desc.freq = 10000000;
			break;
		case FLEXCARD_CLK_100MHZ:
			desc.freq = 100000000;
			break;
		case FLEXCARD_CLK_EXT1:
		case FLEXCARD_CLK_EXT2:
			if (desc.freq < 1 || desc.freq > NSEC_PER_SEC)
				return -EINVAL;
			break;
		default:
			return -EINVAL;
		}

		clk->desc = desc;
		clk->mul = NSEC_PER_SEC/desc.freq;
		writel(clk->desc.type, clk->ts64 + CLKSEL_OFF);
		writel(FLEXCARD_RST_TS, clk->reset);

		break;
	case FCGCLKSRC:
		if (copy_to_user((void __user *)arg, &clk->desc, sizeof(desc)))
			return -EFAULT;
		break;
	default:
		return -ENOTTY;
	}

	return 0;
}

static struct posix_clock_operations flexcard_clk_ops = {
	.owner		= THIS_MODULE,
	.clock_getres	= flexcard_clk_getres,
	.clock_gettime	= flexcard_clk_gettime,
	.clock_settime	= flexcard_clk_settime,
	.ioctl		= flexcard_clk_ioctl,
};

static int flexcard_clk_iomap(struct platform_device *pdev)
{
	struct flexcard_clk *clk = platform_get_drvdata(pdev);
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENXIO;

	clk->ts64 = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!clk->ts64)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res)
		return -ENXIO;

	clk->reset = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!clk->reset)
		return -ENOMEM;

	return 0;
}

static int flexcard_clk_probe(struct platform_device *pdev)
{
	const struct mfd_cell *cell;
	struct flexcard_clk *clk;
	int major, ret;

	cell = mfd_get_cell(pdev);
	if (!cell)
		return -ENODEV;

	if (cell->id >= MAX_CLOCKS) {
		dev_err(&pdev->dev, "all flexcard posix clocks in use: %d\n",
			cell->id);
		return -EBUSY;
	}

	clk = devm_kzalloc(&pdev->dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return -ENOMEM;

	major = MAJOR(flexcard_clk_devt);
	platform_set_drvdata(pdev, clk);

	ret = flexcard_clk_iomap(pdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to map resources: %d\n", ret);
		goto out;
	}

	clk->devid = MKDEV(major, cell->id);
	clk->clock.ops = flexcard_clk_ops;
	clk->desc.type = FLEXCARD_CLK_1MHZ;
	clk->desc.freq = 1000000;
	clk->mul = 1000;

	writel(clk->desc.type, clk->ts64 + CLKSEL_OFF);
	writel(FLEXCARD_RST_TS, clk->reset);

	clk->dev = device_create(flexcard_clk_class, &pdev->dev, clk->devid,
				 clk, "flexcard_clock%d", cell->id);
	if (IS_ERR(clk->dev)) {
		ret = PTR_ERR(clk->dev);
		goto out;
	}

	ret = posix_clock_register(&clk->clock, clk->devid);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to register flexcard posix clock: %d\n", ret);
		goto out_destroy;
	}

	dev_info(&pdev->dev, "flexcard posix clock %d registered", cell->id);

	return 0;

out_destroy:
	device_destroy(flexcard_clk_class, clk->devid);
out:
	return ret;
}

static int flexcard_clk_remove(struct platform_device *pdev)
{
	struct flexcard_clk *clk = platform_get_drvdata(pdev);

	posix_clock_unregister(&clk->clock);
	device_destroy(flexcard_clk_class, clk->devid);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static const struct platform_device_id flexcard_clk_id_table[] = {
	{ "flexcard-clock", 0 },
	{},
};
MODULE_DEVICE_TABLE(platform, flexcard_clk_id_table);

static struct platform_driver flexcard_clk_driver = {
	.probe		= flexcard_clk_probe,
	.remove		= flexcard_clk_remove,
	.driver		= {
		.name	= "flexcard-clock",
	},
	.id_table	= flexcard_clk_id_table,
};

static int __init flexcard_clk_init(void)
{
	int ret;

	flexcard_clk_class = class_create(THIS_MODULE, "flexcard_clock");
	if (IS_ERR(flexcard_clk_class)) {
		pr_err("flexcard_clock: failed to allocate class\n");
		return PTR_ERR(flexcard_clk_class);
	}

	ret = alloc_chrdev_region(&flexcard_clk_devt, 0, MAX_CLOCKS,
				  "flexcard_clock");
	if (ret < 0) {
		pr_err("failed to allocate device region\n");
		goto out;
	}

	ret = platform_driver_register(&flexcard_clk_driver);
	if (ret < 0)
		goto out_unregister;

	return 0;

out_unregister:
	unregister_chrdev_region(flexcard_clk_devt, MAX_CLOCKS);
out:
	class_destroy(flexcard_clk_class);

	return ret;
}

static void __exit flexcard_clk_exit(void)
{
	platform_driver_unregister(&flexcard_clk_driver);
	unregister_chrdev_region(flexcard_clk_devt, MAX_CLOCKS);
	class_destroy(flexcard_clk_class);
}

module_init(flexcard_clk_init);
module_exit(flexcard_clk_exit);

MODULE_AUTHOR("Holger Dengler <dengler@linutronix.de>");
MODULE_AUTHOR("Benedikt Spranger <b.spranger@linutronix.de>");
MODULE_DESCRIPTION("Eberspaecher Flexcard PMC II posix clock driver");
MODULE_LICENSE("GPL v2");
