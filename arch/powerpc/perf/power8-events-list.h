/*
 * Performance counter support for POWER8 processors.
 *
 * Copyright 2014 Sukadev Bhattiprolu, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

/*
 * Power8 event codes.
 */
EVENT(PM_CYC,					0x0001e)
EVENT(PM_GCT_NOSLOT_CYC,			0x100f8)
EVENT(PM_CMPLU_STALL,				0x4000a)
EVENT(PM_INST_CMPL,				0x00002)
EVENT(PM_BRU_FIN,				0x10068)
EVENT(PM_BR_MPRED_CMPL,				0x400f6)
EVENT(PM_LD_REF_L1,				0x100ee)
EVENT(PM_LD_MISS_L1,				0x3e054)
EVENT(PM_ST_MISS_L1,				0x300f0)
EVENT(PM_L1_PREF,				0x0d8b8)
EVENT(PM_INST_FROM_L1,				0x04080)
EVENT(PM_L1_ICACHE_MISS,			0x200fd)
EVENT(PM_L1_DEMAND_WRITE,			0x0408c)
EVENT(PM_IC_PREF_WRITE,				0x0408e)
EVENT(PM_DATA_FROM_L3,				0x4c042)
EVENT(PM_DATA_FROM_L3MISS,			0x300fe)
EVENT(PM_L2_ST,					0x17080)
EVENT(PM_L2_ST_MISS,				0x17082)
EVENT(PM_L3_PREF_ALL,				0x4e052)
EVENT(PM_DTLB_MISS,				0x300fc)
EVENT(PM_ITLB_MISS,				0x400fc)
