/*
 * This file is part of STM32 DFSDM mfd driver
 *
 * Copyright (C) 2017, STMicroelectronics - All Rights Reserved
 * Author(s): Arnaud Pouliquen <arnaud.pouliquen@st.com>.
 *
 * License terms: GPL V2.0.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 */
#ifndef MDF_STM32_DFSDM__H
#define MDF_STM32_DFSDM__H

#include <linux/bitfield.h>

/*
 * STM32 DFSDM - global register map
 * ________________________________________________________
 * | Offset |                 Registers block             |
 * --------------------------------------------------------
 * | 0x000  |      CHANNEL 0 + COMMON CHANNEL FIELDS      |
 * --------------------------------------------------------
 * | 0x020  |                CHANNEL 1                    |
 * --------------------------------------------------------
 * | ...    |                .....                        |
 * --------------------------------------------------------
 * | 0x0E0  |                CHANNEL 7                    |
 * --------------------------------------------------------
 * | 0x100  |      FILTER  0 + COMMON  FILTER FIELDs      |
 * --------------------------------------------------------
 * | 0x200  |                FILTER  1                    |
 * --------------------------------------------------------
 * | 0x300  |                FILTER  2                    |
 * --------------------------------------------------------
 * | 0x400  |                FILTER  3                    |
 * --------------------------------------------------------
 */

/*
 * Channels register definitions
 */
#define DFSDM_CHCFGR1(y)  ((y) * 0x20 + 0x00)
#define DFSDM_CHCFGR2(y)  ((y) * 0x20 + 0x04)
#define DFSDM_AWSCDR(y)   ((y) * 0x20 + 0x08)
#define DFSDM_CHWDATR(y)  ((y) * 0x20 + 0x0C)
#define DFSDM_CHDATINR(y) ((y) * 0x20 + 0x10)

/* CHCFGR1: Channel configuration register 1 */
#define DFSDM_CHCFGR1_SITP_MASK     GENMASK(1, 0)
#define DFSDM_CHCFGR1_SITP(v)       FIELD_PREP(DFSDM_CHCFGR1_SITP_MASK, v)
#define DFSDM_CHCFGR1_SPICKSEL_MASK GENMASK(3, 2)
#define DFSDM_CHCFGR1_SPICKSEL(v)   FIELD_PREP(DFSDM_CHCFGR1_SPICKSEL_MASK, v)
#define DFSDM_CHCFGR1_SCDEN_MASK    BIT(5)
#define DFSDM_CHCFGR1_SCDEN(v)      FIELD_PREP(DFSDM_CHCFGR1_SCDEN_MASK, v)
#define DFSDM_CHCFGR1_CKABEN_MASK   BIT(6)
#define DFSDM_CHCFGR1_CKABEN(v)     FIELD_PREP(DFSDM_CHCFGR1_CKABEN_MASK, v)
#define DFSDM_CHCFGR1_CHEN_MASK     BIT(7)
#define DFSDM_CHCFGR1_CHEN(v)       FIELD_PREP(DFSDM_CHCFGR1_CHEN_MASK, v)
#define DFSDM_CHCFGR1_CHINSEL_MASK  BIT(8)
#define DFSDM_CHCFGR1_CHINSEL(v)    FIELD_PREP(DFSDM_CHCFGR1_CHINSEL_MASK, v)
#define DFSDM_CHCFGR1_DATMPX_MASK   GENMASK(13, 12)
#define DFSDM_CHCFGR1_DATMPX(v)     FIELD_PREP(DFSDM_CHCFGR1_DATMPX_MASK, v)
#define DFSDM_CHCFGR1_DATPACK_MASK  GENMASK(15, 14)
#define DFSDM_CHCFGR1_DATPACK(v)    FIELD_PREP(DFSDM_CHCFGR1_DATPACK_MASK, v)
#define DFSDM_CHCFGR1_CKOUTDIV_MASK GENMASK(23, 16)
#define DFSDM_CHCFGR1_CKOUTDIV(v)   FIELD_PREP(DFSDM_CHCFGR1_CKOUTDIV_MASK, v)
#define DFSDM_CHCFGR1_CKOUTSRC_MASK BIT(30)
#define DFSDM_CHCFGR1_CKOUTSRC(v)   FIELD_PREP(DFSDM_CHCFGR1_CKOUTSRC_MASK, v)
#define DFSDM_CHCFGR1_DFSDMEN_MASK  BIT(31)
#define DFSDM_CHCFGR1_DFSDMEN(v)    FIELD_PREP(DFSDM_CHCFGR1_DFSDMEN_MASK, v)

/*
 * Filters register definitions
 */
#define DFSDM_FILTER_BASE_ADR		0x100
#define DFSDM_FILTER_REG_MASK		0x7F
#define DFSDM_FILTER_X_BASE_ADR(x)	((x) * 0x80 + DFSDM_FILTER_BASE_ADR)

#define DFSDM_CR1(x)     (DFSDM_FILTER_X_BASE_ADR(x)  + 0x00)
#define DFSDM_CR2(x)     (DFSDM_FILTER_X_BASE_ADR(x)  + 0x04)
#define DFSDM_ISR(x)     (DFSDM_FILTER_X_BASE_ADR(x)  + 0x08)
#define DFSDM_ICR(x)     (DFSDM_FILTER_X_BASE_ADR(x)  + 0x0C)
#define DFSDM_JCHGR(x)   (DFSDM_FILTER_X_BASE_ADR(x)  + 0x10)
#define DFSDM_FCR(x)     (DFSDM_FILTER_X_BASE_ADR(x)  + 0x14)
#define DFSDM_JDATAR(x)  (DFSDM_FILTER_X_BASE_ADR(x)  + 0x18)
#define DFSDM_RDATAR(x)  (DFSDM_FILTER_X_BASE_ADR(x)  + 0x1C)
#define DFSDM_AWHTR(x)   (DFSDM_FILTER_X_BASE_ADR(x)  + 0x20)
#define DFSDM_AWLTR(x)   (DFSDM_FILTER_X_BASE_ADR(x)  + 0x24)
#define DFSDM_AWSR(x)    (DFSDM_FILTER_X_BASE_ADR(x)  + 0x28)
#define DFSDM_AWCFR(x)   (DFSDM_FILTER_X_BASE_ADR(x)  + 0x2C)
#define DFSDM_EXMAX(x)   (DFSDM_FILTER_X_BASE_ADR(x)  + 0x30)
#define DFSDM_EXMIN(x)   (DFSDM_FILTER_X_BASE_ADR(x)  + 0x34)
#define DFSDM_CNVTIMR(x) (DFSDM_FILTER_X_BASE_ADR(x)  + 0x38)

/**
 * struct stm32_dfsdm_filter - structure relative to stm32 FDSDM filter
 * TODO: complete structure.
 * @id:		filetr ID,
 */
struct stm32_dfsdm_filter {
	unsigned int id;
};

/**
 * struct stm32_dfsdm_channel - structure relative to stm32 FDSDM channel
 * TODO: complete structure.
 * @id:		filetr ID,
 */
struct stm32_dfsdm_channel {
	unsigned int id;
};

/**
 * struct stm32_dfsdm - stm32 FDSDM driver common data (for all instances)
 * @base:	control registers base cpu addr
 * @phys_base:	DFSDM IP register physical address.
 * @fl_list:	filter resources list
 * @num_fl:	number of filter resources available
 * @ch_list:	channel resources list
 * @num_chs:	number of channel resources available
 */
struct stm32_dfsdm {
	void __iomem	*base;
	phys_addr_t	phys_base;
	struct stm32_dfsdm_filter *fl_list;
	int num_fls;
	struct stm32_dfsdm_channel *ch_list;
	int num_chs;
};

int stm32_dfsdm_start_dfsdm(struct stm32_dfsdm *dfsdm);
int stm32_dfsdm_stop_dfsdm(struct stm32_dfsdm *dfsdm);

#endif
