// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains core KHWASAN code.
 *
 * Copyright (c) 2018 Google, Inc.
 * Author: Andrey Konovalov <andreyknvl@google.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#define DISABLE_BRANCH_PROFILING

#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/kasan.h>
#include <linux/kernel.h>
#include <linux/kmemleak.h>
#include <linux/linkage.h>
#include <linux/memblock.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/stacktrace.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/bug.h>

#include "kasan.h"
#include "../slab.h"

static DEFINE_PER_CPU(u32, prng_state);

void khwasan_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		per_cpu(prng_state, cpu) = get_random_u32();
}

/*
 * If a preemption happens between this_cpu_read and this_cpu_write, the only
 * side effect is that we'll give a few allocated in different contexts objects
 * the same tag. Since KHWASAN is meant to be used a probabilistic bug-detection
 * debug feature, this doesn’t have significant negative impact.
 *
 * Ideally the tags use strong randomness to prevent any attempts to predict
 * them during explicit exploit attempts. But strong randomness is expensive,
 * and we did an intentional trade-off to use a PRNG. This non-atomic RMW
 * sequence has in fact positive effect, since interrupts that randomly skew
 * PRNG at unpredictable points do only good.
 */
u8 random_tag(void)
{
	u32 state = this_cpu_read(prng_state);

	state = 1664525 * state + 1013904223;
	this_cpu_write(prng_state, state);

	return (u8)(state % (KHWASAN_TAG_MAX + 1));
}

void *khwasan_reset_tag(const void *addr)
{
	return reset_tag(addr);
}

void *khwasan_preset_slub_tag(struct kmem_cache *cache, const void *addr)
{
	/*
	 * Since it's desirable to only call object contructors ones during
	 * slab allocation, we preassign tags to all such objects.
	 * Also preassign tags for SLAB_TYPESAFE_BY_RCU slabs to avoid
	 * use-after-free reports.
	 */
	if (cache->ctor || cache->flags & SLAB_TYPESAFE_BY_RCU)
		return set_tag(addr, random_tag());
	return (void *)addr;
}

void *khwasan_preset_slab_tag(struct kmem_cache *cache, unsigned int idx,
				const void *addr)
{
	/*
	 * See comment in khwasan_preset_slub_tag.
	 * For SLAB allocator we can't preassign tags randomly since the
	 * freelist is stored as an array of indexes instead of a linked
	 * list. Assign tags based on objects indexes, so that objects that
	 * are next to each other get different tags.
	 */
	if (cache->ctor || cache->flags & SLAB_TYPESAFE_BY_RCU)
		return set_tag(addr, (u8)idx);
	return (void *)addr;
}

void check_memory_region(unsigned long addr, size_t size, bool write,
				unsigned long ret_ip)
{
	u8 tag;
	u8 *shadow_first, *shadow_last, *shadow;
	void *untagged_addr;

	tag = get_tag((const void *)addr);

	/* Ignore accesses for pointers tagged with 0xff (native kernel
	 * pointer tag) to suppress false positives caused by kmap.
	 *
	 * Some kernel code was written to account for archs that don't keep
	 * high memory mapped all the time, but rather map and unmap particular
	 * pages when needed. Instead of storing a pointer to the kernel memory,
	 * this code saves the address of the page structure and offset within
	 * that page for later use. Those pages are then mapped and unmapped
	 * with kmap/kunmap when necessary and virt_to_page is used to get the
	 * virtual address of the page. For arm64 (that keeps the high memory
	 * mapped all the time), kmap is turned into a page_address call.

	 * The issue is that with use of the page_address + virt_to_page
	 * sequence the top byte value of the original pointer gets lost (gets
	 * set to KHWASAN_TAG_KERNEL (0xFF).
	 */
	if (tag == KHWASAN_TAG_KERNEL)
		return;

	untagged_addr = reset_tag((const void *)addr);
	shadow_first = kasan_mem_to_shadow(untagged_addr);
	shadow_last = kasan_mem_to_shadow(untagged_addr + size - 1);

	for (shadow = shadow_first; shadow <= shadow_last; shadow++) {
		if (*shadow != tag) {
			kasan_report(addr, size, write, ret_ip);
			return;
		}
	}
}

#define DEFINE_HWASAN_LOAD_STORE(size)					\
	void __hwasan_load##size##_noabort(unsigned long addr)		\
	{								\
		check_memory_region(addr, size, false, _RET_IP_);	\
	}								\
	EXPORT_SYMBOL(__hwasan_load##size##_noabort);			\
	void __hwasan_store##size##_noabort(unsigned long addr)		\
	{								\
		check_memory_region(addr, size, true, _RET_IP_);	\
	}								\
	EXPORT_SYMBOL(__hwasan_store##size##_noabort)

DEFINE_HWASAN_LOAD_STORE(1);
DEFINE_HWASAN_LOAD_STORE(2);
DEFINE_HWASAN_LOAD_STORE(4);
DEFINE_HWASAN_LOAD_STORE(8);
DEFINE_HWASAN_LOAD_STORE(16);

void __hwasan_loadN_noabort(unsigned long addr, unsigned long size)
{
	check_memory_region(addr, size, false, _RET_IP_);
}
EXPORT_SYMBOL(__hwasan_loadN_noabort);

void __hwasan_storeN_noabort(unsigned long addr, unsigned long size)
{
	check_memory_region(addr, size, true, _RET_IP_);
}
EXPORT_SYMBOL(__hwasan_storeN_noabort);

void __hwasan_tag_memory(unsigned long addr, u8 tag, unsigned long size)
{
	kasan_poison_shadow((void *)addr, size, tag);
}
EXPORT_SYMBOL(__hwasan_tag_memory);
