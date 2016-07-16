#include "i915_drv.h"

DEFINE_STATIC_KEY_FALSE(has_movntqa);

#ifdef CONFIG_AS_MOVNTDQA
static void __movntqda(void *dst, const void *src, unsigned long len)
{
	len >>= 4;
	while (len >= 4) {
		__asm__ __volatile__(
		"movntdqa (%0), %%xmm0\n"
		"movntdqa 16(%0), %%xmm1\n"
		"movntdqa 32(%0), %%xmm2\n"
		"movntdqa 48(%0), %%xmm3\n"
		"movaps %%xmm0, (%1)\n"
		"movaps %%xmm1, 16(%1)\n"
		"movaps %%xmm2, 32(%1)\n"
		"movaps %%xmm3, 48(%1)\n"
		: : "r" (src), "r" (dst) : "memory");
		src += 64;
		dst += 64;
		len -= 4;
	}
	while (len--) {
		__asm__ __volatile__(
		"movntdqa (%0), %%xmm0\n"
		"movaps %%xmm0, (%1)\n"
		: : "r" (src), "r" (dst) : "memory");
		src += 16;
		dst += 16;
	}
}
#endif

/**
 * i915_memcpy_from_wc: perform an accelerated *aligned* read from WC
 * @dst: destination pointer
 * @src: source pointer
 * @len: how many bytes to copy
 *
 * i915_memcpy_from_wc copies @len bytes from @src to @dst using
 * non-temporal instructions where available. Note that all arguments
 * (@src, @dst) must be aligned to 16 bytes and @len must be a multiple
 * of 16.
 *
 * To test whether accelerated reads from WC are supported, use
 * i915_memcpy_from_wc(NULL, NULL, 0);
 *
 * Returns true if the copy was successful, false if the preconditions
 * are not met.
 */
bool i915_memcpy_from_wc(void *dst, const void *src, unsigned long len)
{
	GEM_BUG_ON((unsigned long)dst & 15);
	GEM_BUG_ON((unsigned long)src & 15);

	if (unlikely(len & 15))
		return false;

#ifdef CONFIG_AS_MOVNTDQA
	if (static_branch_likely(&has_movntqa)) {
		if (len)
			__movntqda(dst, src, len);
		return true;
	}
#endif

	return false;
}

void i915_memcpy_init_early(struct drm_i915_private *dev_priv)
{
	if (static_cpu_has(X86_FEATURE_XMM4_1))
		static_branch_enable(&has_movntqa);
}
