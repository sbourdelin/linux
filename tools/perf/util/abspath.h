#ifndef __PERF_ABSPATH_H
#define __PERF_ABSPATH_H

static inline int is_absolute_path(const char *path)
{
	return path[0] == '/';
}

const char *make_nonrelative_path(const char *path);

#endif /* __PERF_ABSPATH_H */
