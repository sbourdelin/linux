#include <linux/kernel.h>
#include <linux/gcd.h>
#include <linux/export.h>

/*
 * This implements the binary GCD algorithm. (Often attributed to Stein,
 * but as Knuth has noted, appears a first-century Chinese math text.)
 *
 * This is faster than the division-based algorithm even on x86, which
 * has decent hardware division.
 */

#if !defined(CONFIG_CPU_NO_EFFICIENT_FFS)

/* If __ffs is available, the even/odd algorithm benchmarks slower. */
unsigned long gcd(unsigned long a, unsigned long b)
{
	unsigned long r = a | b;

	if (!a || !b)
		return r;

	b >>= __ffs(b);

	for (;;) {
		a >>= __ffs(a);
		if (a == b)
			return a << __ffs(r);
		if (a < b)
			swap(a, b);
		a -= b;
	}
}

#else

/* If normalization is done by loops, the even/odd algorithm is a win. */
unsigned long gcd(unsigned long a, unsigned long b)
{
	unsigned long r = a | b;

	if (!a || !b)
		return r;

	/* Isolate lsbit of r */
	r &= -r;

	while (!(a & r))
		a >>= 1;
	while (!(b & r))
		b >>= 1;

	while (a != b) {
		if (a < b)
			swap(a, b);
		a -= b;

		a >>= 1;
		if (a & r)
			a += b;
		do a >>= 1; while (!(a & r));
	}

	return b;
}

#endif

EXPORT_SYMBOL_GPL(gcd);
