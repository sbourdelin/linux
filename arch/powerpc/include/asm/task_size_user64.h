/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_TASK_SIZE_USER64_H
#define _ASM_POWERPC_TASK_SIZE_USER64_H

#ifdef CONFIG_PPC64
/*
 * 64-bit user address space can have multiple limits
 * For now supported values are:
 */
#define TASK_SIZE_64TB  (0x0000400000000000UL)
#define TASK_SIZE_128TB (0x0000800000000000UL)
#define TASK_SIZE_512TB (0x0002000000000000UL)
#define TASK_SIZE_1PB   (0x0004000000000000UL)
#define TASK_SIZE_2PB   (0x0008000000000000UL)
/*
 * With 52 bits in the address we can support
 * upto 4PB of range.
 */
#define TASK_SIZE_4PB   (0x0010000000000000UL)

/*
 * For now 512TB is only supported with book3s and 64K linux page size.
 */
#if defined(CONFIG_PPC_BOOK3S_64) && defined(CONFIG_PPC_64K_PAGES)
/*
 * Max value currently used:
 */
#define TASK_SIZE_USER64		TASK_SIZE_4PB
#define DEFAULT_MAP_WINDOW_USER64	TASK_SIZE_128TB
#define TASK_CONTEXT_SIZE		TASK_SIZE_512TB
#else
#define TASK_SIZE_USER64		TASK_SIZE_64TB
#define DEFAULT_MAP_WINDOW_USER64	TASK_SIZE_64TB
/*
 * We don't need to allocate extended context ids for 4K page size, because
 * we limit the max effective address on this config to 64TB.
 */
#define TASK_CONTEXT_SIZE		TASK_SIZE_64TB
#endif

#endif /* CONFIG_PPC64 */
#endif /* _ASM_POWERPC_TASK_SIZE_USER64_H */
