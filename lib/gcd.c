#include <linux/kernel.h>
#include <linux/gcd.h>
#include <linux/export.h>

/*
 * use __ffs if the CPU has efficient __ffs
 */
#if (defined(CONFIG_ALPHA) && defined(CONFIG_ALPHA_EV6) && defined(CONFIG_ALPHA_EV67)) || \
	defined(CONFIG_ARC) || \
	(defined(CONFIG_ARM) && __LINUX_ARM_ARCH__ >= 5) || defined(CONFIG_ARM64) || \
	defined(CONFIG_AVR32) || \
	defined(CONFIG_BLACKFIN) || \
	defined(CONFIG_C6X) || \
	defined(CONFIG_CRIS) || \
	defined(CONFIG_FRV) || \
	defined(CONFIG_HEXAGON) || \
	defined(CONFIG_IA64) || \
	(defined(CONFIG_M68K) && \
	 (!defined(CONFIG_CPU_HAS_NO_BITFIELDS) || \
	  ((defined(__mcfisaaplus__) || defined(__mcfisac__)) && \
	   !defined(CONFIG_M68000) && !defined(CONFIG_MCPU32)))) || \
	defined(CONFIG_MN10300) || \
	defined(CONFIG_OPENRISC) || \
	defined(CONFIG_POWERPC) || \
	defined(CONFIG_S390) || \
	defined(CONFIG_TILE) || \
	defined(CONFIG_UNICORE32) || \
	defined(CONFIG_X86) || \
	defined(CONFIG_XTENSA)
# define USE_FFS 1
#elif defined(CONFIG_MIPS)
# define USE_FFS (__builtin_constant_p(cpu_has_clo_clz) && cpu_has_clo_clz)
#else
# define USE_FFS 0
#endif

/*
 * This implements the binary GCD algorithm. (Often attributed to Stein,
 * but as Knith has noted, appears a first-century Chinese math text.)
 */
unsigned long gcd(unsigned long a, unsigned long b)
{
	unsigned long r = a | b;

	if (!a || !b)
		return r;

	if (USE_FFS) {
		b >>= __ffs(b);
	} else {
		/* least-significant mask, equ to "(1UL << ffs(r)) - 1" */
		r ^= (r - 1);

		while (!(b & r))
			b >>= 1;
	}

	for (;;) {
		if (USE_FFS) {
			a >>= __ffs(a);
		} else {
			while (!(a & r))
				a >>= 1;
		}
		if (a == b)
			break;

		if (a < b)
			swap(a, b);

		a -= b;
		if (!USE_FFS) {
			a >>= 1;
			if (a & r)
				a += b;
		}
	}

	if (USE_FFS) {
		b <<= __ffs(r);
	}
	return b;
}
EXPORT_SYMBOL_GPL(gcd);
