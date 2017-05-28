/*
 * Copyright (C) 2012-2017 ARM Limited or its affiliates.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _CC_LLI_DEFS_H_
#define _CC_LLI_DEFS_H_

#include <linux/types.h>

/* Max DLLI size
 *  AKA DX_DSCRPTR_QUEUE_WORD1_DIN_SIZE_BIT_SIZE
 */
#define DLLI_SIZE_BIT_SIZE	0x18

#define CC_MAX_MLLI_ENTRY_SIZE 0x10000

#define LLI_MAX_NUM_OF_DATA_ENTRIES 128
#define LLI_MAX_NUM_OF_ASSOC_DATA_ENTRIES 4
#define MLLI_TABLE_MIN_ALIGNMENT 4 /* 32 bit alignment */
#define MAX_NUM_OF_BUFFERS_IN_MLLI 4
#define MAX_NUM_OF_TOTAL_MLLI_ENTRIES (2 * LLI_MAX_NUM_OF_DATA_ENTRIES + \
				       LLI_MAX_NUM_OF_ASSOC_DATA_ENTRIES)

struct cc_lli_entry {
#ifndef __LITTLE_ENDIAN__
	u32 addr_lsb;
	u16 size;
	u16 addr_msb;
#else /* __BIG_ENDIAN__ */
	u16 addr_msb;
	u16 size;
	u32 addr_lsb;
#endif
} __packed;

/* Size of entry */
#define LLI_ENTRY_BYTE_SIZE sizeof(struct cc_lli_entry)

static inline void cc_lli_set_addr(u32 *lli_p, dma_addr_t addr)
{
	struct cc_lli_entry *entry = (struct cc_lli_entry *)lli_p;

	entry->addr_lsb = (addr & U32_MAX);
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	entry->addr_msb = (addr >> 16);
#endif /* CONFIG_ARCH_DMA_ADDR_T_64BIT */
}

static inline void cc_lli_set_size(u32 *lli_p, u32 size)
{
	struct cc_lli_entry *entry = (struct cc_lli_entry *)lli_p;

	entry->size = size;
}

#endif /*_CC_LLI_DEFS_H_*/
