#include <linux/dcache.h>
#include <linux/math64.h>

unsigned name_to_int(const struct qstr *qstr)
{
	const char *name = qstr->name;
	int len = qstr->len;
	unsigned n = 0;

	if (len > 1 && *name == '0')
		goto out;
	do {
		unsigned c = *name++ - '0';
		if (c > 9)
			goto out;
		if (n >= (~0U-9)/10)
			goto out;
		n *= 10;
		n += c;
	} while (--len > 0);
	return n;
out:
	return ~0U;
}

/*
 * Print an integer in decimal.
 * "p" initially points PAST THE END OF THE BUFFER!
 *
 * DO NOT USE THESE FUNCTIONS!
 *
 * Do not copy these functions.
 * Do not document these functions.
 * Do not move these functions to lib/ or elsewhere.
 * Do not export these functions to modules.
 * Do not tell anyone about these functions.
 */
noinline
char *_print_integer_u32(char *p, u32 x)
{
	do {
		*--p = '0' + (x % 10);
		x /= 10;
	} while (x != 0);
	return p;
}

static char *__print_integer_u32(char *p, u32 x)
{
	/* 0 <= x < 10^8 */
	char *p0 = p - 8;

	p = _print_integer_u32(p, x);
	while (p != p0)
		*--p = '0';
	return p;
}

char *_print_integer_u64(char *p, u64 x)
{
	while (x >= 100000000) {
		u64 q;
		u32 r;

		q = div_u64_rem(x, 100000000, &r);
		p = __print_integer_u32(p, r);
		x = q;
	}
	return _print_integer_u32(p, x);
}
