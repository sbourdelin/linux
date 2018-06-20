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

void *khwasan_set_tag(const void *addr, u8 tag)
{
	return set_tag(addr, tag);
}

u8 khwasan_get_tag(const void *addr)
{
	return get_tag(addr);
}

void *khwasan_reset_tag(const void *addr)
{
	return reset_tag(addr);
}

void check_memory_region(unsigned long addr, size_t size, bool write,
				unsigned long ret_ip)
{
}

#define DEFINE_HWASAN_LOAD_STORE(size)					\
	void __hwasan_load##size##_noabort(unsigned long addr)		\
	{								\
	}								\
	EXPORT_SYMBOL(__hwasan_load##size##_noabort);			\
	void __hwasan_store##size##_noabort(unsigned long addr)		\
	{								\
	}								\
	EXPORT_SYMBOL(__hwasan_store##size##_noabort)

DEFINE_HWASAN_LOAD_STORE(1);
DEFINE_HWASAN_LOAD_STORE(2);
DEFINE_HWASAN_LOAD_STORE(4);
DEFINE_HWASAN_LOAD_STORE(8);
DEFINE_HWASAN_LOAD_STORE(16);

void __hwasan_loadN_noabort(unsigned long addr, unsigned long size)
{
}
EXPORT_SYMBOL(__hwasan_loadN_noabort);

void __hwasan_storeN_noabort(unsigned long addr, unsigned long size)
{
}
EXPORT_SYMBOL(__hwasan_storeN_noabort);

void __hwasan_tag_memory(unsigned long addr, u8 tag, unsigned long size)
{
}
EXPORT_SYMBOL(__hwasan_tag_memory);
