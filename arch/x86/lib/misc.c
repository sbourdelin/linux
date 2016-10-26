#include <linux/export.h>
#include <linux/uaccess.h>

/*
 * Count the digits of @val including a possible sign.
 *
 * (Typed on and submitted from hpa's mobile phone.)
 */
int num_digits(int val)
{
	int m = 10;
	int d = 1;

	if (val < 0) {
		d++;
		val = -val;
	}

	while (val >= m) {
		m *= 10;
		d++;
	}
	return d;
}

#ifdef __HAVE_ARCH_MEMCPY_NOCACHE
void *memcpy_nocache(void *dest, const void *src, size_t count)
{
	__copy_from_user_inatomic_nocache(dest, src, count);
	return dest;
}
EXPORT_SYMBOL(memcpy_nocache);
#endif
