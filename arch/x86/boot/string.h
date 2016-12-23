#ifndef BOOT_STRING_H
#define BOOT_STRING_H

/* Undef any of these macros coming from string_32.h. */
#undef memcpy
#undef memset
#undef memcmp

void *memcpy(void *dst, const void *src, size_t len);
void *memset(void *dst, int c, size_t len);
int memcmp(const void *s1, const void *s2, size_t len);

/*
 * Access builtin version by default. If one needs to use optimized version,
 * do "undef memcpy" in .c file and link against right string.c
 */
#define memcpy(d,s,l) __builtin_memcpy(d,s,l)
#define memset(d,c,l) __builtin_memset(d,c,l)
#define memcmp	__builtin_memcmp

int strcmp(const char *str1, const char *str2);
int strncmp(const char *cs, const char *ct, size_t count);
size_t strlen(const char *s);
char *strstr(const char *s1, const char *s2);
size_t strnlen(const char *s, size_t maxlen);
unsigned int atou(const char *s);
unsigned long long simple_strtoull(const char *cp,
				    char **endp, unsigned int base);

#endif /* BOOT_STRING_H */
