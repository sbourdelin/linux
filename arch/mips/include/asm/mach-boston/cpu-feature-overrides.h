/*
 * Copyright (C) 2015 Imagination Technologies
 * Author: Paul Burton <paul.burton@imgtec.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef __ASM_MACH_BOSTON_CPU_FEATURE_OVERRIDES_H__
#define __ASM_MACH_BOSTON_CPU_FEATURE_OVERRIDES_H__

#define cpu_has_4kex			1
#define cpu_has_4k_cache		1
#define cpu_has_clo_clz			1
#define cpu_has_counter			1
#define cpu_has_divec			1
#define cpu_has_llsc			1
#define cpu_has_mcheck			1
#define cpu_has_nofpuex			0
#define cpu_has_tlb			1
#define cpu_has_vce			0
#define cpu_icache_snoops_remote_store	1

#endif /* __ASM_MACH_BOSTON_CPU_FEATURE_OVERRIDES_H__ */
