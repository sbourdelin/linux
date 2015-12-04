#ifndef __PERF_PATH_H
#define __PERF_PATH_H

char *strip_path_suffix(const char *path, const char *suffix);

extern char *mkpath(const char *fmt, ...) __attribute__((format (printf, 1, 2)));
extern char *perf_path(const char *fmt, ...) __attribute__((format (printf, 1, 2)));

extern char *perf_pathdup(const char *fmt, ...)
	__attribute__((format (printf, 1, 2)));

#ifndef __UCLIBC__
/* Matches the libc/libbsd function attribute so we declare this unconditionally: */
extern size_t strlcpy(char *dest, const char *src, size_t size);
#endif

#endif /* __PERF_PATH_H */
