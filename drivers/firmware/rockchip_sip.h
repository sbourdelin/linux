/* Copyright (c) 2010-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef __SIP_INT_H
#define __SIP_INT_H

/* SMC function IDs for SiP Service queries */
#define SIP_SVC_CALL_COUNT		0x8200ff00
#define SIP_SVC_UID			0x8200ff01
#define SIP_SVC_VERSION			0x8200ff03
#define SIP_DDR_FREQ			0xC2000008

#if IS_ENABLED(CONFIG_ROCKCHIP_SIP)
uint64_t sip_smc_set_ddr_rate(uint64_t rate);
uint64_t sip_smc_get_ddr_rate(void);
uint64_t sip_smc_clr_ddr_irq(void);
uint64_t sip_smc_get_call_count(void);
uint64_t sip_smc_ddr_init(void);
uint64_t sip_smc_set_ddr_param(uint64_t param);
#else
static inline uint64_t sip_smc_set_ddr_rate(uint64_t rate)
{
	return 0;
}

static inline uint64_t sip_smc_get_ddr_rate(void)
{
	return 0;
}

static inline uint64_t sip_smc_clr_ddr_irq(void)
{
	return 0;
}

static inline uint64_t sip_smc_get_call_count(void)
{
	return 0;
}

static inline uint64_t sip_smc_ddr_init(void)
{
	return 0;
}

static inline uint64_t sip_smc_set_ddr_param(uint64_t param)
{
	return 0;
}
#endif
#endif
