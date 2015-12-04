#ifndef __PERF_WRAPPER_H
#define __PERF_WRAPPER_H

extern char *xstrdup(const char *str);
extern void *xrealloc(void *ptr, size_t size) __attribute__((weak));

static inline void *zalloc(size_t size)
{
	return calloc(1, size);
}

#define zfree(ptr) ({ free(*ptr); *ptr = NULL; })

#endif /* __PERF_WRAPPER_H */
