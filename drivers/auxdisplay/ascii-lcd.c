/*
 * Copyright (C) 2015 Imagination Technologies
 * Author: Paul Burton <paul.burton@imgtec.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <generated/utsrelease.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

struct ascii_lcd_ctx;

struct ascii_lcd_config {
	unsigned num_chars;
	void (*update)(struct ascii_lcd_ctx *ctx);
};

struct ascii_lcd_ctx {
	struct platform_device *pdev;
	void __iomem *base;
	const struct ascii_lcd_config *cfg;
	char *message;
	unsigned message_len;
	unsigned scroll_pos;
	unsigned scroll_rate;
	struct timer_list timer;
	char curr[] __aligned(8);
};

static void update_boston(struct ascii_lcd_ctx *ctx)
{
	u32 val32;
	u64 val64;

	if (config_enabled(CONFIG_64BIT)) {
		val64 = *((u64 *)&ctx->curr[0]);
		__raw_writeq(val64, ctx->base);
	} else {
		val32 = *((u32 *)&ctx->curr[0]);
		__raw_writel(val32, ctx->base);
		val32 = *((u32 *)&ctx->curr[4]);
		__raw_writel(val32, ctx->base + 4);
	}
}

static void update_malta(struct ascii_lcd_ctx *ctx)
{
	unsigned i;

	for (i = 0; i < ctx->cfg->num_chars; i++)
		__raw_writel(ctx->curr[i], ctx->base + (i * 8));
}

static struct ascii_lcd_config boston_config = {
	.num_chars = 8,
	.update = update_boston,
};

static struct ascii_lcd_config malta_config = {
	.num_chars = 8,
	.update = update_malta,
};

static const struct of_device_id ascii_lcd_matches[] = {
	{ .compatible = "img,boston-lcd", .data = &boston_config },
	{ .compatible = "mti,malta-lcd", .data = &malta_config },
};

static void ascii_lcd_scroll(unsigned long arg)
{
	struct ascii_lcd_ctx *ctx = (struct ascii_lcd_ctx *)arg;
	unsigned i, ch = ctx->scroll_pos;
	unsigned num_chars = ctx->cfg->num_chars;

	/* update the current message string */
	for (i = 0; i < num_chars;) {
		/* copy as many characters from the string as possible */
		for (; i < num_chars && ch < ctx->message_len; i++, ch++)
			ctx->curr[i] = ctx->message[ch];

		/* wrap around to the start of the string */
		ch = 0;
	}

	/* update the LCD */
	ctx->cfg->update(ctx);

	/* move on to the next character */
	ctx->scroll_pos++;
	ctx->scroll_pos %= ctx->message_len;

	/* rearm the timer */
	if (ctx->message_len > ctx->cfg->num_chars)
		mod_timer(&ctx->timer, jiffies + ctx->scroll_rate);
}

static int ascii_lcd_display(struct ascii_lcd_ctx *ctx,
			     const char *msg, ssize_t count)
{
	char *new_msg;

	/* stop the scroll timer */
	del_timer_sync(&ctx->timer);

	if (count == -1)
		count = strlen(msg);

	/* if the string ends with a newline, trim it */
	if (msg[count - 1] == '\n')
		count--;

	new_msg = devm_kmalloc(&ctx->pdev->dev, count + 1, GFP_KERNEL);
	if (!new_msg)
		return -ENOMEM;

	memcpy(new_msg, msg, count);
	new_msg[count] = 0;

	if (ctx->message)
		devm_kfree(&ctx->pdev->dev, ctx->message);

	ctx->message = new_msg;
	ctx->message_len = count;
	ctx->scroll_pos = 0;

	/* update the LCD */
	ascii_lcd_scroll((unsigned long)ctx);

	return 0;
}

static ssize_t message_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct ascii_lcd_ctx *ctx = dev_get_drvdata(dev);

	return sprintf(buf, "%s\n", ctx->message);
}

static ssize_t message_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct ascii_lcd_ctx *ctx = dev_get_drvdata(dev);
	int err;

	err = ascii_lcd_display(ctx, buf, count);
	return err ?: count;
}

static DEVICE_ATTR_RW(message);

static int ascii_lcd_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	const struct ascii_lcd_config *cfg;
	struct ascii_lcd_ctx *ctx;
	struct resource *res;
	int err;

	match = of_match_device(ascii_lcd_matches, &pdev->dev);
	if (!match)
		return -ENODEV;

	cfg = match->data;
	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx) + cfg->num_chars,
			   GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ctx->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(ctx->base))
		return PTR_ERR(ctx->base);

	ctx->pdev = pdev;
	ctx->cfg = cfg;
	ctx->message = NULL;
	ctx->scroll_pos = 0;
	ctx->scroll_rate = HZ / 2;

	/* initialise a timer for scrolling the message */
	init_timer(&ctx->timer);
	ctx->timer.function = ascii_lcd_scroll;
	ctx->timer.data = (unsigned long)ctx;

	platform_set_drvdata(pdev, ctx);

	/* display a default message */
	err = ascii_lcd_display(ctx, "Linux " UTS_RELEASE "       ", -1);
	if (err)
		goto out_del_timer;

	err = device_create_file(&pdev->dev, &dev_attr_message);
	if (err)
		goto out_del_timer;

	return 0;
out_del_timer:
	del_timer_sync(&ctx->timer);
	return err;
}

static int ascii_lcd_remove(struct platform_device *pdev)
{
	struct ascii_lcd_ctx *ctx = platform_get_drvdata(pdev);

	device_remove_file(&pdev->dev, &dev_attr_message);
	del_timer_sync(&ctx->timer);
	return 0;
}

static struct platform_driver ascii_lcd_driver = {
	.driver = {
		.name		= "ascii-lcd",
		.of_match_table	= ascii_lcd_matches,
	},
	.probe	= ascii_lcd_probe,
	.remove	= ascii_lcd_remove,
};
module_platform_driver(ascii_lcd_driver);
