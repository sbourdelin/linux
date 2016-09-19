/*
 * Driver for MMC and SSD cards for Cavium OCTEON SOCs.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2012-2016 Cavium Inc.
 */
#include <linux/init.h>
#include <linux/module.h>

#include <linux/mmc/octeon_mmc.h>

#define CVMX_MIO_BOOT_CTL      CVMX_ADD_IO_SEG(0x00011800000000D0ull)

/*
 * The functions below are used for the EMMC-17978 workaround.
 *
 * Due to an imperfection in the design of the MMC bus hardware,
 * the 2nd to last cache block of a DMA read must be locked into the L2 Cache.
 * Otherwise, data corruption may occur.
 */

static inline void *phys_to_ptr(u64 address)
{
	return (void *)(address | (1ull<<63)); /* XKPHYS */
}

/**
 * Lock a single line into L2. The line is zeroed before locking
 * to make sure no dram accesses are made.
 *
 * @addr   Physical address to lock
 */
static void l2c_lock_line(u64 addr)
{
	char *addr_ptr = phys_to_ptr(addr);
	asm volatile (
		"cache 31, %[line]"	/* Unlock the line */
		:: [line] "m" (*addr_ptr));
}

/**
 * Unlock a single line in the L2 cache.
 *
 * @addr	Physical address to unlock
 *
 * Return Zero on success
 */
static void l2c_unlock_line(u64 addr)
{
	char *addr_ptr = phys_to_ptr(addr);
	asm volatile (
		"cache 23, %[line]"	/* Unlock the line */
		:: [line] "m" (*addr_ptr));
}

/**
 * Locks a memory region in the L2 cache
 *
 * @start - start address to begin locking
 * @len - length in bytes to lock
 */
void l2c_lock_mem_region(u64 start, u64 len)
{
	u64 end;

	/* Round start/end to cache line boundaries */
	end = ALIGN(start + len - 1, CVMX_CACHE_LINE_SIZE);
	start = ALIGN(start, CVMX_CACHE_LINE_SIZE);

	while (start <= end) {
		l2c_lock_line(start);
		start += CVMX_CACHE_LINE_SIZE;
	}
	asm volatile("sync");
}

/**
 * Unlock a memory region in the L2 cache
 *
 * @start - start address to unlock
 * @len - length to unlock in bytes
 */
void l2c_unlock_mem_region(u64 start, u64 len)
{
	u64 end;

	/* Round start/end to cache line boundaries */
	end = ALIGN(start + len - 1, CVMX_CACHE_LINE_SIZE);
	start = ALIGN(start, CVMX_CACHE_LINE_SIZE);

	while (start <= end) {
		l2c_unlock_line(start);
		start += CVMX_CACHE_LINE_SIZE;
	}
}

void octeon_mmc_acquire_bus(struct octeon_mmc_host *host)
{
	if (!host->has_ciu3) {
		/* Switch the MMC controller onto the bus. */
		down(&octeon_bootbus_sem);
		writeq(0, (void __iomem *)CVMX_MIO_BOOT_CTL);
	} else {
		down(&host->mmc_serializer);
	}
}

void octeon_mmc_release_bus(struct octeon_mmc_host *host)
{
	if (!host->has_ciu3)
		up(&octeon_bootbus_sem);
	else
		up(&host->mmc_serializer);
}
