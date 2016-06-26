/*
 * Copyright (C) 2016, Wang Nan <wangnan0@huawei.com>
 * Copyright (C) 2016, Huawei Inc.
 */
#ifndef __UBPF_HELPERS_H
#define __UBPF_HELPERS_H

#define DEF_UBPF_HELPER(type, name, param) UBPF_FUNC_##name,
enum {
	UBPF_FUNC_unspec = 0,
#include "ubpf-helpers-list.h"
	UBPF_FUNC_MAX
};
#undef DEF_UBPF_HELPER

#ifdef HAVE_UBPF_SUPPORT
void register_ubpf_helpers(void);
#else
static inline void register_ubpf_helpers(void) {};
#endif
#endif
