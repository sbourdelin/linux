#ifndef __PERF_PATH_H
#define __PERF_PATH_H

char *strip_path_suffix(const char *path, const char *suffix);

extern char *mkpath(const char *fmt, ...) __attribute__((format (printf, 1, 2)));
extern char *perf_path(const char *fmt, ...) __attribute__((format (printf, 1, 2)));

extern char *perf_pathdup(const char *fmt, ...)
	__attribute__((format (printf, 1, 2)));

#endif /* __PERF_PATH_H */
