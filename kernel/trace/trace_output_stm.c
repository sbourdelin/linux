/*
 * Output interface from Ftrace to STM buffer
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

#include <linux/stm.h>

/* Offset above the start channel number */
#define STM_FTRACE_CHAN 0

static struct stm_ftrace *trace_output;

void trace_func_to_stm(unsigned long ip, unsigned long parent_ip)
{
	unsigned long ip_array[2] = {ip, parent_ip};

	if (trace_output)
		trace_output->write(&trace_output->data, (char *)ip_array,
				    sizeof(unsigned long) * 2, STM_FTRACE_CHAN);
}

void trace_add_output(struct stm_ftrace *stm)
{
	trace_output = stm;
}
EXPORT_SYMBOL_GPL(trace_add_output);

void trace_rm_output(void)
{
	trace_output = NULL;
}
EXPORT_SYMBOL_GPL(trace_rm_output);
