// SPDX-License-Identifier: GPL-2.0
/*
 * Mainly by David Woodhouse, somewhat modified by Jordan Crouse
 *
 * Copyright © 2006-2007  Red Hat, Inc.
 * Copyright © 2006-2007  Advanced Micro Devices, Inc.
 * Copyright © 2009       VIA Technology, Inc.
 * Copyright (c) 2010  Andres Salomon <dilinger@queued.net>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cs5535.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <asm/olpc.h>

#include "olpc_dcon.h"

static int dcon_init_xo_1(struct dcon_priv *dcon)
{
	unsigned char lob;
	int ret, i;
	struct dcon_gpio *pin;
	unsigned long flags = GPIOD_ASIS;

	struct dcon_gpio gpios[] = {
		{ .ptr = &dcon_stat0, .name = "dcon_stat0", .flags = flags },
		{ .ptr = &dcon_stat1, .name = "dcon_stat1", .flags = flags },
		{ .ptr = &dcon_irq, .name = "dcon_irq", .flags = flags },
		{ .ptr = &dcon_load, .name = "dcon_load", .flags = flags },
		{ .ptr = &dcon_blank, .name = "dcon_blank", .flags = flags },
	};

	for (i = 0; i < ARRAY_SIZE(gpios); i++) {
		pin = &gpios[i];
		*pin->ptr = devm_gpiod_get(&dcon->bl_dev->dev, pin->name,
					   pin->flags);
		if (IS_ERR(*pin->ptr)) {
			ret = PTR_ERR(*pin->ptr);
			dev_err(&dcon->bl_dev->dev,
				"failed to request %s GPIO: %d\n",
				pin->name, ret);
			return ret;
		}
	}

	/* Turn off the event enable for GPIO7 just to be safe */
	cs5535_gpio_clear(OLPC_GPIO_DCON_IRQ, GPIO_EVENTS_ENABLE);

	/*
	 * Determine the current state by reading the GPIO bit; earlier
	 * stages of the boot process have established the state.
	 *
	 * Note that we read GPIO_OUTPUT_VAL rather than GPIO_READ_BACK here;
	 * this is because OFW will disable input for the pin and set a value..
	 * READ_BACK will only contain a valid value if input is enabled and
	 * then a value is set.  So, future readings of the pin can use
	 * READ_BACK, but the first one cannot.  Awesome, huh?
	 */
	dcon->curr_src = cs5535_gpio_isset(OLPC_GPIO_DCON_LOAD, GPIO_OUTPUT_VAL)
		? DCON_SOURCE_CPU
		: DCON_SOURCE_DCON;
	dcon->pending_src = dcon->curr_src;

	/* Set the directions for the GPIO pins */
	gpiod_direction_input(dcon_stat0);
	gpiod_direction_input(dcon_stat1);
	gpiod_direction_input(dcon_irq);
	gpiod_direction_input(dcon_blank);
	gpiod_direction_output(dcon_load, dcon->curr_src == DCON_SOURCE_CPU);

	/* Set up the interrupt mappings */

	/* Set the IRQ to pair 2 */
	cs5535_gpio_setup_event(OLPC_GPIO_DCON_IRQ, 2, 0);

	/* Enable group 2 to trigger the DCON interrupt */
	cs5535_gpio_set_irq(2, DCON_IRQ);

	/* Select edge level for interrupt (in PIC) */
	lob = inb(0x4d0);
	lob &= ~(1 << DCON_IRQ);
	outb(lob, 0x4d0);

	/* Register the interrupt handler */
	if (request_irq(DCON_IRQ, &dcon_interrupt, 0, "DCON", dcon)) {
		pr_err("failed to request DCON's irq\n");
		goto err_req_irq;
	}

	/* Clear INV_EN for GPIO7 (DCONIRQ) */
	cs5535_gpio_clear(OLPC_GPIO_DCON_IRQ, GPIO_INPUT_INVERT);

	/* Enable filter for GPIO12 (DCONBLANK) */
	cs5535_gpio_set(OLPC_GPIO_DCON_BLANK, GPIO_INPUT_FILTER);

	/* Disable filter for GPIO7 */
	cs5535_gpio_clear(OLPC_GPIO_DCON_IRQ, GPIO_INPUT_FILTER);

	/* Disable event counter for GPIO7 (DCONIRQ) and GPIO12 (DCONBLANK) */
	cs5535_gpio_clear(OLPC_GPIO_DCON_IRQ, GPIO_INPUT_EVENT_COUNT);
	cs5535_gpio_clear(OLPC_GPIO_DCON_BLANK, GPIO_INPUT_EVENT_COUNT);

	/* Add GPIO12 to the Filter Event Pair #7 */
	cs5535_gpio_set(OLPC_GPIO_DCON_BLANK, GPIO_FE7_SEL);

	/* Turn off negative Edge Enable for GPIO12 */
	cs5535_gpio_clear(OLPC_GPIO_DCON_BLANK, GPIO_NEGATIVE_EDGE_EN);

	/* Enable negative Edge Enable for GPIO7 */
	cs5535_gpio_set(OLPC_GPIO_DCON_IRQ, GPIO_NEGATIVE_EDGE_EN);

	/* Zero the filter amount for Filter Event Pair #7 */
	cs5535_gpio_set(0, GPIO_FLTR7_AMOUNT);

	/* Clear the negative edge status for GPIO7 and GPIO12 */
	cs5535_gpio_set(OLPC_GPIO_DCON_IRQ, GPIO_NEGATIVE_EDGE_STS);
	cs5535_gpio_set(OLPC_GPIO_DCON_BLANK, GPIO_NEGATIVE_EDGE_STS);

	/* FIXME:  Clear the positive status as well, just to be sure */
	cs5535_gpio_set(OLPC_GPIO_DCON_IRQ, GPIO_POSITIVE_EDGE_STS);
	cs5535_gpio_set(OLPC_GPIO_DCON_BLANK, GPIO_POSITIVE_EDGE_STS);

	/* Enable events for GPIO7 (DCONIRQ) and GPIO12 (DCONBLANK) */
	cs5535_gpio_set(OLPC_GPIO_DCON_IRQ, GPIO_EVENTS_ENABLE);
	cs5535_gpio_set(OLPC_GPIO_DCON_BLANK, GPIO_EVENTS_ENABLE);

	return 0;
}

static void dcon_wiggle_xo_1(void)
{
	int x;

	/*
	 * According to HiMax, when powering the DCON up we should hold
	 * SMB_DATA high for 8 SMB_CLK cycles.  This will force the DCON
	 * state machine to reset to a (sane) initial state.  Mitch Bradley
	 * did some testing and discovered that holding for 16 SMB_CLK cycles
	 * worked a lot more reliably, so that's what we do here.
	 *
	 * According to the cs5536 spec, to set GPIO14 to SMB_CLK we must
	 * simultaneously set AUX1 IN/OUT to GPIO14; ditto for SMB_DATA and
	 * GPIO15.
	 */
	cs5535_gpio_set(OLPC_GPIO_SMB_CLK, GPIO_OUTPUT_VAL);
	cs5535_gpio_set(OLPC_GPIO_SMB_DATA, GPIO_OUTPUT_VAL);
	cs5535_gpio_set(OLPC_GPIO_SMB_CLK, GPIO_OUTPUT_ENABLE);
	cs5535_gpio_set(OLPC_GPIO_SMB_DATA, GPIO_OUTPUT_ENABLE);
	cs5535_gpio_clear(OLPC_GPIO_SMB_CLK, GPIO_OUTPUT_AUX1);
	cs5535_gpio_clear(OLPC_GPIO_SMB_DATA, GPIO_OUTPUT_AUX1);
	cs5535_gpio_clear(OLPC_GPIO_SMB_CLK, GPIO_OUTPUT_AUX2);
	cs5535_gpio_clear(OLPC_GPIO_SMB_DATA, GPIO_OUTPUT_AUX2);
	cs5535_gpio_clear(OLPC_GPIO_SMB_CLK, GPIO_INPUT_AUX1);
	cs5535_gpio_clear(OLPC_GPIO_SMB_DATA, GPIO_INPUT_AUX1);

	for (x = 0; x < 16; x++) {
		udelay(5);
		cs5535_gpio_clear(OLPC_GPIO_SMB_CLK, GPIO_OUTPUT_VAL);
		udelay(5);
		cs5535_gpio_set(OLPC_GPIO_SMB_CLK, GPIO_OUTPUT_VAL);
	}
	udelay(5);
	cs5535_gpio_set(OLPC_GPIO_SMB_CLK, GPIO_OUTPUT_AUX1);
	cs5535_gpio_set(OLPC_GPIO_SMB_DATA, GPIO_OUTPUT_AUX1);
	cs5535_gpio_set(OLPC_GPIO_SMB_CLK, GPIO_INPUT_AUX1);
	cs5535_gpio_set(OLPC_GPIO_SMB_DATA, GPIO_INPUT_AUX1);
}

static void dcon_set_dconload_1(int val)
{
	gpiod_set_value(dcon_load, val);
}

static int dcon_read_status_xo_1(u8 *status)
{
	*status = gpiod_get_value(dcon_stat0);
	*status |= gpiod_get_value(dcon_stat1) << 1;

	/* Clear the negative edge status for GPIO7 */
	cs5535_gpio_set(OLPC_GPIO_DCON_IRQ, GPIO_NEGATIVE_EDGE_STS);

	return 0;
}

struct dcon_platform_data dcon_pdata_xo_1 = {
	.init = dcon_init_xo_1,
	.bus_stabilize_wiggle = dcon_wiggle_xo_1,
	.set_dconload = dcon_set_dconload_1,
	.read_status = dcon_read_status_xo_1,
};
