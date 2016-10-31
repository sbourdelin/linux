/*
 * Copyright (C) 2016-17 Synopsys, Inc. (www.synopsys.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __SOC_ARC_TIMERS_H
#define __SOC_ARC_TIMERS_H

#include <soc/arc/aux.h>

#define ARC_REG_TIMERS_BCR	0x75

struct bcr_timer {
#ifdef CONFIG_CPU_BIG_ENDIAN
	unsigned int pad2:15, rtsc:1, pad1:5, rtc:1, t1:1, t0:1, ver:8;
#else
	unsigned int ver:8, t0:1, t1:1, rtc:1, pad1:5, rtsc:1, pad2:15;
#endif
};

#endif
