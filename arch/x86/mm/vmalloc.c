// SPDX-License-Identifier: GPL-2.0
/*
 * vmalloc.c: x86 arch version of vmalloc.c
 *
 * (C) Copyright 2018 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <linux/mm.h>
#include <linux/set_memory.h>
#include <linux/vmalloc.h>

static void set_area_direct_np(struct vm_struct *area)
{
	int i;

	for (i = 0; i < area->nr_pages; i++)
		set_pages_np_noflush(area->pages[i], 1);
}

static void set_area_direct_prw(struct vm_struct *area)
{
	int i;

	for (i = 0; i < area->nr_pages; i++)
		set_pages_p_noflush(area->pages[i], 1);
}

void arch_vunmap(struct vm_struct *area, int deallocate_pages)
{
	int immediate = area->flags & VM_IMMEDIATE_UNMAP;
	int special = area->flags & VM_HAS_SPECIAL_PERMS;

	/* Unmap from vmalloc area */
	remove_vm_area(area->addr);

	/* If no need to reset directmap perms, just check if need to flush */
	if (!(deallocate_pages || special)) {
		if (immediate)
			vm_unmap_aliases();
		return;
	}

	/* From here we need to make sure to reset the direct map perms */

	/*
	 * If the area being freed does not have any extra capabilities, we can
	 * just reset the directmap to RW before freeing.
	 */
	if (!immediate) {
		set_area_direct_prw(area);
		vm_unmap_aliases();
		return;
	}

	/*
	 * If the vm being freed has security sensitive capabilities such as
	 * executable we need to make sure there is no W window on the directmap
	 * before removing the X in the TLB. So we set not present first so we
	 * can flush without any other CPU picking up the mapping. Then we reset
	 * RW+P without a flush, since NP prevented it from being cached by
	 * other cpus.
	 */
	set_area_direct_np(area);
	vm_unmap_aliases();
	set_area_direct_prw(area);
}
