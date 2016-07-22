/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Copyright (C) 2016 ARM Limited
 */
#include <linux/errno.h>
#include <linux/linkage.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/printk.h>
#include "rockchip_sip.h"

typedef unsigned long (psci_fn)(unsigned long, unsigned long,
				unsigned long, unsigned long);
asmlinkage psci_fn __invoke_psci_fn_smc;

#define CONFIG_DRAM_INIT	0x00
#define CONFIG_DRAM_SET_RATE	0x01
#define CONFIG_DRAM_ROUND_RATE	0x02
#define CONFIG_DRAM_SET_AT_SR	0x03
#define CONFIG_DRAM_GET_BW	0x04
#define CONFIG_DRAM_GET_RATE	0x05
#define CONFIG_DRAM_CLR_IRQ	0x06
#define CONFIG_DRAM_SET_PARAM   0x07

uint64_t sip_smc_ddr_init(void)
{
	return __invoke_psci_fn_smc(SIP_DDR_FREQ, 0,
				    0, CONFIG_DRAM_INIT);
}

uint64_t sip_smc_set_ddr_param(uint64_t param)
{
	return __invoke_psci_fn_smc(SIP_DDR_FREQ, param,
				    0, CONFIG_DRAM_SET_PARAM);
}

uint64_t sip_smc_set_ddr_rate(uint64_t rate)
{
	return __invoke_psci_fn_smc(SIP_DDR_FREQ, rate, 0,
				    CONFIG_DRAM_SET_RATE);
}

uint64_t sip_smc_get_ddr_rate(void)
{
	return __invoke_psci_fn_smc(SIP_DDR_FREQ, 0, 0, CONFIG_DRAM_GET_RATE);
}

uint64_t sip_smc_clr_ddr_irq(void)
{
	return __invoke_psci_fn_smc(SIP_DDR_FREQ, 0, 0, CONFIG_DRAM_CLR_IRQ);
}

uint64_t sip_smc_get_call_count(void)
{
	return __invoke_psci_fn_smc(SIP_SVC_CALL_COUNT, 0, 0, 0);
}
