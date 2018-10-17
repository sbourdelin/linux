/*
 * pvpanic mmio device driver
 *
 * Copyright (C) 2018 ZTE Ltd.
 * Author: Peng Hao <peng.hao2@zte.com.cn>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <asm/virt.h>
#include <linux/platform_device.h>

#define PVPANIC_MMIO_CRASHED	(1 << 0)

struct pvpanic_mmio_device {
	void __iomem *base;
};

static struct pvpanic_mmio_device pvpanic_mmio_dev;

static void
pvpanic_mmio_trigger_event(unsigned int event)
{
	writeb(event, pvpanic_mmio_dev.base);
}

static int
pvpanic_mmio_crash_notify(struct notifier_block *nb, unsigned long code,
			void *unused)
{
	pvpanic_mmio_trigger_event(PVPANIC_MMIO_CRASHED);
	return NOTIFY_DONE;
}

static struct notifier_block pvpanic_mmio_crash_nb = {
	.notifier_call = pvpanic_mmio_crash_notify,
	.priority = 1,
};

static int pvpanic_mmio_probe(struct platform_device *pdev)
{
	struct resource *mem;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem)
		return -EINVAL;

	if (!devm_request_mem_region(&pdev->dev, mem->start,
				resource_size(mem), pdev->name))
		return -EBUSY;

	pvpanic_mmio_dev.base = devm_ioremap(&pdev->dev, mem->start,
										resource_size(mem));
	if (pvpanic_mmio_dev.base == NULL)
		return -EFAULT;

	platform_set_drvdata(pdev, &pvpanic_mmio_dev);

	atomic_notifier_chain_register(&panic_notifier_list,
								  &pvpanic_mmio_crash_nb);

	return 0;
}


static int pvpanic_mmio_remove(struct platform_device *pdev)
{

	atomic_notifier_chain_unregister(&panic_notifier_list,
					&pvpanic_mmio_crash_nb);
	devm_kfree(&pdev->dev, &pvpanic_mmio_dev);
	return 0;
}

static const struct of_device_id pvpanic_mmio_match[] = {
	{ .compatible = "pvpanic,mmio", },
	{},
};
MODULE_DEVICE_TABLE(of, pvpanic_mmio_match);

static struct platform_driver pvpanic_mmio_driver = {
	.probe =        pvpanic_mmio_probe,
	.remove =       pvpanic_mmio_remove,
	.driver = {
		.name =	"pvpanic-mmio",
		.of_match_table = pvpanic_mmio_match,
	},
};

static int __init pvpanic_mmio_init(void)
{
	return platform_driver_register(&pvpanic_mmio_driver);
}

static void __exit pvpanic_mmio_exit(void)
{
	platform_driver_unregister(&pvpanic_mmio_driver);
}

module_init(pvpanic_mmio_init);
module_exit(pvpanic_mmio_exit);

MODULE_AUTHOR("Peng Hao<peng.hao2@zte.com.cn>");
MODULE_DESCRIPTION("pvpanic mmio device driver");
MODULE_LICENSE("GPL");
