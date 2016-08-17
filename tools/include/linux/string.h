#ifndef _TOOLS_LINUX_STRING_H_
#define _TOOLS_LINUX_STRING_H_


#include <linux/types.h>	/* for size_t */

void *memdup(const void *src, size_t len);

int strtobool(const char *s, bool *res);

/*
 * Ideally instead of blacklisting libs which define this already (uclibc,
 * musl..), we could whitelist libs which reuire it (glibc). However uClibc
 * also defines __GLIBC__ for user application headers
 */
#ifndef __UCLIBC__
extern size_t strlcpy(char *dest, const char *src, size_t size);
#endif

char *str_error_r(int errnum, char *buf, size_t buflen);

#endif /* _LINUX_STRING_H_ */
