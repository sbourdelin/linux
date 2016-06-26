/*
 * Copyright (C) 2016, Wang Nan <wangnan0@huawei.com>
 * Copyright (C) 2016, Huawei Inc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <ubpf.h>

#include "ubpf-helpers.h"

static int __maybe_unused
ubpf_memcmp(void *s1, void *s2, unsigned int n)
{
	return memcmp(s1, s2, n);
}

static void __maybe_unused
ubpf_memcpy(void *d, void *s, unsigned int n)
{
	memcpy(d, s, n);
}

static int __maybe_unused
ubpf_strcmp(char *s1, char *s2)
{
	return strcmp(s1, s2);
}

static int __maybe_unused
ubpf_printf(char *fmt, ...)
{
	int ret;
	va_list ap;

	va_start(ap, fmt);
	ret = vprintf(fmt, ap);
	va_end(ap);

	return ret;
}

static int __maybe_unused
ubpf_map_lookup_elem(int map_fd, void *key, void *value)
{
	return bpf_map_lookup_elem(map_fd, key, value);
}

static int __maybe_unused
ubpf_map_update_elem(int map_fd, void *key, void *value,
				unsigned long long flags)
{
	return bpf_map_update_elem(map_fd, key, value, (u64)flags);
}

static int __maybe_unused
ubpf_map_get_next_key(int map_fd, void *key, void *next_key)
{
	return bpf_map_get_next_key(map_fd, key, next_key);
}

void register_ubpf_helpers(void)
{
#define DEF_UBPF_HELPER(type, name, param)			\
	libbpf_set_ubpf_func(UBPF_FUNC_##name, #name, name);
#include "ubpf-helpers-list.h"
#undef DEF_UBPF_HELPER
}
