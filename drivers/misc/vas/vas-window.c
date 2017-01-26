/*
 * Copyright 2016 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <asm/vas.h>
#include "vas-internal.h"

/*
 * Compute the paste address region for the window @window using the
 * ->win_base_addr and ->win_id_shift we got from device tree.
 */
void compute_paste_address(struct vas_window *window, uint64_t *addr, int *len)
{
	uint64_t base, shift;
	int winid;

	base = window->vinst->win_base_addr;
	shift = window->vinst->win_id_shift;
	winid = window->winid;

	*addr  = base + (winid << shift);
	*len = PAGE_SIZE;

	pr_debug("Txwin #%d: Paste addr 0x%llx\n", winid, *addr);
}

static inline void get_hvwc_mmio_bar(struct vas_window *window,
			uint64_t *start, int *len)
{
	uint64_t pbaddr;

	pbaddr = window->vinst->hvwc_bar_start;
	*start = pbaddr + window->winid * VAS_HVWC_SIZE;
	*len = VAS_HVWC_SIZE;
}

static inline void get_uwc_mmio_bar(struct vas_window *window,
			uint64_t *start, int *len)
{
	uint64_t pbaddr;

	pbaddr = window->vinst->uwc_bar_start;
	*start = pbaddr + window->winid * VAS_UWC_SIZE;
	*len = VAS_UWC_SIZE;
}

static void *map_mmio_region(char *name, uint64_t start, int len)
{
	void *map;

	if (!request_mem_region(start, len, name)) {
		pr_devel("%s(): request_mem_region(0x%llx, %d) failed\n",
				__func__, start, len);
		return NULL;
	}

	map = __ioremap(start, len, pgprot_val(pgprot_cached(__pgprot(0))));
	if (!map) {
		pr_devel("%s(): ioremap(0x%llx, %d) failed\n", __func__, start,
				len);
		return NULL;
	}

	return map;
}

/*
 * Unmap the MMIO regions for a window.
 */
void unmap_wc_mmio_bars(struct vas_window *window)
{
	int len;
	uint64_t busaddr_start;

	if (window->paste_kaddr) {
		iounmap(window->paste_kaddr);
		compute_paste_address(window, &busaddr_start, &len);
		release_mem_region((phys_addr_t)busaddr_start, len);
	}

	if (window->hvwc_map) {
		iounmap(window->hvwc_map);
		get_hvwc_mmio_bar(window, &busaddr_start, &len);
		release_mem_region((phys_addr_t)busaddr_start, len);
	}

	if (window->uwc_map) {
		iounmap(window->uwc_map);
		get_uwc_mmio_bar(window, &busaddr_start, &len);
		release_mem_region((phys_addr_t)busaddr_start, len);
	}
}

/*
 * Find the Hypervisor Window Context (HVWC) MMIO Base Address Region and the
 * OS/User Window Context (UWC) MMIO Base Address Region for the given window.
 * Map these bus addresses and save the mapped kernel addresses in @window.
 */
int map_wc_mmio_bars(struct vas_window *window)
{
	int len;
	uint64_t start;

	window->hvwc_map = window->uwc_map = NULL;

	get_hvwc_mmio_bar(window, &start, &len);
	window->hvwc_map = map_mmio_region("HVWCM_Window", start, len);

	get_uwc_mmio_bar(window, &start, &len);
	window->uwc_map = map_mmio_region("UWCM_Window", start, len);

	if (!window->hvwc_map || !window->uwc_map)
		return -1;

	return 0;
}

/* stub for now */
int vas_window_reset(struct vas_instance *vinst, int winid)
{
	return 0;
}
