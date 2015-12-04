#ifndef __API_UTIL_WRAPPER_H
#define __API_UTIL_WRAPPER_H

extern char *xstrdup(const char *str);
extern void *xrealloc(void *ptr, size_t size) __attribute__((weak));

static inline void *zalloc(size_t size)
{
	return calloc(1, size);
}

#define zfree(ptr) ({ free(*ptr); *ptr = NULL; })

#define alloc_nr(x) (((x)+16)*3/2)

/*
 * Realloc the buffer pointed at by variable 'x' so that it can hold
 * at least 'nr' entries; the number of entries currently allocated
 * is 'alloc', using the standard growing factor alloc_nr() macro.
 *
 * DO NOT USE any expression with side-effect for 'x' or 'alloc'.
 */
#define ALLOC_GROW(x, nr, alloc) \
	do { \
		if ((nr) > alloc) { \
			if (alloc_nr(alloc) < (nr)) \
				alloc = (nr); \
			else \
				alloc = alloc_nr(alloc); \
			x = xrealloc((x), alloc * sizeof(*(x))); \
		} \
	} while(0)

#endif /* __API_UTIL_WRAPPER_H */
