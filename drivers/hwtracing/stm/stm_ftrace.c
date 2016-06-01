/*
 * Simple kernel driver to link kernel Ftrace and an STM device
 * Copyright (c) 2016, Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/console.h>
#include <linux/stm.h>

static struct stm_source_data stm_ftrace_data = {
	.name		= "stm_ftrace",
	.nr_chans	= 1,
};

/**
 * stm_ftrace_write() - write data to STM via 'stm_ftrace' source
 * @buf:	buffer containing the data packet
 * @len:	length of the data packet
 * @chan:	offset above the start channel number allocated to 'stm_ftrace'
 */
void notrace stm_ftrace_write(const char *buf, unsigned int len,
			      unsigned int chan)
{
	stm_source_write(&stm_ftrace_data, chan, buf, len);
}
EXPORT_SYMBOL_GPL(stm_ftrace_write);

static int __init stm_ftrace_init(void)
{
	return stm_source_register_device(NULL, &stm_ftrace_data);
}

static void __exit stm_ftrace_exit(void)
{
	stm_source_unregister_device(&stm_ftrace_data);
}

module_init(stm_ftrace_init);
module_exit(stm_ftrace_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("stm_ftrace driver");
MODULE_AUTHOR("Chunyan Zhang <zhang.chunyan@linaro.org>");
