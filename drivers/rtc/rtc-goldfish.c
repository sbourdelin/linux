/* drivers/rtc/rtc-goldfish.c
 *
 * Copyright (C) 2007 Google, Inc.
 * Copyright (C) 2017 Imagination Technologies Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>

#define TIMER_TIME_LOW         0x00	/* get low bits of current time  */
					/*   and update TIMER_TIME_HIGH  */
#define TIMER_TIME_HIGH        0x04	/* get high bits of time at last */
					/*   TIMER_TIME_LOW read         */
#define TIMER_ALARM_LOW        0x08	/* set low bits of alarm and     */
					/*   activate it                 */
#define TIMER_ALARM_HIGH       0x0c	/* set high bits of next alarm   */
#define TIMER_CLEAR_INTERRUPT  0x10
#define TIMER_CLEAR_ALARM      0x14

struct goldfish_rtc {
	void __iomem *base;
	u32 irq;
	struct rtc_device *rtc;
};

static irqreturn_t goldfish_rtc_interrupt(int irq, void *dev_id)
{
	struct goldfish_rtc	*qrtc = dev_id;
	unsigned long		events = 0;
	void __iomem *base = qrtc->base;

	writel(1, base + TIMER_CLEAR_INTERRUPT);
	events = RTC_IRQF | RTC_AF;

	rtc_update_irq(qrtc->rtc, 1, events);

	return IRQ_HANDLED;
}

static int goldfish_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	u64 time;
	u64 time_low;
	u64 time_high;
	u64 time_high_prev;

	struct goldfish_rtc *qrtc =
			platform_get_drvdata(to_platform_device(dev));
	void __iomem *base = qrtc->base;

	time_high = readl(base + TIMER_TIME_HIGH);
	do {
		time_high_prev = time_high;
		time_low = readl(base + TIMER_TIME_LOW);
		time_high = readl(base + TIMER_TIME_HIGH);
	} while (time_high != time_high_prev);
	time = (time_high << 32) | time_low;

	do_div(time, NSEC_PER_SEC);

	rtc_time_to_tm(time, tm);

	return 0;
}

static const struct rtc_class_ops goldfish_rtc_ops = {
	.read_time	= goldfish_rtc_read_time,
};

static int goldfish_rtc_probe(struct platform_device *pdev)
{
	struct resource *r;
	struct goldfish_rtc *qrtc;
	unsigned long rtc_dev_len;
	unsigned long rtc_dev_addr;
	int err;

	qrtc = devm_kzalloc(&pdev->dev, sizeof(*qrtc), GFP_KERNEL);
	if (qrtc == NULL)
		return -ENOMEM;

	platform_set_drvdata(pdev, qrtc);

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (r == NULL)
		return -ENODEV;

	rtc_dev_addr = r->start;
	rtc_dev_len = resource_size(r);
	qrtc->base = devm_ioremap(&pdev->dev, rtc_dev_addr, rtc_dev_len);
	if (IS_ERR(qrtc->base))
		return -ENODEV;

	qrtc->irq = platform_get_irq(pdev, 0);
	if (qrtc->irq < 0)
		return -ENODEV;

	qrtc->rtc = devm_rtc_device_register(&pdev->dev, pdev->name,
					&goldfish_rtc_ops, THIS_MODULE);
	if (IS_ERR(qrtc->rtc))
		return PTR_ERR(qrtc->rtc);

	err = devm_request_irq(&pdev->dev, qrtc->irq, goldfish_rtc_interrupt,
		0, pdev->name, qrtc);
	if (err)
		return err;

	return 0;
}

static const struct of_device_id goldfish_rtc_of_match[] = {
	{ .compatible = "google,goldfish-rtc", },
	{},
};
MODULE_DEVICE_TABLE(of, goldfish_rtc_of_match);

static struct platform_driver goldfish_rtc = {
	.probe = goldfish_rtc_probe,
	.driver = {
		.name = "goldfish_rtc",
		.of_match_table = goldfish_rtc_of_match,
	}
};

module_platform_driver(goldfish_rtc);
