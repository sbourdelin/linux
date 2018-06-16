/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This library provides a set of APIs for user or kernel to use
 * some new instructions:
 * - Director stores: movdiri and movdir64b
 *
 * Detailed information on the instructions can be found in
 * Intel Architecture Instruction Set Extensions and Future Features
 * Programming Reference.
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Author:
 *     Fenghua Yu <fenghua.yu@intel.com>
 */
#ifndef _ASM_X86_LIB_DIRECT_STORES_H
#define _ASM_X86_LIB_DIRECT_STORES_H

#include <stdbool.h>

/* CPUID.07H.0H:ECX[bit 27] */
#define MOVDIRI_BIT		27
/* CPUID.07H.0H:ECX[bit 28] */
#define MOVDIR64B_BIT		28

static bool _movdiri_supported, _movdiri_enumerated;
static bool _movdir64b_supported, _movdir64b_enumerated;

/**
 * movdiri_supported() - Is movdiri instruction supported?
 *
 * Return:
 * true: supported
 *
 * false: not supported
 */
static inline bool movdiri_supported(void)
{
	int eax, ebx, ecx, edx;
	bool ret;

	/*
	 * If movdiri has been enumerated before, return cached movdiri
	 * support info.
	 */
	if (_movdiri_enumerated)
		return _movdiri_supported;

	/* Otherwise, enumerate movdiri from CPUID. */
	asm volatile("mov $7, %%eax\t\n"
		     "mov $0, %%ecx\t\n"
		     "cpuid\t\n"
		     : "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx));

	if (ecx & (1 << MOVDIRI_BIT))
		ret = true;
	else
		ret = false;

	/*
	 * Cache movdiri support info so we can use it later without
	 * calling CPUID.
	 */
	_movdiri_enumerated = true;
	_movdiri_supported = ret;

	return ret;
}

/**
 * movdir64b_supported() - Is movdir64b instruction supported?
 *
 * Return:
 * true: supported
 *
 * false: not supported
 */
static inline bool movdir64b_supported(void)
{
	int eax, ebx, ecx, edx;
	int ret;

	/*
	 * If movdir64b has been enumerated before, return cached movdir64b
	 * support info.
	 */
	if (_movdir64b_enumerated)
		return _movdir64b_supported;

	/* Otherwise, enumerate movdir64b from CPUID. */
	asm volatile("mov $7, %%eax\t\n"
		     "mov $0, %%ecx\t\n"
		     "cpuid\t\n"
		     : "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx));

	if (ecx & (1 << MOVDIR64B_BIT))
		ret = true;
	else
		ret = false;

	/*
	 * Cache movdir64b support info so we can use it later without
	 * calling CPUID.
	 */
	_movdir64b_enumerated = true;
	_movdir64b_supported = ret;

	return ret;
}

/**
 * movdiri32() - Move doubleword using direct store.
 * @dst: Destination address.
 * @data: 32-bit data.
 *
 * Moves the doubleword integer in @data to the destination address @dst
 * using a direct-store operation.
 */
static inline void movdiri32(int *dst, int data)
{
	/* movdiri eax, [rdx] */
	asm volatile(".byte 0x0f, 0x38, 0xf9, 0x02"
		     : "=m" (*dst)
		     : "a" (data), "d" (dst));
}

/**
 * movdiri64() - Move quadword using direct store
 * @dst: Destination address
 * @data: 64-bit data
 *
 * Moves the quadword integer in @data to the destination address @dst
 * using a direct-store operation.
 */
static inline void movdiri64(long *dst, long data)
{
	/* movdiri rax, [rdx] */
	asm volatile(".byte 0x48, 0x0f, 0x38, 0xf9, 0x02"
		     : "=m" (*dst)
		     : "a" (data), "d" (dst));
}

/**
 * movdir64b() - Move 64 bytes using direct store
 * @dst: Destination address
 * @src: Source address
 *
 * Moves 64 bytes as direct store with 64 bytes write atomicity from
 * source memory address @src to destination address @dst.
 *
 * @dst must be 64-byte aligned. No alignment requirement for @src.
 */
static inline void movdir64b(void *dst, void *src)
{
	 /* movdir64b [rax], rdx */
	asm volatile(".byte 0x66, 0x0f, 0x38, 0xf8, 0x02"
		     : "=m" (*dst)
		     : "a" (src), "d" (dst));
}

#endif /* _ASM_X86_LIB_DIRECT_STORES_H */
