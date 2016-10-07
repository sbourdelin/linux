/*
 * Watchdog driver for PTX PMB CPLD based watchdog
 *
 * Copyright (C) 2012 Juniper Networks
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/watchdog.h>
#include <linux/reboot.h>
#include <linux/notifier.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/watchdog.h>
#include <linux/spinlock.h>
#include <linux/mfd/ptxpmb_cpld.h>

#define DRV_NAME "jnx-ptxpmb-wdt"

/*
 * Since we can't really expect userspace to be responsive enough before a
 * watchdog overflow happens, we maintain two separate timers .. One in
 * the kernel for clearing out the watchdog every second, and another for
 * monitoring userspace writes to the WDT device.
 *
 * As such, we currently use a configurable heartbeat interval which defaults
 * to 30s. In this case, the userspace daemon is only responsible for periodic
 * writes to the device before the next heartbeat is scheduled. If the daemon
 * misses its deadline, the kernel timer will allow the WDT to overflow.
 */

#define WD_MIN_TIMEOUT		1
#define WD_MAX_TIMEOUT		65535
#define WD_DEFAULT_TIMEOUT	30	/* 30 sec default heartbeat */

struct ptxpmb_wdt {
	struct pmb_boot_cpld __iomem *cpld;
	struct device		*dev;
	spinlock_t		lock;
	struct timer_list	timer;
	unsigned long		next_heartbeat;
};

static void ptxpmb_wdt_enable(struct watchdog_device *wdog)
{
	struct ptxpmb_wdt *wdt = watchdog_get_drvdata(wdog);

	wdt->next_heartbeat = jiffies + wdog->timeout * HZ;
	mod_timer(&wdt->timer, jiffies + HZ);

	iowrite8(0, &wdt->cpld->watchdog_hbyte);
	iowrite8(0, &wdt->cpld->watchdog_lbyte);
	iowrite8(ioread8(&wdt->cpld->control) | 0x40, &wdt->cpld->control);
}

static void ptxpmb_wdt_disable(struct watchdog_device *wdog)
{
	struct ptxpmb_wdt *wdt = watchdog_get_drvdata(wdog);

	del_timer(&wdt->timer);

	iowrite8(ioread8(&wdt->cpld->control) & ~0x40, &wdt->cpld->control);
}

static int ptxpmb_wdt_keepalive(struct watchdog_device *wdog)
{
	struct ptxpmb_wdt *wdt = watchdog_get_drvdata(wdog);
	unsigned long flags;

	spin_lock_irqsave(&wdt->lock, flags);
	wdt->next_heartbeat = jiffies + (wdog->timeout * HZ);
	spin_unlock_irqrestore(&wdt->lock, flags);

	return 0;
}

static int ptxpmb_wdt_set_timeout(struct watchdog_device *wdog, unsigned int t)
{
	struct ptxpmb_wdt *wdt = watchdog_get_drvdata(wdog);
	unsigned long flags;

	spin_lock_irqsave(&wdt->lock, flags);
	wdog->timeout = t;
	ptxpmb_wdt_enable(wdog);
	spin_unlock_irqrestore(&wdt->lock, flags);

	return 0;
}

static void ptxpmb_wdt_ping(unsigned long data)
{
	struct ptxpmb_wdt *wdt = (struct ptxpmb_wdt *)data;
	unsigned long flags;

	spin_lock_irqsave(&wdt->lock, flags);
	if (time_before(jiffies, wdt->next_heartbeat)) {
		mod_timer(&wdt->timer, jiffies + HZ);
		iowrite8(0, &wdt->cpld->watchdog_hbyte);
		iowrite8(0, &wdt->cpld->watchdog_lbyte);
	} else {
		dev_warn(wdt->dev, "Heartbeat lost! Will not ping the watchdog\n");
	}
	spin_unlock_irqrestore(&wdt->lock, flags);
}

static int ptxpmb_wdt_start(struct watchdog_device *wdog)
{
	struct ptxpmb_wdt *wdt = watchdog_get_drvdata(wdog);
	unsigned long flags;

	spin_lock_irqsave(&wdt->lock, flags);
	ptxpmb_wdt_enable(wdog);
	spin_unlock_irqrestore(&wdt->lock, flags);

	return 0;
}

static int ptxpmb_wdt_stop(struct watchdog_device *wdog)
{
	struct ptxpmb_wdt *wdt = watchdog_get_drvdata(wdog);
	unsigned long flags;

	spin_lock_irqsave(&wdt->lock, flags);
	ptxpmb_wdt_disable(wdog);
	spin_unlock_irqrestore(&wdt->lock, flags);

	return 0;
}

static const struct watchdog_info ptxpmb_wdt_info = {
	.options		= WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT,
	.identity		= "PTX PMB WDT",
};

static const struct watchdog_ops ptxpmb_wdt_ops = {
	.owner = THIS_MODULE,
	.start = ptxpmb_wdt_start,
	.stop = ptxpmb_wdt_stop,
	.ping = ptxpmb_wdt_keepalive,
	.set_timeout = ptxpmb_wdt_set_timeout,
};

static struct watchdog_device *ptxpmb_wdog;

static int ptxpmb_wdt_notify_sys(struct notifier_block *this,
				 unsigned long code, void *unused)
{
	if (code == SYS_DOWN || code == SYS_HALT)
		ptxpmb_wdt_stop(ptxpmb_wdog);

	return NOTIFY_DONE;
}

static struct notifier_block ptxpmb_wdt_notifier = {
	.notifier_call		= ptxpmb_wdt_notify_sys,
};

static int ptxpmb_wdt_probe(struct platform_device *pdev)
{
	struct ptxpmb_wdt *wdt;
	bool nowayout = WATCHDOG_NOWAYOUT;
	struct watchdog_device *wdog;
	struct resource *res;
	struct device *dev = &pdev->dev;
	int rc;

	wdog = devm_kzalloc(dev, sizeof(*wdog), GFP_KERNEL);
	if (unlikely(!wdog))
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (unlikely(!res))
		return -EINVAL;

#if 0
	if (!devm_request_mem_region(dev, res->start,
				     resource_size(res), DRV_NAME))
		return -EBUSY;
#endif

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (unlikely(!wdt))
		return -ENOMEM;

	wdt->dev = dev;

	wdt->cpld = devm_ioremap(dev, res->start, resource_size(res));
	if (unlikely(!wdt->cpld))
		return -ENXIO;

	spin_lock_init(&wdt->lock);

	wdog->info = &ptxpmb_wdt_info;
	wdog->ops = &ptxpmb_wdt_ops;
	wdog->min_timeout = WD_MIN_TIMEOUT;
	wdog->max_timeout = WD_MAX_TIMEOUT;
	wdog->timeout = WD_DEFAULT_TIMEOUT;
	wdog->parent = dev;

	watchdog_set_drvdata(wdog, wdt);
	watchdog_set_nowayout(wdog, nowayout);
	platform_set_drvdata(pdev, wdog);

	ptxpmb_wdt_disable(wdog);
	ptxpmb_wdog = wdog;

	rc = register_reboot_notifier(&ptxpmb_wdt_notifier);
	if (unlikely(rc)) {
		dev_err(dev, "Can't register reboot notifier (err=%d)\n", rc);
		return rc;
	}

	rc = watchdog_register_device(wdog);
	if (rc)
		goto out_unreg;

	init_timer(&wdt->timer);
	wdt->timer.function	= ptxpmb_wdt_ping;
	wdt->timer.data		= (unsigned long)wdt;
	wdt->timer.expires	= jiffies + HZ;

	dev_info(dev, "initialized\n");

	return 0;

out_unreg:
	unregister_reboot_notifier(&ptxpmb_wdt_notifier);
	return rc;
}

static int ptxpmb_wdt_remove(struct platform_device *pdev)
{
	struct watchdog_device *wdog = platform_get_drvdata(pdev);

	unregister_reboot_notifier(&ptxpmb_wdt_notifier);
	watchdog_unregister_device(wdog);

	return 0;
}

static const struct of_device_id ptxpmb_wdt_of_ids[] = {
	{ .compatible = "jnx,ptxpmb-wdt", NULL },
	{ }
};
MODULE_DEVICE_TABLE(of, ptxpmb_wdt_of_ids);

static struct platform_driver ptxpmb_wdt_driver = {
	.driver		= {
		.name	= DRV_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = ptxpmb_wdt_of_ids,
	},

	.probe	= ptxpmb_wdt_probe,
	.remove	= ptxpmb_wdt_remove,
};

static int __init ptxpmb_wdt_init(void)
{
	return platform_driver_register(&ptxpmb_wdt_driver);
}

static void __exit ptxpmb_wdt_exit(void)
{
	platform_driver_unregister(&ptxpmb_wdt_driver);
}
module_init(ptxpmb_wdt_init);
module_exit(ptxpmb_wdt_exit);

MODULE_AUTHOR("Guenter Roeck <groeck@juniper.net>");
MODULE_DESCRIPTION("Juniper PTX PMB CPLD watchdog driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
