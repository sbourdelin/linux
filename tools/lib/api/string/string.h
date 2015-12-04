#ifndef _API_STRING_H
#define _API_STRING_H

#include <stddef.h>
#include <stdbool.h>

void *memdup(const void *src, size_t len);

int strtobool(const char *s, bool *res);

#ifndef __UCLIBC__
/* Matches the libc/libbsd function attribute so we declare this unconditionally: */
extern size_t strlcpy(char *dest, const char *src, size_t size);
#endif

#endif /* _API_STRING_H */
