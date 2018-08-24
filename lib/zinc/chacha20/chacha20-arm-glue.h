/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2015-2018 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include <zinc/chacha20.h>

asmlinkage void chacha20_arm(u8 *out, const u8 *in, const size_t len, const u32 key[8], const u32 counter[4]);
#if IS_ENABLED(CONFIG_KERNEL_MODE_NEON) && (!defined(__LINUX_ARM_ARCH__) || __LINUX_ARM_ARCH__ >= 7)
#define ARM_USE_NEON
#include <asm/hwcap.h>
#include <asm/neon.h>
asmlinkage void chacha20_neon(u8 *out, const u8 *in, const size_t len, const u32 key[8], const u32 counter[4]);
#endif

static bool chacha20_use_neon __ro_after_init;

void __init chacha20_fpu_init(void)
{
#if defined(CONFIG_ARM64)
	chacha20_use_neon = elf_hwcap & HWCAP_ASIMD;
#elif defined(CONFIG_ARM)
	chacha20_use_neon = elf_hwcap & HWCAP_NEON;
#endif
}

static inline bool chacha20_arch(u8 *dst, const u8 *src, const size_t len, const u32 key[8], const u32 counter[4], simd_context_t simd_context)
{
	if (simd_context != HAVE_FULL_SIMD
#if defined(ARM_USE_NEON)
		|| !chacha20_use_neon
#endif
	)
		chacha20_arm(dst, src, len, key, counter);
	else
		chacha20_neon(dst, src, len, key, counter);
	return true;
}

static inline bool hchacha20_arch(u8 *derived_key, const u8 *nonce, const u8 *key, simd_context_t simd_context) { return false; }

#define HAVE_CHACHA20_ARCH_IMPLEMENTATION
