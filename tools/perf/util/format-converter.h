#ifndef __PERF_FORMAT_CONVERTER_H
#define __PERF_FORMAT_CONVERTER_H

#include "evlist.h"

struct format_converter {
	int (*init)(struct perf_evlist *evlist, const char *path, void **priv);
	int (*write)(struct perf_evlist *evlist, void *buf, size_t size, void *priv);
	int (*cleanup)(struct perf_evlist *evlist, void *priv);
	void *priv;
};

#ifdef HAVE_LIBBABELTRACE_SUPPORT
extern struct format_converter ctf_format;
#endif

#endif /* __PERF_FORMAT_CONVERTER_H */
