/*
 * pwrseq_generic.c	Generic power sequence handling
 *
 * Copyright (C) 2016 Freescale Semiconductor, Inc.
 * Author: Peter Chen <peter.chen@nxp.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>

#include <linux/power/pwrseq.h>

struct pwrseq_generic {
	struct pwrseq pwrseq;
	struct gpio_desc *gpiod_reset;
	struct clk *clks[PWRSEQ_MAX_CLKS];
};

#define to_generic_pwrseq(p) container_of(p, struct pwrseq_generic, pwrseq)

static void pwrseq_generic_free(struct pwrseq *pwrseq)
{
	struct pwrseq_generic *pwrseq_gen = to_generic_pwrseq(pwrseq);

	kfree(pwrseq_gen);
}

static void pwrseq_generic_put(struct pwrseq *pwrseq)
{
	struct pwrseq_generic *pwrseq_gen = to_generic_pwrseq(pwrseq);
	int clk;

	if (pwrseq_gen->gpiod_reset)
		gpiod_put(pwrseq_gen->gpiod_reset);

	for (clk = 0; clk < PWRSEQ_MAX_CLKS; clk++)
		clk_put(pwrseq_gen->clks[clk]);
}

static void pwrseq_generic_off(struct pwrseq *pwrseq)
{
	struct pwrseq_generic *pwrseq_gen = to_generic_pwrseq(pwrseq);
	int clk;

	for (clk = PWRSEQ_MAX_CLKS - 1; clk >= 0; clk--)
		clk_disable_unprepare(pwrseq_gen->clks[clk]);
}

static int pwrseq_generic_on(struct device_node *np, struct pwrseq *pwrseq)
{
	struct pwrseq_generic *pwrseq_gen = to_generic_pwrseq(pwrseq);
	int clk, ret = 0;
	struct gpio_desc *gpiod_reset = pwrseq_gen->gpiod_reset;

	for (clk = 0; clk < PWRSEQ_MAX_CLKS && pwrseq_gen->clks[clk]; clk++) {
		ret = clk_prepare_enable(pwrseq_gen->clks[clk]);
		if (ret) {
			pr_err("Can't enable clock on %s: %d\n",
				np->full_name, ret);
			goto err_disable_clks;
		}
	}

	if (gpiod_reset) {
		u32 duration_us = 50;

		of_property_read_u32(np, "reset-duration-us",
				&duration_us);
		if (duration_us <= 10)
			udelay(10);
		else
			usleep_range(duration_us, duration_us + 100);
		gpiod_set_value(gpiod_reset, 0);
	}

	return ret;

err_disable_clks:
	while (--clk >= 0)
		clk_disable_unprepare(pwrseq_gen->clks[clk]);

	return ret;
}

static int pwrseq_generic_get(struct device_node *np, struct pwrseq *pwrseq)
{
	struct pwrseq_generic *pwrseq_gen = to_generic_pwrseq(pwrseq);
	enum of_gpio_flags flags;
	int reset_gpio, clk, ret = 0;

	for (clk = 0; clk < PWRSEQ_MAX_CLKS; clk++) {
		pwrseq_gen->clks[clk] = of_clk_get(np, clk);
		if (IS_ERR(pwrseq_gen->clks[clk])) {
			ret = PTR_ERR(pwrseq_gen->clks[clk]);
			if (ret == -EPROBE_DEFER)
				goto err_put_clks;
			pwrseq_gen->clks[clk] = NULL;
			break;
		}
	}

	reset_gpio = of_get_named_gpio_flags(np, "reset-gpios", 0, &flags);
	if (gpio_is_valid(reset_gpio)) {
		unsigned long gpio_flags;

		if (flags & OF_GPIO_ACTIVE_LOW)
			gpio_flags = GPIOF_ACTIVE_LOW | GPIOF_OUT_INIT_LOW;
		else
			gpio_flags = GPIOF_OUT_INIT_HIGH;

		ret = gpio_request_one(reset_gpio, gpio_flags,
				"pwrseq-reset-gpios");
		if (ret)
			goto err_put_clks;

		pwrseq_gen->gpiod_reset = gpio_to_desc(reset_gpio);
	} else {
		if (reset_gpio == -ENOENT)
			return 0;

		ret = reset_gpio;
		pr_err("Failed to get reset gpio on %s, err = %d\n",
				np->full_name, reset_gpio);
		goto err_put_clks;
	}

	return ret;

err_put_clks:
	while (--clk >= 0)
		clk_put(pwrseq_gen->clks[clk]);
	return ret;
}

struct pwrseq *pwrseq_alloc_generic(void)
{
	struct pwrseq_generic *pwrseq_gen;

	pwrseq_gen = kzalloc(sizeof(*pwrseq_gen), GFP_KERNEL);
	if (!pwrseq_gen)
		return ERR_PTR(-ENOMEM);

	pwrseq_gen->pwrseq.get = pwrseq_generic_get;
	pwrseq_gen->pwrseq.on = pwrseq_generic_on;
	pwrseq_gen->pwrseq.off = pwrseq_generic_off;
	pwrseq_gen->pwrseq.put = pwrseq_generic_put;
	pwrseq_gen->pwrseq.free = pwrseq_generic_free;

	return &pwrseq_gen->pwrseq;
}
EXPORT_SYMBOL_GPL(pwrseq_alloc_generic);
